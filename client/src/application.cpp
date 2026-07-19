#include "application.h"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <string>

#include <GLFW/glfw3.h>
#include <filament/Viewport.h>
#include <utils/EntityManager.h>

#include <math/quat.h>

#include "native_window.h"

using namespace filament;
using namespace filament::math;

namespace game {

namespace {

constexpr double kNearPlane = 0.1;
constexpr double kFarPlane = 1000.0;
constexpr double kVerticalFovDegrees = 60.0;

// Overridable so a build can point at a non-local server without recompiling.
std::string authHost() {
    const char* value = std::getenv("AUTH_HOST");
    return value && *value ? value : "127.0.0.1";
}

uint16_t authPort() {
    const char* value = std::getenv("AUTH_SERVER_PORT");
    return value && *value ? static_cast<uint16_t>(std::atoi(value)) : 7001;
}

// Extrapolation is capped: past this, the server has probably stopped sending
// and predicting further would fling the entity across the map.
constexpr double kMaxExtrapolationSeconds = 0.25;

Application* self(GLFWwindow* window) {
    return static_cast<Application*>(glfwGetWindowUserPointer(window));
}

}  // namespace

// GLFW is a C API, so these forward to the instance behind the user pointer.
void charCallback(GLFWwindow* window, unsigned int codepoint) {
    self(window)->mInput.characters.push_back(codepoint);
}

void keyCallback(GLFWwindow* window, int key, int, int action, int) {
    Application* app = self(window);
    const bool pressed = action == GLFW_PRESS || action == GLFW_REPEAT;

    switch (key) {
        case GLFW_KEY_BACKSPACE: app->mInput.backspace = pressed; break;
        case GLFW_KEY_TAB: app->mInput.tab = pressed; break;
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER: app->mInput.enter = pressed; break;
        case GLFW_KEY_ESCAPE:
            if (action == GLFW_PRESS) {
                app->mInput.escape = true;
            }
            break;
        default: break;
    }
}

void mouseButtonCallback(GLFWwindow* window, int button, int action, int) {
    if (button != GLFW_MOUSE_BUTTON_LEFT) {
        return;
    }
    Application* app = self(window);
    if (action == GLFW_PRESS) {
        app->mInput.mouseDown = true;
        app->mInput.mousePressed = true;
    } else if (action == GLFW_RELEASE) {
        app->mInput.mouseDown = false;
        app->mInput.mouseReleased = true;
    }
}

bool Application::init(int width, int height, const char* title) {
    if (!glfwInit()) {
        fprintf(stderr, "glfwInit failed\n");
        return false;
    }

    // Filament owns the Vulkan device and the surface; GLFW must not create a
    // context of its own.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // Compositors key window rules, icons and taskbar entries off these, and
    // GLFW leaves them empty by default.
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "untitled");
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "untitled");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "untitled");

    mWindow = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!mWindow) {
        fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return false;
    }

    // Wayland has no way to query the size before the first configure event, so
    // ask GLFW rather than assuming the requested size was honoured.
    glfwGetFramebufferSize(mWindow, &mWidth, &mHeight);

    glfwSetWindowUserPointer(mWindow, this);
    glfwSetFramebufferSizeCallback(mWindow, [](GLFWwindow* w, int fbWidth, int fbHeight) {
        self(w)->resize(fbWidth, fbHeight);
    });
    glfwSetCharCallback(mWindow, charCallback);
    glfwSetKeyCallback(mWindow, keyCallback);
    glfwSetMouseButtonCallback(mWindow, mouseButtonCallback);

    mEngine = Engine::Builder().backend(Engine::Backend::VULKAN).build();
    if (!mEngine) {
        fprintf(stderr, "Failed to create a Vulkan Filament engine\n");
        return false;
    }

    void* nativeWindow = getNativeWindow(mWindow, mWidth, mHeight);
    if (!nativeWindow) {
        fprintf(stderr, "Failed to obtain a native window handle\n");
        return false;
    }

    mSwapChain = mEngine->createSwapChain(nativeWindow);
    mRenderer = mEngine->createRenderer();
    mScene = mEngine->createScene();
    mView = mEngine->createView();

    mCameraEntity = utils::EntityManager::get().create();
    mCamera = mEngine->createCamera(mCameraEntity);

    mView->setScene(mScene);
    mView->setCamera(mCamera);

    // The world is deliberately not built here: nothing is loaded until the
    // player is actually in it.
    if (!mNet.init()) {
        fprintf(stderr, "Failed to initialise sockets\n");
        return false;
    }

    if (!mUi.init(mEngine)) {
        fprintf(stderr, "Failed to initialise the UI renderer\n");
        return false;
    }

    Renderer::ClearOptions clearOptions;
    clearOptions.clearColor = {0.05f, 0.06f, 0.09f, 1.0f};
    clearOptions.clear = true;
    mRenderer->setClearOptions(clearOptions);

    resize(mWidth, mHeight);
    enterState(AppState::Login);
    return true;
}

