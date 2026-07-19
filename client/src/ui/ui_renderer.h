#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/Scene.h>
#include <filament/Texture.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <utils/Entity.h>

#include "ui/font.h"

namespace ui {

// RGBA, 0-255. Kept byte-sized because that's what the vertex format uses.
struct Color {
    uint8_t r = 255, g = 255, b = 255, a = 255;

    static constexpr Color rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        return Color{r, g, b, a};
    }
};

// Draws the interface as a single batch of textured quads into an overlay view
// composited on top of the 3D scene.
//
// Coordinates are pixels with the origin at the top-left, which is what layout
// code wants to think in; the orthographic camera maps that to clip space.
class UiRenderer {
public:
    bool init(filament::Engine* engine);
    void destroy(filament::Engine* engine);

    // Call when the window resizes.
    void setViewport(int width, int height);

    // Clears the frame's geometry. Everything below appends to it.
    void begin();

    void rect(float x, float y, float width, float height, Color color);
    // Outline drawn as four thin rects, inset within the given bounds.
    void rectOutline(float x, float y, float width, float height, float thickness, Color color);
    // Draws with `y` as the top of the line. `pixelSize` snaps to the nearest
    // baked font size; see ui/font.h.
    void text(float x, float y, const std::string& value, Color color,
            int pixelSize = kFontSizeBody);

    // Uploads the batch and points the renderable at it.
    void end(filament::Engine* engine);

    // Not static any more: real glyph widths come from the font's metrics.
    float textWidth(const std::string& value, int pixelSize) const;
    float textHeight(int pixelSize) const;
    const Font& font() const { return mFont; }

    filament::View* view() const { return mView; }
    int width() const { return mWidth; }
    int height() const { return mHeight; }

private:
    struct Vertex {
        float x, y;
        float u, v;
        uint8_t r, g, b, a;
    };

    void quad(float x, float y, float width, float height, float u0, float v0, float u1, float v1,
            Color color);

    Font mFont;
    filament::Material* mMaterial = nullptr;
    filament::MaterialInstance* mMaterialInstance = nullptr;
    filament::Texture* mAtlas = nullptr;
    filament::VertexBuffer* mVertexBuffer = nullptr;
    filament::IndexBuffer* mIndexBuffer = nullptr;
    filament::View* mView = nullptr;
    filament::Scene* mScene = nullptr;
    filament::Camera* mCamera = nullptr;
    utils::Entity mCameraEntity;
    utils::Entity mRenderable;

    std::vector<Vertex> mVertices;
    std::vector<uint16_t> mIndices;
    int mWidth = 0;
    int mHeight = 0;
    // Re-pointing the renderable's geometry recreates a driver handle, so the
    // draw range is bucketed and only updated when the bucket changes.
    size_t mLastDrawCount = SIZE_MAX;
};

}  // namespace ui
