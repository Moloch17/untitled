#include "ui/font.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace ui {

namespace {

constexpr char kFirstGlyph = 0x20;
constexpr char kLastGlyph = 0x7E;

// Big enough for every printable character at all four sizes with room to
// spare. build() fails loudly rather than silently dropping glyphs if a future
// size doesn't fit.
constexpr int kAtlasSize = 1024;

// Keeps bilinear sampling from bleeding between neighbouring glyphs.
constexpr int kPadding = 1;

}  // namespace

bool Font::build(const uint8_t* ttfData, size_t ttfSize, const std::vector<int>& pixelSizes) {
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttfData, stbtt_GetFontOffsetForIndex(ttfData, 0))) {
        return false;
    }
    (void) ttfSize;

    mAtlasWidth = kAtlasSize;
    mAtlasHeight = kAtlasSize;
    mPixels.assign(static_cast<size_t>(mAtlasWidth) * mAtlasHeight * 4, 0);

    // Shelf packing: fill a row left to right, drop to a new row when full.
    int penX = kPadding;
    int penY = kPadding;
    int rowHeight = 0;

    // One opaque texel first, for solid rectangles.
    {
        const size_t index = (static_cast<size_t>(penY) * mAtlasWidth + penX) * 4;
        mPixels[index + 0] = 255;
        mPixels[index + 1] = 255;
        mPixels[index + 2] = 255;
        mPixels[index + 3] = 255;
        mWhiteU = (penX + 0.5f) / static_cast<float>(mAtlasWidth);
        mWhiteV = (penY + 0.5f) / static_cast<float>(mAtlasHeight);
        penX += 1 + kPadding * 2;
        rowHeight = 1;
    }

    for (int pixelSize : pixelSizes) {
        const float scale = stbtt_ScaleForPixelHeight(&info, static_cast<float>(pixelSize));

        int ascent = 0;
        int descent = 0;
        int lineGap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);

        SizedFont sized;
        sized.ascent = ascent * scale;
        sized.lineHeight = (ascent - descent + lineGap) * scale;

        for (char c = kFirstGlyph; c <= kLastGlyph; ++c) {
            int advance = 0;
            int leftBearing = 0;
            stbtt_GetCodepointHMetrics(&info, c, &advance, &leftBearing);

            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetCodepointBitmapBox(&info, c, scale, scale, &x0, &y0, &x1, &y1);

            const int width = x1 - x0;
            const int height = y1 - y0;

            Glyph glyph;
            glyph.advance = advance * scale;
            glyph.offsetX = static_cast<float>(x0);
            glyph.offsetY = static_cast<float>(y0);
            glyph.width = static_cast<float>(width);
            glyph.height = static_cast<float>(height);

            if (width > 0 && height > 0) {
                if (penX + width + kPadding > mAtlasWidth) {
                    penX = kPadding;
                    penY += rowHeight + kPadding * 2;
                    rowHeight = 0;
                }
                if (penY + height + kPadding > mAtlasHeight) {
                    return false;  // out of room; raise kAtlasSize
                }

                // Rasterise coverage into a scratch buffer, then expand it to
                // white RGB with coverage in alpha.
                std::vector<uint8_t> coverage(static_cast<size_t>(width) * height);
                stbtt_MakeCodepointBitmap(&info, coverage.data(), width, height, width, scale,
                        scale, c);

                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        const size_t destination =
                                (static_cast<size_t>(penY + y) * mAtlasWidth + penX + x) * 4;
                        mPixels[destination + 0] = 255;
                        mPixels[destination + 1] = 255;
                        mPixels[destination + 2] = 255;
                        mPixels[destination + 3] = coverage[static_cast<size_t>(y) * width + x];
                    }
                }

                glyph.u0 = static_cast<float>(penX) / mAtlasWidth;
                glyph.v0 = static_cast<float>(penY) / mAtlasHeight;
                glyph.u1 = static_cast<float>(penX + width) / mAtlasWidth;
                glyph.v1 = static_cast<float>(penY + height) / mAtlasHeight;

                penX += width + kPadding * 2;
                rowHeight = std::max(rowHeight, height);
            }

            sized.glyphs[c] = glyph;
        }

        mSizes[pixelSize] = std::move(sized);
    }

    return true;
}

int Font::nearestSize(int requested) const {
    int best = 0;
    int bestDistance = 1 << 30;
    for (const auto& [size, sized] : mSizes) {
        const int distance = std::abs(size - requested);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = size;
        }
    }
    return best;
}

const Glyph* Font::glyph(int pixelSize, char c) const {
    auto sized = mSizes.find(pixelSize);
    if (sized == mSizes.end()) {
        return nullptr;
    }
    auto glyph = sized->second.glyphs.find(c);
    if (glyph == sized->second.glyphs.end()) {
        // Anything outside the baked range draws as '?' rather than nothing, so
        // it's visible that a character is missing.
        glyph = sized->second.glyphs.find('?');
        if (glyph == sized->second.glyphs.end()) {
            return nullptr;
        }
    }
    return &glyph->second;
}

float Font::textWidth(int pixelSize, const std::string& text) const {
    const int size = nearestSize(pixelSize);
    float width = 0.0f;
    for (char c : text) {
        if (const Glyph* g = glyph(size, c)) {
            width += g->advance;
        }
    }
    return width;
}

float Font::ascent(int pixelSize) const {
    auto sized = mSizes.find(nearestSize(pixelSize));
    return sized == mSizes.end() ? 0.0f : sized->second.ascent;
}

float Font::lineHeight(int pixelSize) const {
    auto sized = mSizes.find(nearestSize(pixelSize));
    return sized == mSizes.end() ? 0.0f : sized->second.lineHeight;
}

}  // namespace ui
