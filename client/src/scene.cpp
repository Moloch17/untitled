#include "scene.h"

#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <math/mat4.h>
#include <utils/EntityManager.h>

#include "clientmaterials.h"

using namespace filament;
using namespace filament::math;

namespace game {

namespace {

constexpr float kPlaneSize = 20.0f;
constexpr float kCubeSize = 1.0f;

// Must match the server's collider (see worldserver simulation.cpp), otherwise
// players visibly float above or sink into the ground.
constexpr float kPlayerRadius = 0.35f;
constexpr float kPlayerHalfHeight = 0.5f;

// The sun and moon sit far enough out to read as sky rather than scenery, and
// are drawn much larger than life so they're legible at that distance.
// How far shadows are drawn. Cascades divide this range, so it is the main
// control over texel density: the whole 1km terrain would give metres per
// texel, while this keeps them small where the player is actually looking.
// Past it, haze has taken the detail anyway.
constexpr float kShadowDistance = 120.0f;
constexpr float kNearPlaneForShadows = 0.5f;

constexpr float kCelestialDistance = 260.0f;
constexpr float kCelestialRadius = 9.0f;

}  // namespace

void DemoScene::build(Engine* engine, Scene* scene) {
    if (mLoaded) {
        return;
    }
    auto& entityManager = utils::EntityManager::get();

    // Shaders are baked into the executable by resgen at build time; see
    // client/CMakeLists.txt.
    mMaterial = Material::Builder()
            .package(CLIENTMATERIALS_LIT_DATA, CLIENTMATERIALS_LIT_SIZE)
            .build(*engine);

    mPlaneMaterial = mMaterial->createInstance();
    mPlaneMaterial->setParameter("baseColor", RgbType::LINEAR, float3{0.16f, 0.20f, 0.12f});
    mPlaneMaterial->setParameter("roughness", 0.95f);
    mPlaneMaterial->setParameter("metallic", 0.0f);

    mCubeMaterial = mMaterial->createInstance();
    mCubeMaterial->setParameter("baseColor", RgbType::LINEAR, float3{0.8f, 0.22f, 0.18f});
    mCubeMaterial->setParameter("roughness", 0.4f);
    mCubeMaterial->setParameter("metallic", 0.0f);

    mPlaneMesh = createTerrain(engine);
    mCubeMesh = createCube(engine, kCubeSize);
    mCapsuleMesh = createCapsule(engine, kPlayerRadius, kPlayerHalfHeight);

    mPlane = entityManager.create();
    RenderableManager::Builder(1)
            .boundingBox(mPlaneMesh.boundingBox)
            .material(0, mPlaneMaterial)
            .geometry(0, RenderableManager::PrimitiveType::TRIANGLES,
                    mPlaneMesh.vertexBuffer, mPlaneMesh.indexBuffer)
            .receiveShadows(true)
            .castShadows(true)
            .build(*engine, mPlane);
    scene->addEntity(mPlane);

    mCube = entityManager.create();
    RenderableManager::Builder(1)
            .boundingBox(mCubeMesh.boundingBox)
            .material(0, mCubeMaterial)
            .geometry(0, RenderableManager::PrimitiveType::TRIANGLES,
                    mCubeMesh.vertexBuffer, mCubeMesh.indexBuffer)
            .receiveShadows(true)
            .castShadows(true)
            .build(*engine, mCube);
    scene->addEntity(mCube);

    // Rest the cube on the plane rather than intersecting it.
    auto& transformManager = engine->getTransformManager();
    transformManager.setTransform(transformManager.getInstance(mCube),
            mat4f::translation(float3{0.0f, kCubeSize * 0.5f, 0.0f}));

    // Key light. Its direction, colour and intensity are driven by the day
    // cycle; see applySky().
    //
    // Shadow settings matter far more here than they did over a small plane: a
    // single map stretched across a kilometre of terrain gives each texel
    // metres of ground, which is what makes edges crawl.
    //
    // These follow Filament's own sample defaults (samples/material_sandbox,
    // Config.h): 2048 maps, lispsm on, stable off, normalBias 1.0,
    // constantBias 0.001. `stable` was tried and reverted -- the header is
    // explicit that it "disables all resolution enhancing features", and the
    // resulting maps were too coarse to hold a shadow for anything the size of
    // a character.
    //
    // Cascades are what actually fix the crawl: they concentrate resolution
    // near the viewer instead of spreading it over the whole world.
    LightManager::ShadowOptions shadowOptions;
    shadowOptions.mapSize = 2048;
    shadowOptions.shadowCascades = 4;
    // lambda leans toward a logarithmic split, which puts more resolution
    // close to the camera where shadow edges are actually examined.
    LightManager::ShadowCascades::computePracticalSplits(shadowOptions.cascadeSplitPositions,
            /*cascades=*/4, kNearPlaneForShadows, kShadowDistance, /*lambda=*/0.9f);
    // Off, per Filament's samples: it trades away the resolution this needs.
    shadowOptions.stable = false;
    // Light-space perspective shadow maps: more texels where the camera is
    // looking, for free.
    shadowOptions.lispsm = true;
    // Beyond this, haze has taken the detail anyway.
    shadowOptions.shadowFar = kShadowDistance;
    // The header says this "should be 1.0" -- it scales Filament's own error
    // estimate, and raising it detaches shadows from small objects.
    shadowOptions.normalBias = 1.0f;
    shadowOptions.constantBias = 0.001f;

    // Screen-space contact shadows. Cascaded maps lose the fine contact where
    // an object meets the ground -- the cube and capsule end up looking like
    // they hover. This traces a short ray in screen space to put that contact
    // back, at a cost that scales with step count rather than scene size.
    shadowOptions.screenSpaceContactShadows = true;
    shadowOptions.stepCount = 8;
    shadowOptions.maxShadowDistance = 0.4f;

    mSun = entityManager.create();
    LightManager::Builder(LightManager::Type::DIRECTIONAL)
            .color(Color::toLinear<ACCURATE>(sRGBColor{1.0f, 0.96f, 0.90f}))
            .intensity(90000.0f)
            .direction({0.6f, -1.0f, -0.8f})
            .castShadows(true)
            .shadowOptions(shadowOptions)
            .build(*engine, mSun);
    scene->addEntity(mSun);

    // Ambient fill. Filament supports only one directional light per scene, so
    // this can't be a second sun -- it's a single-band spherical harmonic,
    // i.e. constant sky-coloured irradiance from every direction. Without it,
    // shadows and surfaces facing away from the sun render pure black.
    const float3 ambient[1] = {{0.10f, 0.13f, 0.19f}};
    mIndirectLight = IndirectLight::Builder()
            .irradiance(1, ambient)
            .intensity(30000.0f)
            .build(*engine);
    scene->setIndirectLight(mIndirectLight);

    // Sun and moon bodies. Unlit, because they are the light: shading them by
    // the scene's own lighting would black the sun out at dusk.
    mCharacterBase = Material::Builder()
            .package(CLIENTMATERIALS_CHARACTER_DATA, CLIENTMATERIALS_CHARACTER_SIZE)
            .build(*engine);

    mUnlitBase = Material::Builder()
            .package(CLIENTMATERIALS_UNLIT_DATA, CLIENTMATERIALS_UNLIT_SIZE)
            .build(*engine);
    mDiscMesh = createSphere(engine, kCelestialRadius);

    mSunMaterial = mUnlitBase->createInstance();
    mMoonMaterial = mUnlitBase->createInstance();

    mSunDisc = entityManager.create();
    RenderableManager::Builder(1)
            .boundingBox(mDiscMesh.boundingBox)
            .material(0, mSunMaterial)
            .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, mDiscMesh.vertexBuffer,
                    mDiscMesh.indexBuffer)
            .castShadows(false)
            .receiveShadows(false)
            .build(*engine, mSunDisc);

