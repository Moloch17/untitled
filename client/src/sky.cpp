#include "sky.h"

#include <cmath>

#include <math/scalar.h>

using namespace filament::math;

namespace game {

namespace {

// The sun's arc is tilted off the axes so shadows sweep diagonally across the
// ground rather than sliding straight along it.
constexpr float kArcAzimuth = 0.6f;

// Peak brightness at noon, and the far dimmer moonlit night.
constexpr float kSunIntensity = 100000.0f;
constexpr float kMoonIntensity = 9000.0f;
constexpr float kDayAmbient = 30000.0f;
// Night is lit well above black on purpose. A scene the player cannot read is
// not atmospheric, it is broken, so this is the floor the ambient never goes
// below rather than a value it passes through.
constexpr float kNightAmbient = 9000.0f;

const float3 kNoonLight{1.0f, 0.96f, 0.90f};
const float3 kHorizonLight{1.0f, 0.52f, 0.26f};
const float3 kMoonLight{0.55f, 0.68f, 1.0f};

const float3 kDaySky{0.10f, 0.22f, 0.45f};
const float3 kNightSky{0.020f, 0.028f, 0.055f};
const float3 kTwilightSky{0.30f, 0.12f, 0.07f};

const float3 kSunDisc{1.0f, 0.95f, 0.75f};
const float3 kMoonDisc{0.80f, 0.85f, 0.95f};

float smoothstep(float edge0, float edge1, float x) {
    const float t = clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// filament::math supplies mix() for float3; only the scalar smoothstep above
// is ours.

}  // namespace

SkyState evaluateSky(float timeOfDay) {
    SkyState sky;

    // Angle measured so that sunrise (0.25) puts the sun on the horizon and
    // noon (0.5) puts it overhead.
    const float angle = 2.0f * static_cast<float>(F_PI) * (timeOfDay - 0.25f);
    const float elevation = std::sin(angle);
    const float horizontal = std::cos(angle);

    sky.sunDirection = normalize(float3{horizontal * std::cos(kArcAzimuth), elevation,
        horizontal * std::sin(kArcAzimuth)});
    // The moon rides the opposite end of the same arc, so one is always up.
    sky.moonDirection = -sky.sunDirection;

    const float sunHeight = sky.sunDirection.y;
    const float moonHeight = sky.moonDirection.y;

    // How much of full daylight we're in. The lower edge is slightly below the
    // horizon so the light fades out before the sun visually touches it.
    const float dayAmount = smoothstep(-0.10f, 0.28f, sunHeight);

    // Peaks as either body crosses the horizon, which is what tints sunrise and
    // sunset without needing separate cases for them.
    const float horizonCloseness = std::exp(-(sunHeight * sunHeight) / (0.16f * 0.16f));

    sky.sunColor = kSunDisc;
    sky.moonColor = kMoonDisc;
    sky.sunVisible = sunHeight > -0.12f;
    sky.moonVisible = moonHeight > -0.12f;

    if (sunHeight > 0.0f) {
        // Daytime: the sun lights the scene, reddening as it nears the horizon.
        sky.lightDirection = -sky.sunDirection;
        sky.lightColor = mix(kHorizonLight, kNoonLight, smoothstep(0.0f, 0.35f, sunHeight));
        sky.lightIntensity = kSunIntensity * smoothstep(-0.02f, 0.25f, sunHeight);
    } else {
        // Night: the moon takes over, far dimmer and cooler.
        sky.lightDirection = -sky.moonDirection;
        sky.lightColor = kMoonLight;
        sky.lightIntensity = kMoonIntensity * smoothstep(-0.02f, 0.25f, moonHeight);
    }

    sky.skyColor = mix(kNightSky, kDaySky, dayAmount);
    // Warm the sky through dawn and dusk, strongest right at the horizon.
    sky.skyColor = mix(sky.skyColor, kTwilightSky, horizonCloseness * 0.75f);

    sky.ambientIntensity = kNightAmbient + (kDayAmbient - kNightAmbient) * dayAmount;
    // Belt and braces: whatever the curve does above, never let it fall to a
    // level where the world reads as black.
    if (sky.ambientIntensity < kNightAmbient) {
        sky.ambientIntensity = kNightAmbient;
    }

    // Haze is a slightly desaturated, brighter version of the sky: air scatters
    // blue toward the viewer, so distant ground drifts toward the sky's colour
    // rather than simply darkening.
    sky.fogColor = mix(sky.skyColor * 1.15f + float3{0.03f, 0.05f, 0.08f}, kTwilightSky,
            horizonCloseness * 0.5f);
    // Light enough to read as distance rather than a wall. Haze should suggest
    // depth, not hide the terrain a few hundred metres out.
    sky.fogDensity = 0.0006f + 0.0004f * (1.0f - dayAmount);

    return sky;
}

}  // namespace game
