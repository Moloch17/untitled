#pragma once

#include <string>

#include "ui/widgets.h"

namespace game {

// What the client is currently showing. The world is only simulated and
// rendered once the player has been let in.
enum class AppState {
    Login,       // credentials form, no world
    Connecting,  // request in flight
    InGame,      // world visible, cursor captured
    Menu,        // in-world, cursor free, escape menu up
};

// Login form state and layout. Drawing it returns what the user asked for; the
// caller performs the actual network request.
class LoginScreen {
public:
    enum class Action {
        None,
        Submit,
        Quit,
    };

    Action draw(ui::Ui& ui, ui::UiRenderer& renderer, AppState state);

    const std::string& username() const { return mUsername; }
    const std::string& password() const { return mPassword; }

    void setStatus(const std::string& status, bool error);
    void clearPassword() { mPassword.clear(); }

private:
    std::string mUsername;
    std::string mPassword;
    std::string mStatus;
    bool mStatusIsError = false;
};

// Escape menu shown over the world.
class MenuScreen {
public:
    enum class Action {
        None,
        Resume,
        Logout,
        Quit,
    };

    Action draw(ui::Ui& ui, ui::UiRenderer& renderer, const std::string& username);
};

}  // namespace game
