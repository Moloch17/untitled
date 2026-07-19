#pragma once

#include <math/vec3.h>

namespace game {

// Everything the renderer needs to draw one moment of the day.
//
// This is purely presentation: the time itself is owned by the server and
// arrives in every snapshot. Nothing here feeds back into gameplay, so a client
// cannot gain anything by lying about what time it thinks it is.
struct SkyState {
    // Unit vectors pointing from the world toward each body.
    filament::math::float3 sunDirection{0.0f, 1.0f, 0.0f};
    filament::math::float3 moonDirection{0.0f, -1.0f, 0.0f};

    // Direction the light travels, colour, and brightness of whichever body is
    // currently lighting the scene.
    filament::math::float3 lightDirection{0.0f, -1.0f, 0.0f};
    filament::math::float3 lightColor{1.0f, 1.0f, 1.0f};
    float lightIntensity = 0.0f;

    filament::math::float3 skyColor{0.0f, 0.0f, 0.0f};
    float ambientIntensity = 0.0f;

    filament::math::float3 sunColor{1.0f, 1.0f, 1.0f};
    filament::math::float3 moonColor{1.0f, 1.0f, 1.0f};

    bool sunVisible = false;
    bool moonVisible = false;
};

// Maps a time of day in [0,1) -- 0 midnight, 0.25 sunrise, 0.5 noon, 0.75
// sunset -- onto the lighting and sky above.
SkyState evaluateSky(float timeOfDay);

}  // namespace game
