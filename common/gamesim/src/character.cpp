#include "gamesim/character.h"

#include <cmath>

namespace gamesim {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}  // namespace

void stepCharacter(Character& character, const CharacterInput& input, float deltaSeconds) {
    // --- Horizontal: velocity follows intent directly ----------------------
    // No acceleration curve and no friction: a character that keeps sliding
    // after the key is released feels broken, however physical it is.
    const float sinYaw = std::sin(input.yaw);
    const float cosYaw = std::cos(input.yaw);
    float wishX = sinYaw * input.moveForward + cosYaw * input.moveRight;
    float wishZ = -cosYaw * input.moveForward + sinYaw * input.moveRight;

    // Normalise so diagonal movement isn't faster than straight movement.
    const float wishLength = std::sqrt(wishX * wishX + wishZ * wishZ);
    if (wishLength > 1.0f) {
        wishX /= wishLength;
        wishZ /= wishLength;
    }

    const float speed = input.sprint ? kSprintSpeed : kWalkSpeed;
    float targetX = wishX * speed;
    float targetZ = wishZ * speed;

    if (character.grounded) {
        character.velocity.x = targetX;
        character.velocity.z = targetZ;
    } else {
        // Airborne: blend toward the target so a jump keeps its momentum.
        character.velocity.x += (targetX - character.velocity.x) * kAirControl;
        character.velocity.z += (targetZ - character.velocity.z) * kAirControl;
    }

    // --- Vertical: gravity and jump, integrated by hand --------------------
    if (input.jump && character.grounded) {
        character.velocity.y = kJumpSpeed;
        character.grounded = false;
    } else if (!character.grounded) {
        character.velocity.y += kGravity * deltaSeconds;
        if (character.velocity.y < kTerminalVelocity) {
            character.velocity.y = kTerminalVelocity;
        }
    }

    // --- Face where the camera faces, while moving --------------------------
    // Locked to the camera's forward direction, not to the velocity vector.
    // Deriving it from velocity points the body along the diagonal the moment
    // two keys are held -- 45 degrees off for W+D, 90 for a pure strafe, 180
    // for walking backwards. Locking it to the camera means strafing slides
    // sideways and S walks backwards, both while still facing forward, which is
    // what a third-person camera implies.
    //
    // Facing only changes while there is movement intent: looking around while
    // standing still leaves the character pointing where it was.
    if (wishLength > 0.0f) {
        character.facingYaw = input.yaw;
    }

    // --- Move the transform ------------------------------------------------
    character.position.x += character.velocity.x * deltaSeconds;
    character.position.y += character.velocity.y * deltaSeconds;
    character.position.z += character.velocity.z * deltaSeconds;

    // --- Resolve the ground ------------------------------------------------
    // Sampled, not cast: for a heightfield the answer is exact and identical on
    // both ends.
    const float groundY = terrain::heightAt(character.position.x, character.position.z);
    const float restY = groundY + kRestHeight;
    const bool descending = character.velocity.y <= 0.0f;

    if (character.position.y <= restY) {
        // Penetrated the ground: sit exactly on it.
        character.position.y = restY;
        character.velocity.y = 0.0f;
        character.grounded = true;
    } else if (descending && character.position.y - restY <= kGroundSnapDistance) {
        // Just above it and falling: snap down, which keeps a character walking
        // down a slope in contact instead of hopping off every bump.
        character.position.y = restY;
        character.velocity.y = 0.0f;
        character.grounded = true;
    } else {
        character.grounded = false;
    }
}

}  // namespace gamesim