void Application::enterState(AppState state) {
    mState = state;

    // The cursor is captured only while actually playing; every UI state needs
    // a visible pointer.
    const bool playing = state == AppState::InGame;
    mCameraController.setEnabled(mWindow, playing);
}

void Application::beginLogin() {
    if (mLoginScreen.username().empty() || mLoginScreen.password().empty()) {
        mLoginScreen.setStatus("username and password are required", true);
        return;
    }

    mLoginScreen.setStatus("connecting...", false);
    mConnectingSince = mElapsedSeconds;
    mNet.login(authHost(), authPort(), mLoginScreen.username(), mLoginScreen.password());
    enterState(AppState::Connecting);
}

void Application::loadWorld() {
    mDemoScene.build(mEngine, mScene);
    mCameraController.reset(0.0f, -10.0f);
}

void Application::unloadWorld() {
    mPrediction.shutdown();
    mDemoScene.destroy(mEngine, mScene);
}

void Application::logout() {
    // Tell the server first, while the connection is still up.
    mNet.logout();
    unloadWorld();

    mUsername.clear();
    mLoginScreen.clearPassword();
    mLoginScreen.resetFocus();
    mLoginScreen.setStatus("logged out", false);
    enterState(AppState::Login);
}

void Application::applyReplication() {
    if (!mDemoScene.loaded()) {
        return;
    }

    std::set<uint32_t> presentPlayers;

    for (const auto& [id, replicated] : mNet.entities()) {
        // Extrapolate from the newest snapshot using the velocities the server
        // sent with it, so motion stays smooth between ticks.
        const double age = std::min(mElapsedSeconds - replicated.receivedAt,
                kMaxExtrapolationSeconds);
        const net::EntityState& e = replicated.state;

        net::Vec3 position{
            e.position.x + e.velocity.x * static_cast<float>(age),
            e.position.y + e.velocity.y * static_cast<float>(age),
            e.position.z + e.velocity.z * static_cast<float>(age),
        };

        // Advance the orientation by the angular velocity over the same window.
        const filament::math::quatf current{e.rotation.w, e.rotation.x, e.rotation.y,
                e.rotation.z};
        const filament::math::float3 axis{e.angularVelocity.x, e.angularVelocity.y,
                e.angularVelocity.z};
        const float speed = length(axis);
        filament::math::quatf predicted = current;
        if (speed > 1e-6f) {
            predicted = filament::math::quatf::fromAxisAngle(axis / speed,
                                 speed * static_cast<float>(age))
                    * current;
        }

        net::Quat rotation{predicted.x, predicted.y, predicted.z, predicted.w};

        if (e.type == static_cast<uint8_t>(net::EntityType::Player)) {
            presentPlayers.insert(id);

            if (id == mNet.ownEntityId()) {
                // Our own character starts predicting from the first
                // authoritative position we see, then reconciles against each
                // later one.
                if (!mPrediction.active()) {
                    mPrediction.init({e.position.x, e.position.y, e.position.z});
                } else if (mNet.snapshotsReceived() != mLastReconciledSnapshot) {
                    mLastReconciledSnapshot = mNet.snapshotsReceived();
                    mPrediction.reconcile(e);
                }

                // Draw it where prediction says, not where the (older)
                // snapshot says.
                const gamesim::Vec3 predicted = mPrediction.renderPosition();
                mDemoScene.setPlayerTransform(mEngine, mScene, id, true,
                        {predicted.x, predicted.y, predicted.z}, rotation);
            } else {
                mDemoScene.setPlayerTransform(mEngine, mScene, id, false, position, rotation);
            }
        } else {
            mDemoScene.setEntityTransform(mEngine, id, position, rotation);
        }
    }

    // Players who stopped being reported have left; drop their renderables.
    mDemoScene.removeAbsentPlayers(mEngine, mScene, presentPlayers);
}

