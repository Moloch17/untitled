#pragma once

#include <cstdint>

#include <box3d/box3d.h>
#include <terrain/heightfield.h>

// Shared character simulation.
//
// Characters are *not* rigid bodies. Their transform is moved directly: the
// controller integrates velocity and gravity itself and resolves the ground by
// query. Handing a character to a physics solver makes it floaty and lets
// contacts shove it around in ways a player reads as the game fighting them,
// which is why this is a special case rather than ordinary dynamics.
//
// The ground comes from the shared terrain heightfield, sampled directly rather
// than raycast. For a heightfield that is both exact and far cheaper than a
// cast, and -- because client and server call the same function -- it removes a
// whole class of prediction error. box3d remains for dynamic bodies, which do
// not exist yet; it no longer holds the terrain.
//
// Client-side prediction requires both ends to apply identical rules to
// identical input, so this code is compiled into the world server and the
// client, and is the only place character movement is expressed.
namespace gamesim {

// --- Tuning, shared by both ends -------------------------------------------
constexpr float kGravity = -20.0f;  // snappier than real gravity, as games are

constexpr float kCapsuleRadius = 0.35f;
constexpr float kCapsuleHalfHeight = 0.5f;
// Where a grounded capsule's centre sits above the surface under it.
constexpr float kRestHeight = kCapsuleHalfHeight + kCapsuleRadius;

constexpr float kWalkSpeed = 5.0f;
constexpr float kSprintSpeed = 9.0f;
constexpr float kJumpSpeed = 7.5f;
constexpr float kAirControl = 0.35f;
// Falling faster than gravity alone would allow makes landings feel mushy.
constexpr float kTerminalVelocity = -50.0f;
// A character within this distance of the ground while descending is snapped
// down to it, so walking down a slope doesn't leave it stepping into the air.
constexpr float kGroundSnapDistance = 0.15f;

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// Movement intent for one simulation tick.
struct CharacterInput {
    uint32_t sequence = 0;
    float moveForward = 0.0f;  // -1..1
    float moveRight = 0.0f;    // -1..1
    float yaw = 0.0f;          // radians
    bool jump = false;
    bool sprint = false;
};

// The complete state of a character. Plain data on purpose: a rollback is then
// just an assignment, with nothing hidden in a solver to restore.
struct Character {
    Vec3 position;
    Vec3 velocity;
    bool grounded = false;
    // Where the body is pointing, which is deliberately *not* the camera's
    // direction: standing still and looking around must not spin the character.
    float facingYaw = 0.0f;
};

// Advances a character by one fixed tick. Must be called at the same rate on
// both ends: a different timestep produces different results and therefore
// constant corrections.
void stepCharacter(Character& character, const CharacterInput& input, float deltaSeconds);

}  // namespace gamesim
