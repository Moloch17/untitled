#include "ui/ui_renderer.h"

#include <cstring>

#include <filament/Camera.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/TextureSampler.h>
#include <filament/Viewport.h>
#include <utils/EntityManager.h>

#include "clientmaterials.h"
#include "ui/font.h"

using namespace filament;

namespace ui {

namespace {

// The atlas holds 16x6 glyph cells plus one extra row whose first cell is
// solid white, so untextured rectangles can share the same texture and the
// entire UI draws in a single batch.
constexpr int kAtlasCols = 16;
constexpr int kGlyphRows = 6;  // 96 cells, 95 glyphs
constexpr int kAtlasRows = kGlyphRows + 1;
constexpr int kAtlasWidth = kAtlasCols * kGlyphWidth;    // 128
constexpr int kAtlasHeight = kAtlasRows * kGlyphHeight;  // 56

// Centre of the white cell, so bilinear or edge sampling can't bleed into a
// neighbouring glyph.
constexpr float kWhiteU = (0.5f * kGlyphWidth) / kAtlasWidth;
constexpr float kWhiteV = ((kGlyphRows + 0.5f) * kGlyphHeight) / kAtlasHeight;

// Enough for a dense screen of text; each glyph is one quad.
constexpr size_t kMaxQuads = 8192;
constexpr size_t kMaxVertices = kMaxQuads * 4;
constexpr size_t kMaxIndices = kMaxQuads * 6;

// Draw ranges are rounded up to whole buckets of this many indices (64 quads).
// RenderableManager::setGeometryAt destroys and recreates a driver handle on
// every call, and Filament defers those destructions -- calling it per frame
// fills the handle arena within seconds. Bucketing means it is only called when
// the UI's complexity changes materially, e.g. switching screens.
constexpr size_t kIndexBucket = 384;

}  // namespace

bool UiRenderer::init(Engine* engine) {
    mMaterial = Material::Builder()
                        .package(CLIENTMATERIALS_UI_DATA, CLIENTMATERIALS_UI_SIZE)
                        .build(*engine);
    mMaterialInstance = mMaterial->createInstance();

    // Build the atlas: white pixels with alpha from the glyph bitmap, so the
    // vertex colour can tint text to anything.
    auto* pixels = new uint8_t[kAtlasWidth * kAtlasHeight * 4];
    std::memset(pixels, 0, static_cast<size_t>(kAtlasWidth) * kAtlasHeight * 4);

    for (int glyph = 0; glyph < kGlyphCount; ++glyph) {
        const uint8_t* rows = kFontData[glyph];
        const int cellX = (glyph % kAtlasCols) * kGlyphWidth;
        const int cellY = (glyph / kAtlasCols) * kGlyphHeight;
        for (int row = 0; row < kGlyphHeight; ++row) {
            for (int column = 0; column < kGlyphWidth; ++column) {
                const bool on = (rows[row] & (0x80u >> column)) != 0;
                if (!on) {
                    continue;
                }
                const size_t index =
                        (static_cast<size_t>(cellY + row) * kAtlasWidth + cellX + column) * 4;
                pixels[index + 0] = 255;
                pixels[index + 1] = 255;
                pixels[index + 2] = 255;
                pixels[index + 3] = 255;
            }
        }
    }

    // The solid-white cell used by rect().
    for (int row = 0; row < kGlyphHeight; ++row) {
        for (int column = 0; column < kGlyphWidth; ++column) {
            const size_t index =
                    (static_cast<size_t>(kGlyphRows * kGlyphHeight + row) * kAtlasWidth + column)
                    * 4;
            pixels[index + 0] = 255;
            pixels[index + 1] = 255;
            pixels[index + 2] = 255;
            pixels[index + 3] = 255;
        }
    }

    mAtlas = Texture::Builder()
                     .width(kAtlasWidth)
                     .height(kAtlasHeight)
                     .levels(1)
                     .format(Texture::InternalFormat::RGBA8)
                     .sampler(Texture::Sampler::SAMPLER_2D)
                     .build(*engine);

    Texture::PixelBufferDescriptor buffer(pixels,
            static_cast<size_t>(kAtlasWidth) * kAtlasHeight * 4, Texture::Format::RGBA,
            Texture::Type::UBYTE, [](void* data, size_t, void*) {
                delete[] static_cast<uint8_t*>(data);
            });
    mAtlas->setImage(*engine, 0, std::move(buffer));

    // Nearest filtering: this is a pixel font, and blurring it would defeat the
    // point.
    const TextureSampler sampler(TextureSampler::MinFilter::NEAREST,
            TextureSampler::MagFilter::NEAREST);
    mMaterialInstance->setParameter("atlas", mAtlas, sampler);

    mVertexBuffer = VertexBuffer::Builder()
                            .vertexCount(kMaxVertices)
                            .bufferCount(1)
                            .attribute(VertexAttribute::POSITION, 0,
                                    VertexBuffer::AttributeType::FLOAT2, offsetof(Vertex, x),
                                    sizeof(Vertex))
                            .attribute(VertexAttribute::UV0, 0,
                                    VertexBuffer::AttributeType::FLOAT2, offsetof(Vertex, u),
                                    sizeof(Vertex))
                            .attribute(VertexAttribute::COLOR, 0,
                                    VertexBuffer::AttributeType::UBYTE4, offsetof(Vertex, r),
                                    sizeof(Vertex))
                            .normalized(VertexAttribute::COLOR)
                            .build(*engine);

    mIndexBuffer = IndexBuffer::Builder()
                           .indexCount(kMaxIndices)
                           .bufferType(IndexBuffer::IndexType::USHORT)
                           .build(*engine);

    mScene = engine->createScene();
    mView = engine->createView();
    mView->setScene(mScene);
    // Composite over the 3D view rather than replacing it.
    mView->setBlendMode(View::BlendMode::TRANSLUCENT);
    mView->setPostProcessingEnabled(false);
    mView->setShadowingEnabled(false);

    mCameraEntity = utils::EntityManager::get().create();
    mCamera = engine->createCamera(mCameraEntity);
    mView->setCamera(mCamera);

    mRenderable = utils::EntityManager::get().create();
    RenderableManager::Builder(1)
            .boundingBox({{0, 0, 0}, {10000, 10000, 1}})
            // The UI is always on screen; skip the culling test entirely.
            .culling(false)
            .castShadows(false)
            .receiveShadows(false)
            .material(0, mMaterialInstance)
            .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, mVertexBuffer,
                    mIndexBuffer, 0, 0)
            .build(*engine, mRenderable);
    mScene->addEntity(mRenderable);