    mMoonDisc = entityManager.create();
    RenderableManager::Builder(1)
            .boundingBox(mDiscMesh.boundingBox)
            .material(0, mMoonMaterial)
            .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, mDiscMesh.vertexBuffer,
                    mDiscMesh.indexBuffer)
            .castShadows(false)
            .receiveShadows(false)
            .build(*engine, mMoonDisc);

    mLoaded = true;
}

void DemoScene::setEntityTransform(Engine* engine, uint32_t entityId, const net::Vec3& position,
        const net::Quat& rotation) {
    if (!mLoaded || entityId != 1) {
        return;  // only the cube is replicated so far
    }

    auto& transformManager = engine->getTransformManager();
    const auto instance = transformManager.getInstance(mCube);

    const quatf orientation{rotation.w, rotation.x, rotation.y, rotation.z};
    const mat4f transform = mat4f::translation(float3{position.x, position.y, position.z})
            * mat4f(orientation);
    transformManager.setTransform(instance, transform);
}

void DemoScene::applySky(Engine* engine, Scene* scene, const SkyState& sky) {
    if (!mLoaded) {
        return;
    }

    auto& lightManager = engine->getLightManager();
    const auto sun = lightManager.getInstance(mSun);
    lightManager.setDirection(sun, sky.lightDirection);
    lightManager.setColor(sun, sky.lightColor);
    lightManager.setIntensity(sun, sky.lightIntensity);

    // Only the intensity of the ambient term is animated. Its colour is baked
    // into spherical harmonics at build time, and rebuilding an IndirectLight
    // every frame would churn driver handles for a difference the sky colour
    // already carries.
    mIndirectLight->setIntensity(sky.ambientIntensity);

    auto& transformManager = engine->getTransformManager();
    const auto place = [&](utils::Entity entity, const filament::math::float3& direction) {
        transformManager.setTransform(transformManager.getInstance(entity),
                mat4f::translation(direction * kCelestialDistance));
    };
    place(mSunDisc, sky.sunDirection);
    place(mMoonDisc, sky.moonDirection);

    mSunMaterial->setParameter("baseColor", RgbType::LINEAR, sky.sunColor);
    mMoonMaterial->setParameter("baseColor", RgbType::LINEAR, sky.moonColor);

    // The ground plane is finite, so a body below the horizon would otherwise
    // be visible past its edge. Add and remove them instead of just moving
    // them, and only when the state actually changes.
    if (sky.sunVisible != mSunDiscInScene) {
        sky.sunVisible ? scene->addEntity(mSunDisc) : scene->remove(mSunDisc);
        mSunDiscInScene = sky.sunVisible;
    }
    if (sky.moonVisible != mMoonDiscInScene) {
        sky.moonVisible ? scene->addEntity(mMoonDisc) : scene->remove(mMoonDisc);
        mMoonDiscInScene = sky.moonVisible;
    }
}

