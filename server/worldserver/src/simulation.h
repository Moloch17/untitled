#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include <box3d/box3d.h>
#include <gamesim/character.h>
#include <net/protocol.h>
#include <net/snapshot.h>

namespace world {

constexpr uint32_t kCubeEntityId = 1;
// Player entity ids start above the scenery so the two can't collide.
constexpr uint32_t kFirstPlayerEntityId = 1000;

// Movement intent from a client, as defined by the shared simulation.
using PlayerInput = gamesim::CharacterInput;

// The authoritative world simulation, including physics.
//
// Player movement, ground collision and jumping all happen here and nowhere
// else -- the client sends intent and renders what comes back, so it cannot
// place itself anywhere the server didn't agree to.
class Simulation {
public:
    // dayLengthSeconds is how long one full day/night cycle takes. Zero or
    // less follows the wall clock instead, so in-game noon is real noon.
    void init(float dayLengthSeconds);
    void shutdown();

    // Adds a player capsule at a spawn point. Returns its entity id.
    uint32_t addPlayer(uint64_t playerId);
    void removePlayer(uint64_t playerId);

    // Latest intent for a player, applied on the next step.
    void setInput(uint64_t playerId, const PlayerInput& input);

    void step(float deltaSeconds);
    void buildSnapshot(net::Snapshot* out) const;

    uint32_t tick() const { return mTick; }
    float timeOfDay() const { return mTimeOfDay; }
    size_t playerCount() const { return mPlayers.size(); }

    // Jumps the world clock. In wall-clock mode this is stored as an offset, so
    // time keeps advancing from where it was set rather than freezing.
    void setTimeOfDay(float timeOfDay);
    // Drops any offset and follows real time again.
    void followRealTime();

private:
    struct Player {
        uint32_t entityId = 0;
        // Moved directly by the shared controller; not a rigid body.
        gamesim::Character character;
        PlayerInput input;
        // Echoed back in snapshots so the client knows which of its predicted
        // inputs this state already includes.
        uint32_t lastInputSequence = 0;
    };

    std::map<uint64_t, Player> mPlayers;
    uint32_t mNextPlayerEntityId = kFirstPlayerEntityId;

    // The cube is scripted rather than physical: its motion is a closed-form
    // function of time, so it can't drift and survives a restart identically.
    net::Vec3 mCubePosition;
    net::Quat mCubeRotation;
    net::Vec3 mCubeVelocity;

    uint32_t mTick = 0;
    double mElapsed = 0.0;

    // Wraps in [0,1). Starts at morning so a fresh server isn't pitch black.
    float mTimeOfDay = 0.3f;
    // Zero or less means "follow the wall clock".
    float mDayLengthSeconds = 0.0f;
    // Added to wall-clock time, wrapped. Set by the console.
    float mTimeOffset = 0.0f;
};

}  // namespace world
