#include "gamesim/character.h"

#include <cmath>

namespace gamesim {

b3WorldId createWorld() {
    b3WorldDef worldDef = b3DefaultWorldDef();
    // Gravity is integrated by the character controller itself; nothing in this
    // world is a dynamic body, so the world's own gravity never applies.
    worldDef.gravity = (b3Vec3){0.0f, 0.0f, 0.0f};
    return b3CreateWorld(&worldDef);
}

b3BodyId createGround(b3WorldId world) {
    // Top surface at y = 0, so the collision surface and the rendered plane are
    // the same place.
    b3BodyDef bodyDef = b3DefaultBodyDef();
    bodyDef.position = (b3Pos){0.0f, -1.0f, 0.0f};
    const b3BodyId ground = b3CreateBody(world, &bodyDef);

    b3BoxHull box = b3MakeBoxHull(kGroundHalfExtent, 1.0f, kGroundHalfExtent);
    b3ShapeDef shapeDef = b3DefaultShapeDef();
    b3CreateHullShape(ground, &shapeDef, &box.base);
    return ground;
}

void finalizeWorld(b3WorldId world) {
    // Static shapes enter the broadphase during a step. Without one, casts
    // against them find nothing and every character falls through the floor.
    b3World_Step(world, 1.0f / 60.0f, 1);
}

bool groundHeightBelow(b3WorldId world, Vec3 position, float maxDistance, float* groundY) {
    const b3Pos origin = (b3Pos){position.x, position.y, position.z};
    const b3Vec3 translation = {0.0f, -maxDistance, 0.0f};

    const b3RayResult hit = b3World_CastRayClosest(world, origin, translation,
            b3DefaultQueryFilter());
    if (!hit.hit) {
        return false;
    }
    *groundY = static_cast<float>(hit.point.y);
    return true;
}

void stepCharacter(b3WorldId world, Character& character, const CharacterInput& input,
        float deltaSeconds) {
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

    // --- Move the transform ------------------------------------------------
    character.position.x += character.velocity.x * deltaSeconds;
    character.position.y += character.velocity.y * deltaSeconds;
    character.position.z += character.velocity.z * deltaSeconds;

    // --- Resolve the ground ------------------------------------------------
    // Cast from head height so a character that moved into the floor this tick
    // is still above the ray's origin.
    const Vec3 castFrom{character.position.x, character.position.y + kCapsuleHalfHeight,
        character.position.z};
    const float castDistance = kCapsuleHalfHeight + kRestHeight + kGroundSnapDistance;

    float groundY = 0.0f;
    const bool foundGround = groundHeightBelow(world, castFrom, castDistance, &groundY);

    if (!foundGround) {
        // Nothing underneath: walked off an edge.
        character.grounded = false;
        return;
    }

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