void DemoScene::setPlayerTransform(Engine* engine, Scene* scene, uint32_t entityId,
        bool isLocalPlayer, const net::Vec3& position, const net::Quat& rotation) {
    if (!mLoaded) {
        return;
    }

    auto it = mPlayers.find(entityId);
    if (it == mPlayers.end()) {
        PlayerVisual visual;
        visual.material = mCharacterBase->createInstance();
        // Checker in two tones of the player's colour, so rotation is readable,
        // with a contrasting plus marking the forward face. The local player is
        // tinted differently so it's obvious which capsule the camera follows.
        const float3 light = isLocalPlayer ? float3{0.30f, 0.55f, 0.85f}
                                           : float3{0.85f, 0.72f, 0.25f};
        const float3 dark = light * 0.35f;
        visual.material->setParameter("colorA", RgbType::LINEAR, light);
        visual.material->setParameter("colorB", RgbType::LINEAR, dark);
        visual.material->setParameter("markColor", RgbType::LINEAR, float3{0.95f, 0.95f, 0.95f});

        visual.entity = utils::EntityManager::get().create();
        RenderableManager::Builder(1)
                .boundingBox(mCapsuleMesh.boundingBox)
                .material(0, visual.material)
                .geometry(0, RenderableManager::PrimitiveType::TRIANGLES,
                        mCapsuleMesh.vertexBuffer, mCapsuleMesh.indexBuffer)
                .receiveShadows(true)
                .castShadows(true)
                .build(*engine, visual.entity);
        scene->addEntity(visual.entity);

        it = mPlayers.emplace(entityId, visual).first;
    }

    auto& transformManager = engine->getTransformManager();
    const quatf orientation{rotation.w, rotation.x, rotation.y, rotation.z};
    transformManager.setTransform(transformManager.getInstance(it->second.entity),
            mat4f::translation(float3{position.x, position.y, position.z}) * mat4f(orientation));
}

void DemoScene::removeAbsentPlayers(Engine* engine, Scene* scene,
        const std::set<uint32_t>& present) {
    for (auto it = mPlayers.begin(); it != mPlayers.end();) {
        if (present.count(it->first)) {
            ++it;
            continue;
        }
        scene->remove(it->second.entity);
        engine->destroy(it->second.entity);
        utils::EntityManager::get().destroy(it->second.entity);
        engine->destroy(it->second.material);
        it = mPlayers.erase(it);
    }
}

void DemoScene::destroy(Engine* engine, Scene* scene) {
    if (!mLoaded) {
        return;
    }
    auto& entityManager = utils::EntityManager::get();

    for (utils::Entity entity : {mPlane, mCube, mSun, mSunDisc, mMoonDisc}) {
        scene->remove(entity);
        engine->destroy(entity);
        entityManager.destroy(entity);
    }
    mSunDiscInScene = false;
    mMoonDiscInScene = false;

    scene->setIndirectLight(nullptr);
    engine->destroy(mIndirectLight);

    removeAbsentPlayers(engine, scene, {});

    mPlaneMesh.destroy(engine);
    mCubeMesh.destroy(engine);
    mCapsuleMesh.destroy(engine);
    mDiscMesh.destroy(engine);

    engine->destroy(mPlaneMaterial);
    engine->destroy(mCubeMaterial);
    engine->destroy(mSunMaterial);
    engine->destroy(mMoonMaterial);
    engine->destroy(mMaterial);
    engine->destroy(mUnlitBase);
    engine->destroy(mCharacterBase);

    mPlaneMaterial = nullptr;
    mCubeMaterial = nullptr;
    mMaterial = nullptr;
    mIndirectLight = nullptr;
    mLoaded = false;
}

}  // namespace game
