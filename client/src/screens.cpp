#include "screens.h"

namespace game {

namespace {

// Widget ids. They only have to be unique within a screen.
enum WidgetId {
    kIdUsername = 1,
    kIdPassword,
    kIdSubmit,
    kIdQuit,
    kIdResume,
    kIdLogout,
    kIdMenuQuit,
};

constexpr float kFieldHeight = 40.0f;
constexpr float kButtonHeight = 44.0f;
constexpr float kRowGap = 16.0f;
constexpr float kTitleScale = 4.0f;
constexpr float kLabelScale = 2.0f;

}  // namespace

void LoginScreen::setStatus(const std::string& status, bool error) {
    mStatus = status;
    mStatusIsError = error;
}

LoginScreen::Action LoginScreen::draw(ui::Ui& ui, ui::UiRenderer& renderer, AppState state) {
    const float screenWidth = static_cast<float>(renderer.width());
    const float screenHeight = static_cast<float>(renderer.height());

    // The login screen owns the whole window, so dim the world behind it.
    renderer.rect(0, 0, screenWidth, screenHeight, ui.theme.background);

    const float panelWidth = 460.0f;
    const float panelHeight = 320.0f;
    const float panelX = (screenWidth - panelWidth) * 0.5f;
    const float panelY = (screenHeight - panelHeight) * 0.5f;

    ui.panel(panelX, panelY, panelWidth, panelHeight);

    const float contentX = panelX + 32.0f;
    const float contentWidth = panelWidth - 64.0f;
    float y = panelY + 28.0f;

    ui.labelCentred(panelX + panelWidth * 0.5f, y, "UNTITLED", kTitleScale);
    y += ui::UiRenderer::textHeight(kTitleScale) + 28.0f;

    const bool busy = state == AppState::Connecting;

    ui.labelDim(contentX, y, "USERNAME", kLabelScale);
    y += ui::UiRenderer::textHeight(kLabelScale) + 6.0f;
    const bool usernameSubmitted =
            ui.textField(kIdUsername, contentX, y, contentWidth, kFieldHeight, mUsername,
                    "enter username");
    y += kFieldHeight + kRowGap;

    ui.labelDim(contentX, y, "PASSWORD", kLabelScale);
    y += ui::UiRenderer::textHeight(kLabelScale) + 6.0f;
    const bool passwordSubmitted =
            ui.textField(kIdPassword, contentX, y, contentWidth, kFieldHeight, mPassword,
                    "enter password", /*password=*/true);
    y += kFieldHeight + kRowGap + 4.0f;

    const float buttonWidth = (contentWidth - 12.0f) * 0.5f;
    const bool submitClicked =
            ui.button(kIdSubmit, contentX, y, buttonWidth, kButtonHeight, busy ? "..." : "LOGIN");
    const bool quitClicked = ui.button(kIdQuit, contentX + buttonWidth + 12.0f, y, buttonWidth,
            kButtonHeight, "QUIT");
    y += kButtonHeight + 14.0f;

    if (!mStatus.empty()) {
        const ui::Color colour = mStatusIsError ? ui::Color::rgba(230, 110, 100, 255)
                                                : ui.theme.textDim;
        const float textX = panelX + (panelWidth - ui::UiRenderer::textWidth(mStatus, 1.5f)) * 0.5f;
        renderer.text(textX, y, mStatus, colour, 1.5f);
    }

    if (quitClicked) {
        return Action::Quit;
    }
    // Enter in either field submits, which is what a two-field form should do.
    if (!busy && (submitClicked || usernameSubmitted || passwordSubmitted)) {
        return Action::Submit;
    }
    return Action::None;
}

MenuScreen::Action MenuScreen::draw(ui::Ui& ui, ui::UiRenderer& renderer,
        const std::string& username) {
    const float screenWidth = static_cast<float>(renderer.width());
    const float screenHeight = static_cast<float>(renderer.height());

    // Dim the world rather than hiding it: the player is still in the world.
    renderer.rect(0, 0, screenWidth, screenHeight, ui::Color::rgba(8, 10, 14, 150));

    const float panelWidth = 340.0f;
    const float panelHeight = 260.0f;
    const float panelX = (screenWidth - panelWidth) * 0.5f;
    const float panelY = (screenHeight - panelHeight) * 0.5f;

    ui.panel(panelX, panelY, panelWidth, panelHeight);

    float y = panelY + 26.0f;
    ui.labelCentred(panelX + panelWidth * 0.5f, y, "PAUSED", 3.0f);
    y += ui::UiRenderer::textHeight(3.0f) + 10.0f;

    const std::string subtitle = "logged in as " + username;
    renderer.text(panelX + (panelWidth - ui::UiRenderer::textWidth(subtitle, 1.5f)) * 0.5f, y,
            subtitle, ui.theme.textDim, 1.5f);
    y += ui::UiRenderer::textHeight(1.5f) + 26.0f;

    const float contentX = panelX + 30.0f;
    const float contentWidth = panelWidth - 60.0f;

    const bool resume = ui.button(kIdResume, contentX, y, contentWidth, kButtonHeight, "RESUME");
    y += kButtonHeight + 12.0f;
    const bool logout =
            ui.dangerButton(kIdLogout, contentX, y, contentWidth, kButtonHeight, "LOGOUT");
    y += kButtonHeight + 12.0f;
    const bool quit = ui.button(kIdMenuQuit, contentX, y, contentWidth, kButtonHeight, "QUIT");

    if (resume) {
        return Action::Resume;
    }
    if (logout) {
        return Action::Logout;
    }
    if (quit) {
        return Action::Quit;
    }
    return Action::None;
}

}  // namespace game