void Application::resize(int width, int height) {
    // Minimising on Wayland reports a zero-sized framebuffer; skip until it
    // comes back, otherwise the swap chain and projection are degenerate.
    if (width <= 0 || height <= 0) {
        return;
    }

    mWidth = width;
    mHeight = height;

    mView->setViewport({0, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
    mUi.setViewport(width, height);

    const double aspect = static_cast<double>(width) / static_cast<double>(height);
    mCamera->setProjection(kVerticalFovDegrees, aspect, kNearPlane, kFarPlane,
            Camera::Fov::VERTICAL);

    // The Wayland swap chain is bound to the size it was created with, so it
    // has to be rebuilt whenever the window changes size.
#if defined(__linux__)
    mEngine->flushAndWait();
    mEngine->destroy(mSwapChain);
    mSwapChain = mEngine->createSwapChain(getNativeWindow(mWindow, mWidth, mHeight));
#endif
}

void Application::update(double deltaSeconds) {
    mElapsedSeconds += deltaSeconds;

    // Never blocks; one non-blocking step of the login/join/stream machine.
    mNet.poll(mElapsedSeconds);

    if (mState == AppState::Connecting) {
        if (mNet.state() == NetClient::State::InWorld) {
            mUsername = mNet.username();
            mLoginScreen.setStatus("", false);
            loadWorld();
            enterState(AppState::InGame);
        } else if (mNet.state() == NetClient::State::Failed) {
            mLoginScreen.setStatus(mNet.error(), true);
            mLoginScreen.clearPassword();
            enterState(AppState::Login);
        }
    } else if ((mState == AppState::InGame || mState == AppState::Menu)
            && mNet.state() == NetClient::State::Failed) {
        // Dropped mid-session: fall back to the login screen with the reason.
        const std::string reason = mNet.error();
        unloadWorld();
        mUsername.clear();
        mLoginScreen.clearPassword();
        mLoginScreen.setStatus(reason, true);
        enterState(AppState::Login);
    }

    // Input is produced on a fixed tick, predicted locally, and sent. The
    // server remains authoritative; prediction only decides what to draw until
    // its answer arrives.
    const MovementIntent intent = mCameraController.sample(mWindow);
    constexpr double kTickSeconds = 1.0 / static_cast<double>(net::kServerTickHz);
    mTickAccumulator += deltaSeconds;
    // Cap the catch-up so a stalled frame doesn't replay a huge backlog.
    if (mTickAccumulator > 0.25) {
        mTickAccumulator = 0.25;
    }
    while (mTickAccumulator >= kTickSeconds) {
        mTickAccumulator -= kTickSeconds;
        if (mState != AppState::InGame) {
            continue;
        }
        const gamesim::CharacterInput input = mPrediction.step(intent.moveForward,
                intent.moveRight, intent.yaw, intent.jump, intent.sprint);
        mNet.sendInput(input.moveForward, input.moveRight, input.yaw, input.jump, input.sprint,
                input.sequence);
    }
    mPrediction.updateSmoothing(static_cast<float>(deltaSeconds));

    applyReplication();

    // Follow the predicted position, not the replicated one: the camera has to
    // track the character the player is actually controlling.
    if (mDemoScene.loaded() && mPrediction.active()) {
        const gamesim::Vec3 position = mPrediction.renderPosition();
        mCameraController.updateCamera(mCamera,
                filament::math::float3{position.x, position.y, position.z});
    }
}

void Application::drawUi() {
    double cursorX = 0.0;
    double cursorY = 0.0;
    glfwGetCursorPos(mWindow, &cursorX, &cursorY);
    mInput.mouseX = static_cast<float>(cursorX);
    mInput.mouseY = static_cast<float>(cursorY);

    mUi.begin();
    mWidgets.begin(&mUi, mInput);

    switch (mState) {
        case AppState::Login:
        case AppState::Connecting: {
            const LoginScreen::Action action = mLoginScreen.draw(mWidgets, mUi, mState);
            if (action == LoginScreen::Action::Submit) {
                beginLogin();
            } else if (action == LoginScreen::Action::Quit) {
                glfwSetWindowShouldClose(mWindow, GLFW_TRUE);
            }
            break;
        }

        case AppState::InGame:
            if (mInput.escape) {
                enterState(AppState::Menu);
            }
            break;

        case AppState::Menu: {
            const MenuScreen::Action action = mMenuScreen.draw(mWidgets, mUi, mUsername);
            if (action == MenuScreen::Action::Resume || mInput.escape) {
                enterState(AppState::InGame);
            } else if (action == MenuScreen::Action::Logout) {
                logout();
            } else if (action == MenuScreen::Action::Quit) {
                glfwSetWindowShouldClose(mWindow, GLFW_TRUE);
            }
            break;
        }
    }

    mWidgets.end();
    mUi.end(mEngine);
    mInput.endFrame();
}

void Application::run() {
    double previous = glfwGetTime();

    while (!glfwWindowShouldClose(mWindow)) {
        glfwPollEvents();

        // Filament paces frames: beginFrame returns false for one that should
        // be skipped. Everything below has to sit inside that check.
        //
        // Doing the per-frame work regardless -- rebuilding the UI geometry
        // and uploading it -- queues thousands of buffer updates per displayed
        // frame, which backs the driver up and reads as input lag. Typed
        // characters aren't lost meanwhile: they accumulate in the input state
        // and are consumed by the next frame that actually draws.
        if (!mRenderer->beginFrame(mSwapChain)) {
            // Yield instead of spinning a core between frames. Short enough to
            // cost no meaningful input latency.
            std::this_thread::sleep_for(std::chrono::microseconds(250));
            continue;
        }

        // Timed here rather than at the top of the loop: this must be the
        // interval between *rendered* frames. Measuring across skipped
        // iterations would report a fraction of a millisecond and starve the
        // fixed-tick accumulator that drives input and prediction.
        const double now = glfwGetTime();
        const double delta = now - previous;
        previous = now;

        update(delta);
        drawUi();

        mRenderer->render(mView);
        // Drawn after the world so it composites on top.
        mRenderer->render(mUi.view());
        mRenderer->endFrame();
    }
}

Application::~Application() {
    mNet.shutdown();

    if (mEngine) {
        mEngine->flushAndWait();
        mUi.destroy(mEngine);
        mDemoScene.destroy(mEngine, mScene);
        mEngine->destroy(mView);
        mEngine->destroy(mScene);
        mEngine->destroy(mRenderer);
        mEngine->destroyCameraComponent(mCameraEntity);
        utils::EntityManager::get().destroy(mCameraEntity);
        mEngine->destroy(mSwapChain);
        Engine::destroy(&mEngine);
    }

    if (mWindow) {
        glfwDestroyWindow(mWindow);
    }
    glfwTerminate();
}

}  // namespace game
