#include "ui/widgets.h"

#include <chrono>

namespace ui {

namespace {

constexpr float kTextScale = 2.0f;
constexpr float kBorder = 2.0f;
constexpr float kPadding = 10.0f;
constexpr double kCaretBlinkSeconds = 0.53;

// Seconds since process start, used only for the caret blink.
double now() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// Centres text vertically in a box of the given height.
float centreTextY(float y, float height, float scale) {
    return y + (height - UiRenderer::textHeight(scale)) * 0.5f;
}

}  // namespace

void Ui::begin(UiRenderer* renderer, const InputState& input) {
    mRenderer = renderer;
    mInput = input;
    mCaretTimer = now();
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
}

bool Ui::hovered(float x, float y, float width, float height) const {
    return mInput.mouseX >= x && mInput.mouseX < x + width && mInput.mouseY >= y
            && mInput.mouseY < y + height;
}

void Ui::panel(float x, float y, float width, float height) {
    mRenderer->rect(x, y, width, height, theme.panel);
    mRenderer->rectOutline(x, y, width, height, kBorder, theme.border);
}

void Ui::label(float x, float y, const std::string& text, float scale) {
    mRenderer->text(x, y, text, theme.text, scale);
}

void Ui::labelDim(float x, float y, const std::string& text, float scale) {
    mRenderer->text(x, y, text, theme.textDim, scale);
}

void Ui::labelCentred(float centreX, float y, const std::string& text, float scale) {
    mRenderer->text(centreX - UiRenderer::textWidth(text, scale) * 0.5f, y, text, theme.text,
            scale);
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

    mRenderer->text(x + (width - UiRenderer::textWidth(text, kTextScale)) * 0.5f,
            centreTextY(y, height, kTextScale), text, theme.text, kTextScale);
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
    const float textY = centreTextY(y, height, kTextScale);

    if (value.empty() && !isFocused) {
        mRenderer->text(textX, textY, placeholder, theme.textDim, kTextScale);
    } else {
        const std::string shown = password ? std::string(value.size(), '*') : value;
        mRenderer->text(textX, textY, shown, theme.text, kTextScale);
    }

    // Blinking caret, drawn after the text so it sits at the insertion point.
    if (isFocused) {
        const bool visible = static_cast<int>(mCaretTimer / kCaretBlinkSeconds) % 2 == 0;
        if (visible) {
            const float caretX = textX + UiRenderer::textWidth(
                    password ? std::string(value.size(), '*') : value, kTextScale);
            mRenderer->rect(caretX, textY, 2.0f, UiRenderer::textHeight(kTextScale), theme.text);
        }
    }

    return submitted;
}

}  // namespace ui
