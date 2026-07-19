#pragma once

#include <cstdint>

namespace ui {

// 8x8 bitmap font covering printable ASCII (0x20..0x7E).
//
// Each glyph is 8 bytes, one per row, top row first. Bit 7 (0x80) is the
// leftmost pixel. The data is written as binary literals in font.cpp so each
// glyph is readable as a picture in the source.
constexpr int kGlyphWidth = 8;
constexpr int kGlyphHeight = 8;
constexpr char kFirstGlyph = 0x20;
constexpr char kLastGlyph = 0x7E;
constexpr int kGlyphCount = kLastGlyph - kFirstGlyph + 1;

extern const uint8_t kFontData[kGlyphCount][kGlyphHeight];

// Rows for a character, or the rows for '?' if it isn't in the set.
const uint8_t* glyphRows(char c);

}  // namespace ui
