#pragma once

#include <string>

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/View.h>
#include <utils/Entity.h>

#include "camera_controller.h"
#include "net_client.h"
#include "prediction.h"
#include "scene.h"
#include "screens.h"
#include "ui/ui_renderer.h"
#include "ui/widgets.h"

struct GLFWwindow;

namespace game {

class Application {
public:
    Application() = default;
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool init(int width, int height, const char* title);
    void run();

private:
    void resize(int width, int height);
    void update(double deltaSeconds);
    void drawUi();

    // State changes that also have to move the cursor and camera in step.
    void enterState(AppState state);
    void beginLogin();
    void logout();
    // Builds the world on entry and tears it down on exit, so nothing is
    // resident while the login screen is up.
    void loadWorld();
    void unloadWorld();
    // Applies replicated transforms, extrapolated to the current frame.
    void applyReplication();

    GLFWwindow* mWindow = nullptr;

    filament::Engine* mEngine = nullptr;
    filament::SwapChain* mSwapChain = nullptr;
    filament::Renderer* mRenderer = nullptr;
    filament::Scene* mScene = nullptr;
    filament::View* mView = nullptr;
    filament::Camera* mCamera = nullptr;
    utils::Entity mCameraEntity;

    CameraController mCameraController;
    DemoScene mDemoScene;

    ui::UiRenderer mUi;
    ui::Ui mWidgets;
    ui::InputState mInput;
    LoginScreen mLoginScreen;
    MenuScreen mMenuScreen;

    NetClient mNet;
    Prediction mPrediction;
    // Input is produced at the server's fixed tick rate, not the render rate:
    // predicting at a different timestep than the server simulates would
    // diverge every frame.
    double mTickAccumulator = 0.0;
    uint32_t mLastReconciledSnapshot = 0;
    AppState mState = AppState::Login;
    std::string mUsername;
    double mElapsedSeconds = 0.0;
    double mConnectingSince = 0.0;

    int mWidth = 0;
    int mHeight = 0;

    // GLFW callbacks need access to the input struct.
    friend void charCallback(GLFWwindow*, unsigned int);
    friend void keyCallback(GLFWwindow*, int, int, int, int);
    friend void mouseButtonCallback(GLFWwindow*, int, int, int);
};

}  // namespace game
