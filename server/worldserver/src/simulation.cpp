#include "simulation.h"

#include <chrono>
#include <cmath>
#include <ctime>

namespace world {

namespace {

constexpr float kSpawnClearance = 0.2f;

// --- Cube ------------------------------------------------------------------
constexpr float kCubeSize = 1.0f;
constexpr float kSpinRadiansPerSecond = 0.6f;
constexpr float kStrafeAmplitude = 3.0f;
constexpr float kStrafeRadiansPerSecond = 0.8f;

}  // namespace

namespace {

// Seconds since the epoch, as a double so long uptimes keep sub-second
// resolution.
double realSeconds() {
    return std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
}

// Fraction of the day elapsed by the server's local wall-clock time, so that
// in-game noon is real noon. Local rather than UTC -- inside a container that
// means the TZ the container was given; see compose.yaml.
float wallClockTimeOfDay() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const int secondsToday = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    return static_cast<float>(secondsToday) / 86400.0f;
}

}  // namespace

void Simulation::init(float timeSpeed) {
    mTimeSpeed = timeSpeed > 0.0f ? timeSpeed : 1.0f;
    resetClock();

    // The ground is the shared terrain heightfield; there is no collision world
    // to build.
    mCubePosition = {0.0f, terrain::heightAt(0.0f, 0.0f) + kCubeSize * 0.5f, 0.0f};
}

void Simulation::shutdown() {
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
    const float spawnX = std::cos(angle) * 3.0f;
    const float spawnZ = 6.0f + std::sin(angle) * 2.0f;
    // Drop in from just above the ground rather than a fixed height, which on
    // a hill would be either underground or a long fall.
    player.character.position = {spawnX,
        terrain::heightAt(spawnX, spawnZ) + gamesim::kRestHeight + kSpawnClearance, spawnZ};

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

    // Derived from the anchor rather than accumulated, so it stays exact over
    // long uptimes and across a pause.
    const double elapsed = realSeconds() - mAnchorRealSeconds;
    mTimeOfDay = mAnchorTimeOfDay
            + static_cast<float>(elapsed * static_cast<double>(mTimeSpeed) / 86400.0);
    mTimeOfDay -= std::floor(mTimeOfDay);

    // --- Players ---------------------------------------------------------
    // The movement rule itself lives in common/gamesim so the client can run
    // exactly the same one when predicting. Nothing is solved: the controller
    // moves each transform directly.
    for (auto& [playerId, player] : mPlayers) {
        gamesim::stepCharacter(player.character, player.input, deltaSeconds);
        player.lastInputSequence = player.input.sequence;

        // The jump is consumed: holding the key must not make the player hop
        // continuously without the client resending the intent.
        player.input.jump = false;
    }

    // --- Cube ------------------------------------------------------------
    const double phase = mElapsed * kStrafeRadiansPerSecond;
    mCubePosition.x = kStrafeAmplitude * static_cast<float>(std::sin(phase));
    // Ride the terrain rather than hovering at a fixed height.
    mCubePosition.y = terrain::heightAt(mCubePosition.x, mCubePosition.z) + kCubeSize * 0.5f;
    mCubeVelocity.x = kStrafeAmplitude * kStrafeRadiansPerSecond
            * static_cast<float>(std::cos(phase));

    const double angle = mElapsed * kSpinRadiansPerSecond;
    mCubeRotation.x = 0.0f;
    mCubeRotation.y = static_cast<float>(std::sin(angle * 0.5));
    mCubeRotation.z = 0.0f;
    mCubeRotation.w = static_cast<float>(std::cos(angle * 0.5));
}

void Simulation::reanchor() {
    mAnchorRealSeconds = realSeconds();
    mAnchorTimeOfDay = mTimeOfDay;
}

void Simulation::setTimeOfDay(float timeOfDay) {
    timeOfDay -= std::floor(timeOfDay);
    mTimeOfDay = timeOfDay;
    reanchor();
}

void Simulation::setTimeSpeed(float speed) {
    // Re-anchor first: the new rate applies from now, so changing speed never
    // makes the clock jump.
    reanchor();
    mTimeSpeed = speed;
}

void Simulation::resetClock() {
    mTimeSpeed = 1.0f;
    mTimeOfDay = wallClockTimeOfDay();
    reanchor();
}

void Simulation::buildSnapshot(net::Snapshot* out) const {
    out->tick = mTick;
    out->timeOfDay = mTimeOfDay;
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
        // The body's own facing, which follows movement rather than the
        // camera -- looking around while standing still must not spin it.
        // Negated for the same reason as the client: a +Y rotation by f maps
        // the mesh's forward (0,0,-1) to (-sin f, 0, -cos f), but movement
        // treats forward as (+sin f, 0, -cos f).
        const float facing = player.character.facingYaw;
        state.rotation = {0.0f, std::sin(-facing * 0.5f), 0.0f, std::cos(facing * 0.5f)};
        state.velocity = {velocity.x, velocity.y, velocity.z};
        // The acknowledgement the client reconciles against.
        state.lastInputSequence = player.lastInputSequence;
        out->entities.push_back(state);
    }
}

}  // namespace world
