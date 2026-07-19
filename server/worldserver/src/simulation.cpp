#include "simulation.h"

#include <cmath>

namespace world {

namespace {

constexpr float kSpawnHeight = 1.2f;

// --- Cube ------------------------------------------------------------------
constexpr float kCubeSize = 1.0f;
constexpr float kSpinRadiansPerSecond = 0.6f;
constexpr float kStrafeAmplitude = 3.0f;
constexpr float kStrafeRadiansPerSecond = 0.8f;

}  // namespace

void Simulation::init() {
    // Same world construction the client uses for prediction. It holds static
    // geometry only -- characters are moved by the controller, not solved.
    mWorld = gamesim::createWorld();
    mGround = gamesim::createGround(mWorld);
    gamesim::finalizeWorld(mWorld);
    mCubePosition = {0.0f, kCubeSize * 0.5f, 0.0f};
}

void Simulation::shutdown() {
    if (b3World_IsValid(mWorld)) {
        // Destroying the world releases every body and shape in it.
        b3DestroyWorld(mWorld);
        mWorld = b3_nullWorldId;
    }
    mPlayers.clear();
}

uint32_t Simulation::addPlayer(uint64_t playerId) {
    auto existing = mPlayers.find(playerId);
    if (existing != mPlayers.end()) {
        return existing->second.entityId;
    }

    Player player;
    player.entityId = mNextPlayerEntityId++;

    // Spread spawns out so two players don't appear in the same spot.
    const float angle = static_cast<float>(mPlayers.size()) * 1.2f;
    player.character.position = {std::cos(angle) * 3.0f, kSpawnHeight,
        6.0f + std::sin(angle) * 2.0f};

    mPlayers.emplace(playerId, player);
    return player.entityId;
}

void Simulation::removePlayer(uint64_t playerId) {
    mPlayers.erase(playerId);
}

void Simulation::setInput(uint64_t playerId, const PlayerInput& input) {
    auto it = mPlayers.find(playerId);
    if (it == mPlayers.end()) {
        return;
    }
    // Ignore inputs that arrive out of order: UDP makes no promises, and an
    // older intent must not overwrite a newer one.
    if (input.sequence != 0 && input.sequence < it->second.input.sequence) {
        return;
    }
    it->second.input = input;
}

void Simulation::step(float deltaSeconds) {
    mElapsed += deltaSeconds;
    ++mTick;

    // --- Players ---------------------------------------------------------
    // The movement rule itself lives in common/gamesim so the client can run
    // exactly the same one when predicting. Nothing is solved: the controller
    // moves each transform directly.
    for (auto& [playerId, player] : mPlayers) {
        gamesim::stepCharacter(mWorld, player.character, player.input, deltaSeconds);
        player.lastInputSequence = player.input.sequence;

        // The jump is consumed: holding the key must not make the player hop
        // continuously without the client resending the intent.
        player.input.jump = false;
    }

    // --- Cube ------------------------------------------------------------
    const double phase = mElapsed * kStrafeRadiansPerSecond;
    mCubePosition.x = kStrafeAmplitude * static_cast<float>(std::sin(phase));
    mCubeVelocity.x = kStrafeAmplitude * kStrafeRadiansPerSecond
            * static_cast<float>(std::cos(phase));

    const double angle = mElapsed * kSpinRadiansPerSecond;
    mCubeRotation.x = 0.0f;
    mCubeRotation.y = static_cast<float>(std::sin(angle * 0.5));
    mCubeRotation.z = 0.0f;
    mCubeRotation.w = static_cast<float>(std::cos(angle * 0.5));
}

void Simulation::buildSnapshot(net::Snapshot* out) const {
    out->tick = mTick;
    out->entities.clear();
    out->entities.reserve(mPlayers.size() + 1);

    net::EntityState cube;
    cube.id = kCubeEntityId;
    cube.type = static_cast<uint8_t>(net::EntityType::Cube);
    cube.position = mCubePosition;
    cube.rotation = mCubeRotation;
    cube.velocity = mCubeVelocity;
    cube.angularVelocity = {0.0f, kSpinRadiansPerSecond, 0.0f};
    out->entities.push_back(cube);

    for (const auto& [playerId, player] : mPlayers) {
        const gamesim::Vec3& position = player.character.position;
        const gamesim::Vec3& velocity = player.character.velocity;

        net::EntityState state;
        state.id = player.entityId;
        state.type = static_cast<uint8_t>(net::EntityType::Player);
        state.position = {position.x, position.y, position.z};
        // The capsule's physical rotation is locked upright, so the visible
        // facing comes from the player's own yaw.
        state.rotation = {0.0f, std::sin(player.input.yaw * 0.5f), 0.0f,
            std::cos(player.input.yaw * 0.5f)};
        state.velocity = {velocity.x, velocity.y, velocity.z};
        // The acknowledgement the client reconciles against.
        state.lastInputSequence = player.lastInputSequence;
        out->entities.push_back(state);
    }
}

}  // namespace world
