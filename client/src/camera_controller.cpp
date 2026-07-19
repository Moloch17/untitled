#include "camera_controller.h"

#include <cmath>

#include <GLFW/glfw3.h>
#include <math/mat3.h>
#include <math/scalar.h>

using namespace filament;
using namespace filament::math;

namespace game {

namespace {

constexpr float kMouseSensitivity = 0.12f;  // degrees per pixel
constexpr float kMaxPitch = 75.0f;
constexpr float kMinPitch = -35.0f;

// Over-the-shoulder framing: back along the view direction, up a little, and
// offset to the right so the character doesn't sit in the middle of the screen.
constexpr float kCameraDistance = 4.0f;
constexpr float kCameraHeight = 1.1f;
constexpr float kShoulderOffset = 0.75f;
// The camera looks slightly above the capsule's centre, roughly at head height.
constexpr float kLookAtHeight = 0.9f;

float3 directionFromAngles(float yawDegrees, float pitchDegrees) {
    const float yaw = yawDegrees * f::DEG_TO_RAD;
    const float pitch = pitchDegrees * f::DEG_TO_RAD;
    return normalize(float3{
        std::sin(yaw) * std::cos(pitch),
        std::sin(pitch),
        -std::cos(yaw) * std::cos(pitch),
    });
}

}  // namespace

void CameraController::reset(float yawDegrees, float pitchDegrees) {
    mYaw = yawDegrees;
    mPitch = pitchDegrees;
    mHasLastCursor = false;
}

void CameraController::setEnabled(GLFWwindow* window, bool enabled) {
    mEnabled = enabled;
    glfwSetInputMode(window, GLFW_CURSOR, enabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);

    // Raw motion skips the desktop's pointer acceleration curve, which is what
    // you want for camera look.
    if (glfwRawMouseMotionSupported()) {
        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, enabled ? GLFW_TRUE : GLFW_FALSE);
    }

    // Force the next sample to re-seed its reference point, otherwise the
    // camera jumps by however far the cursor moved while it was free.
    mHasLastCursor = false;
}

float CameraController::yawRadians() const {
    return mYaw * f::DEG_TO_RAD;
}

MovementIntent CameraController::sample(GLFWwindow* window) {
    MovementIntent intent;
    if (!mEnabled) {
        // Still report where we're facing, so the character doesn't snap when
        // the menu closes.
        intent.yaw = yawRadians();
        return intent;
    }

    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);

    if (mHasLastCursor) {
        mYaw += static_cast<float>(x - mLastCursorX) * kMouseSensitivity;
        // Screen coordinates grow downwards; invert so pushing the mouse
        // forward looks up.
        mPitch -= static_cast<float>(y - mLastCursorY) * kMouseSensitivity;
        mPitch = clamp(mPitch, kMinPitch, kMaxPitch);
    }

    mLastCursorX = x;
    mLastCursorY = y;
    mHasLastCursor = true;

    const auto held = [window](int key) { return glfwGetKey(window, key) == GLFW_PRESS; };
    if (held(GLFW_KEY_W)) intent.moveForward += 1.0f;
    if (held(GLFW_KEY_S)) intent.moveForward -= 1.0f;
    if (held(GLFW_KEY_D)) intent.moveRight += 1.0f;
    if (held(GLFW_KEY_A)) intent.moveRight -= 1.0f;
    intent.jump = held(GLFW_KEY_SPACE);
    intent.sprint = held(GLFW_KEY_LEFT_SHIFT);
    intent.yaw = yawRadians();
    return intent;
}

void CameraController::updateCamera(Camera* camera, const float3& target) {
    const float3 forward = directionFromAngles(mYaw, mPitch);
    const float3 worldUp{0.0f, 1.0f, 0.0f};
    const float3 right = normalize(cross(forward, worldUp));

    const float3 focus = target + float3{0.0f, kLookAtHeight, 0.0f};
    const float3 eye = focus - forward * kCameraDistance + float3{0.0f, kCameraHeight, 0.0f}
            + right * kShoulderOffset;

    camera->lookAt(eye, focus, worldUp);
}

}  // namespace game