    return true;
}

void UiRenderer::setViewport(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    mWidth = width;
    mHeight = height;
    mView->setViewport({0, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
    // Origin at the top-left, y increasing downwards -- the convention layout
    // code is written in.
    mCamera->setProjection(Camera::Projection::ORTHO, 0.0, static_cast<double>(width),
            static_cast<double>(height), 0.0, 0.0, 1.0);
}

void UiRenderer::begin() {
    mVertices.clear();
    mIndices.clear();
}

void UiRenderer::quad(float x, float y, float width, float height, float u0, float v0, float u1,
        float v1, Color color) {
    if (mVertices.size() + 4 > kMaxVertices) {
        return;  // silently drop rather than overrun the GPU buffer
    }

    // The sampler's V axis runs bottom-up while the atlas is uploaded top row
    // first, so V is mirrored here. Doing it per quad rather than globally in
    // the shader keeps the flip inside each cell -- a global 1-v would also
    // reverse which cell of the atlas is selected.
    v0 = 1.0f - v0;
    v1 = 1.0f - v1;

    const auto base = static_cast<uint16_t>(mVertices.size());
    mVertices.push_back({x, y, u0, v0, color.r, color.g, color.b, color.a});
    mVertices.push_back({x + width, y, u1, v0, color.r, color.g, color.b, color.a});
    mVertices.push_back({x + width, y + height, u1, v1, color.r, color.g, color.b, color.a});
    mVertices.push_back({x, y + height, u0, v1, color.r, color.g, color.b, color.a});

    for (uint16_t offset : {0, 1, 2, 2, 3, 0}) {
        mIndices.push_back(static_cast<uint16_t>(base + offset));
    }
}

void UiRenderer::rect(float x, float y, float width, float height, Color color) {
    quad(x, y, width, height, kWhiteU, kWhiteV, kWhiteU, kWhiteV, color);
}

void UiRenderer::rectOutline(float x, float y, float width, float height, float thickness,
        Color color) {
    rect(x, y, width, thickness, color);                            // top
    rect(x, y + height - thickness, width, thickness, color);       // bottom
    rect(x, y, thickness, height, color);                           // left
    rect(x + width - thickness, y, thickness, height, color);       // right
}

void UiRenderer::text(float x, float y, const std::string& value, Color color, float scale) {
    const float advance = kGlyphWidth * scale;
    float penX = x;

    for (char c : value) {
        if (c == ' ') {
            penX += advance;
            continue;
        }

        const int index = (c < kFirstGlyph || c > kLastGlyph) ? ('?' - kFirstGlyph)
                                                              : (c - kFirstGlyph);
        const int cellX = index % kAtlasCols;
        const int cellY = index / kAtlasCols;

        const float u0 = static_cast<float>(cellX * kGlyphWidth) / kAtlasWidth;
        const float v0 = static_cast<float>(cellY * kGlyphHeight) / kAtlasHeight;
        const float u1 = static_cast<float>((cellX + 1) * kGlyphWidth) / kAtlasWidth;
        const float v1 = static_cast<float>((cellY + 1) * kGlyphHeight) / kAtlasHeight;

        quad(penX, y, kGlyphWidth * scale, kGlyphHeight * scale, u0, v0, u1, v1, color);
        penX += advance;
    }
}

float UiRenderer::textWidth(const std::string& value, float scale) {
    return static_cast<float>(value.size()) * kGlyphWidth * scale;
}

float UiRenderer::textHeight(float scale) {
    return kGlyphHeight * scale;
}

void UiRenderer::end(Engine* engine) {
    auto& renderableManager = engine->getRenderableManager();
    const auto instance = renderableManager.getInstance(mRenderable);

    if (mIndices.empty()) {
        // Nothing to draw this frame: an empty range is cheaper than leaving
        // stale geometry on screen.
        if (mLastDrawCount != 0) {
            renderableManager.setGeometryAt(instance, 0,
                    RenderableManager::PrimitiveType::TRIANGLES, mVertexBuffer, mIndexBuffer, 0,
                    0);
            mLastDrawCount = 0;
        }
        return;
    }

    // Pad up to the bucket with triangles that reference vertex 0 three times.
    // They have zero area, so they cost a little vertex work and rasterise
    // nothing.
    size_t drawCount = ((mIndices.size() + kIndexBucket - 1) / kIndexBucket) * kIndexBucket;
    if (drawCount > kMaxIndices) {
        drawCount = kMaxIndices;
    }
    mIndices.resize(drawCount, 0);

    // Copies are handed to Filament and freed from the upload callback, since
    // the GPU may read them after this function returns.
    auto* vertexData = new std::vector<Vertex>(mVertices);
    mVertexBuffer->setBufferAt(*engine, 0,
            VertexBuffer::BufferDescriptor(vertexData->data(),
                    vertexData->size() * sizeof(Vertex),
                    [](void*, size_t, void* user) {
                        delete static_cast<std::vector<Vertex>*>(user);
                    },
                    vertexData));

    auto* indexData = new std::vector<uint16_t>(mIndices);
    mIndexBuffer->setBuffer(*engine,
            IndexBuffer::BufferDescriptor(indexData->data(), indexData->size() * sizeof(uint16_t),
                    [](void*, size_t, void* user) {
                        delete static_cast<std::vector<uint16_t>*>(user);
                    },
                    indexData));

    if (drawCount != mLastDrawCount) {
        renderableManager.setGeometryAt(instance, 0, RenderableManager::PrimitiveType::TRIANGLES,
                mVertexBuffer, mIndexBuffer, 0, drawCount);
        mLastDrawCount = drawCount;
    }
}

void UiRenderer::destroy(Engine* engine) {
    auto& entityManager = utils::EntityManager::get();

    mScene->remove(mRenderable);
    engine->destroy(mRenderable);
    entityManager.destroy(mRenderable);

    engine->destroyCameraComponent(mCameraEntity);
    entityManager.destroy(mCameraEntity);

    engine->destroy(mView);
    engine->destroy(mScene);
    engine->destroy(mVertexBuffer);
    engine->destroy(mIndexBuffer);
    engine->destroy(mAtlas);
    engine->destroy(mMaterialInstance);
    engine->destroy(mMaterial);
}

}  // namespace ui
