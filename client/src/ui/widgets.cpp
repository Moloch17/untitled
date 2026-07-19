#include "ui/widgets.h"

#include <algorithm>
#include <chrono>
#include <iterator>

namespace ui {

namespace {

constexpr int kTextSize = kFontSizeBody;
constexpr float kBorder = 2.0f;
constexpr float kPadding = 10.0f;
constexpr double kCaretBlinkSeconds = 0.53;

// Seconds since process start, used only for the caret blink.
double now() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// Centres a line of text vertically in a box of the given height.
float centreTextY(const UiRenderer& renderer, float y, float height, int pixelSize) {
    return y + (height - renderer.textHeight(pixelSize)) * 0.5f;
}

}  // namespace

void Ui::begin(UiRenderer* renderer, const InputState& input) {
    mRenderer = renderer;
    mInput = input;
    mCaretTimer = now();

    // Tab is handled in end(), once every field has registered itself, so the
    // order matches what's on screen this frame.
    mTabPending = input.tab;
    mTextFields.clear();
}

void Ui::end() {
    // A click that didn't land on any widget clears focus, which is what
    // clicking the background should do.
    if (mInput.mousePressed && mActive == 0) {
        mFocused = 0;
    }
    if (mInput.mouseReleased) {
        mActive = 0;
    }

    // Tab moves to the next text field, wrapping at the end. With nothing
    // focused it selects the first, so Tab always does something useful.
    if (mTabPending && !mTextFields.empty()) {
        auto current = std::find(mTextFields.begin(), mTextFields.end(), mFocused);
        if (current == mTextFields.end() || std::next(current) == mTextFields.end()) {
            mFocused = mTextFields.front();
        } else {
            mFocused = *std::next(current);
        }
    }
    mTabPending = false;
}

bool Ui::hovered(float x, float y, float width, float height) const {
    return mInput.mouseX >= x && mInput.mouseX < x + width && mInput.mouseY >= y
            && mInput.mouseY < y + height;
}

void Ui::panel(float x, float y, float width, float height) {
    mRenderer->rect(x, y, width, height, theme.panel);
    mRenderer->rectOutline(x, y, width, height, kBorder, theme.border);
}

void Ui::label(float x, float y, const std::string& text, int pixelSize) {
    mRenderer->text(x, y, text, theme.text, pixelSize);
}

void Ui::labelDim(float x, float y, const std::string& text, int pixelSize) {
    mRenderer->text(x, y, text, theme.textDim, pixelSize);
}

void Ui::labelCentred(float centreX, float y, const std::string& text, int pixelSize) {
    mRenderer->text(centreX - mRenderer->textWidth(text, pixelSize) * 0.5f, y, text, theme.text,
            pixelSize);
}

bool Ui::buttonInternal(int id, float x, float y, float width, float height,
        const std::string& text, Color base, Color hover, Color pressed) {
    const bool isHovered = hovered(x, y, width, height);

    if (isHovered && mInput.mousePressed) {
        mActive = id;
        mFocused = id;
    }

    // Released while still over the button: that's a click. Dragging off before
    // releasing cancels, which is the standard behaviour.
    bool clicked = false;
    if (mActive == id && mInput.mouseReleased && isHovered) {
        clicked = true;
    }

    const Color fill = (mActive == id && isHovered) ? pressed : (isHovered ? hover : base);
    mRenderer->rect(x, y, width, height, fill);
    if (mFocused == id) {
        mRenderer->rectOutline(x, y, width, height, kBorder, theme.borderFocused);
    }

    mRenderer->text(x + (width - mRenderer->textWidth(text, kTextSize)) * 0.5f,
            centreTextY(*mRenderer, y, height, kTextSize), text, theme.text, kTextSize);
    return clicked;
}

bool Ui::button(int id, float x, float y, float width, float height, const std::string& text) {
    return buttonInternal(id, x, y, width, height, text, theme.accent, theme.accentHover,
            theme.accentPressed);
}

bool Ui::dangerButton(int id, float x, float y, float width, float height,
        const std::string& text) {
    return buttonInternal(id, x, y, width, height, text, theme.danger, theme.dangerHover,
            theme.danger);
}

bool Ui::textField(int id, float x, float y, float width, float height, std::string& value,
        const std::string& placeholder, bool password, size_t maxLength) {
    // Registered in draw order so Tab cycles top to bottom.
    mTextFields.push_back(id);

    const bool isHovered = hovered(x, y, width, height);
    if (isHovered && mInput.mousePressed) {
        mFocused = id;
        mActive = id;
    }

    const bool isFocused = mFocused == id;
    bool submitted = false;

    if (isFocused) {
        for (uint32_t codepoint : mInput.characters) {
            // Only ASCII the font can actually draw; anything else would render
            // as a placeholder glyph and be misleading in a password field.
            if (codepoint >= 0x20 && codepoint <= 0x7E && value.size() < maxLength) {
                value.push_back(static_cast<char>(codepoint));
            }
        }
        if (mInput.backspace && !value.empty()) {
            value.pop_back();
        }
        if (mInput.enter) {
            submitted = true;
        }
    }

    mRenderer->rect(x, y, width, height, theme.field);
    mRenderer->rectOutline(x, y, width, height, kBorder,
            isFocused ? theme.borderFocused : theme.border);

    const float textX = x + kPadding;
    const float textY = centreTextY(*mRenderer, y, height, kTextSize);
    const std::string shown = password ? std::string(value.size(), '*') : value;

    if (value.empty() && !isFocused) {
        mRenderer->text(textX, textY, placeholder, theme.textDim, kTextSize);
    } else {
        mRenderer->text(textX, textY, shown, theme.text, kTextSize);
    }

    // Blinking caret, drawn after the text so it sits at the insertion point.
    if (isFocused) {
        const bool visible = static_cast<int>(mCaretTimer / kCaretBlinkSeconds) % 2 == 0;
        if (visible) {
            const float caretX = textX + mRenderer->textWidth(shown, kTextSize);
            const float caretHeight = mRenderer->textHeight(kTextSize) * 0.8f;
            mRenderer->rect(caretX + 1.0f, textY + caretHeight * 0.15f, 2.0f, caretHeight,
                    theme.text);
        }
    }

    return submitted;
}

}  // namespace ui
