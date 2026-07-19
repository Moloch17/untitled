#pragma once

#include <cstdint>

#include <filament/Camera.h>
#include <math/vec3.h>

struct GLFWwindow;

namespace game {

// What the player is asking to do this frame. The client never moves anything
// itself: this is sent to the server, which decides what actually happens.
struct MovementIntent {
    float moveForward = 0.0f;  // -1..1
    float moveRight = 0.0f;    // -1..1
    float yaw = 0.0f;          // radians
    bool jump = false;
    bool sprint = false;
};

// Over-the-shoulder camera. Mouse look aims the camera; the camera's yaw is
// also the direction the character faces, so movement follows the view.
//
// The camera orbits a target position supplied each frame -- the replicated
// position of the local player's capsule.
class CameraController {
public:
    void reset(float yawDegrees, float pitchDegrees);

    // Captures the cursor and starts responding to input, or releases it. The
    // application drives this from its state machine: the cursor is only
    // captured while actually playing, never while a menu is up.
    void setEnabled(GLFWwindow* window, bool enabled);
    bool enabled() const { return mEnabled; }

    // Reads the mouse and keyboard, and returns what the player wants to do.
    MovementIntent sample(GLFWwindow* window);

    // Places the camera behind and above `target`, looking over its shoulder.
    void updateCamera(filament::Camera* camera, const filament::math::float3& target);

    float yawRadians() const;

private:
    float mYaw = 0.0f;    // degrees, 0 looks down -Z
    float mPitch = 0.0f;  // degrees, clamped to avoid flipping at the poles

    bool mEnabled = false;
    bool mHasLastCursor = false;
    double mLastCursorX = 0.0;
    double mLastCursorY = 0.0;
};

}  // namespace game
