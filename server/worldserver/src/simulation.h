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
    // timeSpeed is how fast the world clock runs relative to real time: 1 is
    // real time, so in-game noon is real noon; 24 gives a one hour day.
    void init(float timeSpeed);
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

    // Jumps the world clock, which then keeps running from there.
    void setTimeOfDay(float timeOfDay);
    // How fast the clock runs relative to real time. 1 is real time, 0 freezes.
    void setTimeSpeed(float speed);
    float timeSpeed() const { return mTimeSpeed; }
    // Back to defaults: real time of day, running at real speed.
    void resetClock();

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

    // Wraps in [0,1).
    float mTimeOfDay = 0.3f;

    // The clock is an anchor plus a rate rather than an accumulator: world time
    // is a function of how much real time has passed since the anchor. That
    // keeps it exact across pauses and lets `speed` change without the clock
    // jumping, since changing it re-anchors at the current time.
    double mAnchorRealSeconds = 0.0;
    float mAnchorTimeOfDay = 0.0f;
    float mTimeSpeed = 1.0f;

    void reanchor();
};

}  // namespace world
