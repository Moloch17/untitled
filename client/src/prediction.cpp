#include "prediction.h"

#include <cmath>

#include <net/protocol.h>

namespace game {

namespace {

// Below this, a disagreement isn't worth correcting: snapping the character
// around for a millimetre of float drift looks worse than the drift.
constexpr float kPositionTolerance = 0.02f;

// Corrections are eased out over roughly this long.
constexpr float kSmoothingHalfLife = 0.08f;

// A correction larger than this is a teleport, a respawn, or a client that has
// fallen badly out of sync. Easing that would drag the character across the
// map, so it's applied instantly instead.
constexpr float kMaxSmoothedCorrection = 2.0f;

// One simulation tick, matching the server's rate exactly. Predicting at the
// render framerate instead would diverge immediately.
constexpr float kTickSeconds = 1.0f / static_cast<float>(net::kServerTickHz);

float distance(const gamesim::Vec3& a, const gamesim::Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

void Prediction::init(gamesim::Vec3 spawnPosition) {
    shutdown();

    // A private world holding just the static ground, used for the controller's
    // ground queries. Other players aren't here -- they're replicated visuals,
    // and predicting them would mean predicting their input too.
    mWorld = gamesim::createWorld();
    mGround = gamesim::createGround(mWorld);
    gamesim::finalizeWorld(mWorld);

    mCharacter = {};
    mCharacter.position = spawnPosition;

    mPending.clear();
    mNextSequence = 1;
    mSmoothingOffset = {};
    mRollbacks = 0;
    mLastCorrection = 0.0f;
    mActive = true;
}

void Prediction::shutdown() {
    if (b3World_IsValid(mWorld)) {
        b3DestroyWorld(mWorld);
    }
    mWorld = b3_nullWorldId;
    mGround = b3_nullBodyId;
    mCharacter = {};
    mPending.clear();
    mActive = false;
}

void Prediction::simulateTick(const gamesim::CharacterInput& input) {
    gamesim::stepCharacter(mWorld, mCharacter, input, kTickSeconds);
}

gamesim::CharacterInput Prediction::step(float moveForward, float moveRight, float yaw, bool jump,
        bool sprint) {
    gamesim::CharacterInput input;
    input.sequence = mNextSequence++;
    input.moveForward = moveForward;
    input.moveRight = moveRight;
    input.yaw = yaw;
    input.jump = jump;
    input.sprint = sprint;

    if (!mActive) {
        return input;
    }

    // Apply immediately: this is what makes the character respond on the frame
    // the key is pressed rather than a round trip later.
    simulateTick(input);
    mPending.push_back(input);

    // Bound the history. If this fills, the server has stopped acknowledging
    // and replaying further would cost more than it's worth.
    constexpr size_t kMaxPending = 256;
    while (mPending.size() > kMaxPending) {
        mPending.pop_front();
    }

    return input;
}

void Prediction::reconcile(const net::EntityState& authoritative) {
    if (!mActive) {
        return;
    }

    // Everything up to and including the acknowledged sequence is already
    // accounted for by the authoritative state.
    const uint32_t acked = authoritative.lastInputSequence;
    while (!mPending.empty() && mPending.front().sequence <= acked) {
        mPending.pop_front();
    }

    // Where we currently think we are, before touching anything.
    const gamesim::Vec3 predictedBefore = mCharacter.position;

    // Rewind to the server's state. With a kinematic character this is the
    // whole state, so nothing can be left behind out of sync.
    mCharacter.position = {authoritative.position.x, authoritative.position.y,
        authoritative.position.z};
    mCharacter.velocity = {authoritative.velocity.x, authoritative.velocity.y,
        authoritative.velocity.z};

    // ...then replay the inputs the server hasn't seen yet, which brings the
    // character back to the present along a path the server would agree with.
    for (const gamesim::CharacterInput& input : mPending) {
        simulateTick(input);
    }

    const gamesim::Vec3 predictedAfter = mCharacter.position;
    const float error = distance(predictedBefore, predictedAfter);
    mLastCorrection = error;

    if (error <= kPositionTolerance) {
        return;  // agreement, near enough; nothing to smooth
    }

    ++mRollbacks;

    if (error > kMaxSmoothedCorrection) {
        // Too far to ease; accept the jump.
        mSmoothingOffset = {};
        return;
    }

    // Keep drawing the character where it was, and let that offset decay, so a
    // correction reads as a slight drift rather than a snap.
    mSmoothingOffset.x += predictedBefore.x - predictedAfter.x;
    mSmoothingOffset.y += predictedBefore.y - predictedAfter.y;
    mSmoothingOffset.z += predictedBefore.z - predictedAfter.z;
}

void Prediction::updateSmoothing(float deltaSeconds) {
    // Exponential decay, framerate independent.
    const float decay = std::exp2(-deltaSeconds / kSmoothingHalfLife);
    mSmoothingOffset.x *= decay;
    mSmoothingOffset.y *= decay;
    mSmoothingOffset.z *= decay;
}

gamesim::Vec3 Prediction::renderPosition() const {
    if (!mActive) {
        return {};
    }
    return {mCharacter.position.x + mSmoothingOffset.x,
        mCharacter.position.y + mSmoothingOffset.y,
        mCharacter.position.z + mSmoothingOffset.z};
}

}  // namespace game
