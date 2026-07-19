#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ui {

// Roboto, rasterised from the bundled TTF at startup.
//
// Glyphs are baked at a fixed set of pixel sizes rather than at one size and
// scaled: scaling a rasterised glyph blurs it, and text is the one thing in a
// UI where that reads as broken. Ask for a size and you get the nearest baked
// one, drawn 1:1.
constexpr int kFontSizeSmall = 13;
constexpr int kFontSizeBody = 17;
constexpr int kFontSizeHeading = 26;
constexpr int kFontSizeTitle = 34;

// One rasterised character: where it sits in the atlas, and how to place it.
struct Glyph {
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    float offsetX = 0.0f;  // from the pen position to the bitmap's left edge
    float offsetY = 0.0f;  // from the baseline to the bitmap's top edge
    float advance = 0.0f;  // how far the pen moves for the next character
    float width = 0.0f;
    float height = 0.0f;
};

class Font {
public:
    // Rasterises every printable ASCII character at each requested size into a
    // single atlas. Returns false if they don't fit.
    bool build(const uint8_t* ttfData, size_t ttfSize, const std::vector<int>& pixelSizes);

    // Nearest baked size to `requested`, so callers can ask for anything.
    int nearestSize(int requested) const;

    const Glyph* glyph(int pixelSize, char c) const;

    float textWidth(int pixelSize, const std::string& text) const;
    // Distance from the top of a line to the baseline.
    float ascent(int pixelSize) const;
    float lineHeight(int pixelSize) const;

    // RGBA8, white with coverage in alpha so vertex colour can tint it.
    const std::vector<uint8_t>& pixels() const { return mPixels; }
    int atlasWidth() const { return mAtlasWidth; }
    int atlasHeight() const { return mAtlasHeight; }

    // A fully opaque texel, used by rect() so the whole UI stays one batch.
    float whiteU() const { return mWhiteU; }
    float whiteV() const { return mWhiteV; }

private:
    struct SizedFont {
        std::map<char, Glyph> glyphs;
        float ascent = 0.0f;
        float lineHeight = 0.0f;
    };

    std::map<int, SizedFont> mSizes;
    std::vector<uint8_t> mPixels;
    int mAtlasWidth = 0;
    int mAtlasHeight = 0;
    float mWhiteU = 0.0f;
    float mWhiteV = 0.0f;
};

}  // namespace ui
