#pragma once

#include <cstdint>
#include <deque>

#include <gamesim/character.h>
#include <net/snapshot.h>

namespace game {

// Client-side prediction with rollback.
//
// The client can't wait for the server to answer before moving: that would put
// a full round trip between pressing W and the character responding. Instead it
// applies input locally straight away using the same rules the server will, and
// keeps every input it hasn't seen acknowledged yet.
//
// When an authoritative state arrives it is stamped with the last input the
// server actually consumed. If the prediction disagreed, the local character is
// snapped back to the server's state and the still-unacknowledged inputs are
// replayed on top -- a rollback -- which brings it back to "now" along a path
// consistent with the server.
//
// Because characters are moved by the controller rather than solved as rigid
// bodies, their entire state is plain data: a rollback is an assignment, with
// nothing hidden inside a solver that also needs rewinding.
class Prediction {
public:
    void init(gamesim::Vec3 spawnPosition);
    void shutdown();
    bool active() const { return mActive; }

    // Runs one fixed tick locally. Returns the input, stamped with its
    // sequence number, for sending to the server.
    gamesim::CharacterInput step(float moveForward, float moveRight, float yaw, bool jump,
            bool sprint);

    // Applies an authoritative state and reconciles against it.
    void reconcile(const net::EntityState& authoritative);

    // Where to draw the local character. Includes the smoothing offset, so a
    // correction is eased out over a few frames rather than snapping.
    gamesim::Vec3 renderPosition() const;
    // Decays the visual correction offset. Call once per rendered frame.
    void updateSmoothing(float deltaSeconds);

    uint32_t pendingInputCount() const { return static_cast<uint32_t>(mPending.size()); }
    uint32_t rollbackCount() const { return mRollbacks; }
    float lastCorrection() const { return mLastCorrection; }

private:
    void simulateTick(const gamesim::CharacterInput& input);

    // Other players are deliberately absent from prediction -- predicting them
    // would mean predicting their input. The ground comes from the shared
    // terrain function, so there is nothing to build here.
    gamesim::Character mCharacter;
    bool mActive = false;

    // Inputs applied locally but not yet acknowledged by the server.
    std::deque<gamesim::CharacterInput> mPending;
    uint32_t mNextSequence = 1;

    // Difference between where the character was drawn before a correction and
    // where it ended up, decayed to zero so corrections don't pop.
    gamesim::Vec3 mSmoothingOffset;

    uint32_t mRollbacks = 0;
    float mLastCorrection = 0.0f;
};

}  // namespace game
