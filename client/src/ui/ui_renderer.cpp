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

    // Rasterise Roboto at the sizes the interface uses. The TTF is embedded in
    // the executable alongside the shaders; see client/CMakeLists.txt.
    if (!mFont.build(CLIENTMATERIALS_ROBOTO_MEDIUM_DATA, CLIENTMATERIALS_ROBOTO_MEDIUM_SIZE,
                {kFontSizeSmall, kFontSizeBody, kFontSizeHeading, kFontSizeTitle})) {
        return false;
    }
    mAtlas = Texture::Builder()
                     .width(static_cast<uint32_t>(mFont.atlasWidth()))
                     .height(static_cast<uint32_t>(mFont.atlasHeight()))
                     .levels(1)
                     .format(Texture::InternalFormat::RGBA8)
                     .sampler(Texture::Sampler::SAMPLER_2D)
                     .build(*engine);

    auto* pixels = new std::vector<uint8_t>(mFont.pixels());
    Texture::PixelBufferDescriptor buffer(pixels->data(), pixels->size(), Texture::Format::RGBA,
            Texture::Type::UBYTE, [](void*, size_t, void* user) {
                delete static_cast<std::vector<uint8_t>*>(user);
            }, pixels);
    mAtlas->setImage(*engine, 0, std::move(buffer));

    // Linear filtering: glyphs are rasterised at the size they're drawn, and
    // linear keeps their antialiased edges smooth.
    const TextureSampler sampler(TextureSampler::MinFilter::LINEAR,
            TextureSampler::MagFilter::LINEAR);
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
    const float u = mFont.whiteU();
    const float v = mFont.whiteV();
    quad(x, y, width, height, u, v, u, v, color);
}

void UiRenderer::rectOutline(float x, float y, float width, float height, float thickness,
        Color color) {
    rect(x, y, width, thickness, color);                            // top
    rect(x, y + height - thickness, width, thickness, color);       // bottom
    rect(x, y, thickness, height, color);                           // left
    rect(x + width - thickness, y, thickness, height, color);       // right
}

void UiRenderer::text(float x, float y, const std::string& value, Color color, int pixelSize) {
    const int size = mFont.nearestSize(pixelSize);
    // `y` is the top of the line; glyph offsets are relative to the baseline.
    const float baseline = y + mFont.ascent(size);
    float penX = x;

    for (char c : value) {
        const Glyph* glyph = mFont.glyph(size, c);
        if (!glyph) {
            continue;
        }
        if (glyph->width > 0.0f && glyph->height > 0.0f) {
            quad(penX + glyph->offsetX, baseline + glyph->offsetY, glyph->width, glyph->height,
                    glyph->u0, glyph->v0, glyph->u1, glyph->v1, color);
        }
        penX += glyph->advance;
    }
}

float UiRenderer::textWidth(const std::string& value, int pixelSize) const {
    return mFont.textWidth(pixelSize, value);
}

float UiRenderer::textHeight(int pixelSize) const {
    return mFont.lineHeight(pixelSize);
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
