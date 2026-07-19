#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ui/ui_renderer.h"

namespace ui {

// Input collected from GLFW callbacks over the last frame and handed to the UI
// once per frame.
struct InputState {
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    bool mouseDown = false;      // held this frame
    bool mousePressed = false;   // went down this frame
    bool mouseReleased = false;  // came up this frame

    // Characters typed this frame, already decoded by GLFW.
    std::vector<uint32_t> characters;

    bool backspace = false;
    bool tab = false;
    bool enter = false;
    bool escape = false;

    // Clears the one-frame edges. Call after the UI has consumed them.
    void endFrame() {
        mousePressed = false;
        mouseReleased = false;
        characters.clear();
        backspace = false;
        tab = false;
        enter = false;
        escape = false;
    }
};

// Colours for the whole interface, kept in one place.
struct Theme {
    Color background = Color::rgba(14, 16, 22, 235);
    Color panel = Color::rgba(26, 30, 40, 245);
    Color border = Color::rgba(70, 80, 100, 255);
    Color borderFocused = Color::rgba(120, 170, 240, 255);
    Color field = Color::rgba(18, 20, 27, 255);
    Color text = Color::rgba(226, 232, 240, 255);
    Color textDim = Color::rgba(130, 142, 160, 255);
    Color accent = Color::rgba(58, 110, 200, 255);
    Color accentHover = Color::rgba(78, 138, 235, 255);
    Color accentPressed = Color::rgba(42, 86, 165, 255);
    Color danger = Color::rgba(190, 70, 62, 255);
    Color dangerHover = Color::rgba(215, 88, 80, 255);
};

// Immediate-mode UI: widgets are function calls, and the only retained state is
// which widget is focused or being pressed. Every widget needs a stable `id`
// that is unique within a screen -- that's what identifies it across frames.
class Ui {
public:
    void begin(UiRenderer* renderer, const InputState& input);
    void end();

    void panel(float x, float y, float width, float height);
    void label(float x, float y, const std::string& text, int pixelSize = kFontSizeBody);
    void labelDim(float x, float y, const std::string& text, int pixelSize = kFontSizeBody);
    void labelCentred(float centreX, float y, const std::string& text,
            int pixelSize = kFontSizeBody);

    // Returns true on the frame the button is released while hovered, which is
    // what users expect: pressing and dragging off cancels.
    bool button(int id, float x, float y, float width, float height, const std::string& text);
    bool dangerButton(int id, float x, float y, float width, float height,
            const std::string& text);

    // Editable single-line field. `value` is modified in place. Returns true if
    // Enter was pressed while focused.
    bool textField(int id, float x, float y, float width, float height, std::string& value,
            const std::string& placeholder, bool password = false, size_t maxLength = 32);

    // Lets a screen set initial focus, so a form is typable without clicking.
    void setFocus(int id) { mFocused = id; }
    int focused() const { return mFocused; }
    bool hasFocus() const { return mFocused != 0; }

    Theme theme;

private:
    bool hovered(float x, float y, float width, float height) const;
    bool buttonInternal(int id, float x, float y, float width, float height,
            const std::string& text, Color base, Color hover, Color pressed);

    UiRenderer* mRenderer = nullptr;
    InputState mInput;

    int mFocused = 0;
    int mActive = 0;  // widget currently held down

    // Text fields register themselves each frame in draw order, which is what
    // Tab cycles through. Rebuilt every frame so it follows the current screen
    // rather than going stale when the layout changes.
    std::vector<int> mTextFields;
    bool mTabPending = false;

    double mCaretTimer = 0.0;
};

}  // namespace ui
