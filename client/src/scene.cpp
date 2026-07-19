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
    mPlaneMaterial->setParameter("baseColor", RgbType::LINEAR, float3{0.35f, 0.36f, 0.38f});
    mPlaneMaterial->setParameter("roughness", 0.85f);
    mPlaneMaterial->setParameter("metallic", 0.0f);

    mCubeMaterial = mMaterial->createInstance();
    mCubeMaterial->setParameter("baseColor", RgbType::LINEAR, float3{0.8f, 0.22f, 0.18f});
    mCubeMaterial->setParameter("roughness", 0.4f);
    mCubeMaterial->setParameter("metallic", 0.0f);

    mPlaneMesh = createPlane(engine, kPlaneSize);
    mCubeMesh = createCube(engine, kCubeSize);
    mCapsuleMesh = createCapsule(engine, kPlayerRadius, kPlayerHalfHeight);

    mPlane = entityManager.create();
    RenderableManager::Builder(1)
            .boundingBox(mPlaneMesh.boundingBox)
            .material(0, mPlaneMaterial)
            .geometry(0, RenderableManager::PrimitiveType::TRIANGLES,
                    mPlaneMesh.vertexBuffer, mPlaneMesh.indexBuffer)
            .culling(false)
            .receiveShadows(true)
            .castShadows(false)
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

    // Key light: casts the shadow that sells the cube sitting on the plane.
    mSun = entityManager.create();
    LightManager::Builder(LightManager::Type::DIRECTIONAL)
            .color(Color::toLinear<ACCURATE>(sRGBColor{1.0f, 0.96f, 0.90f}))
            .intensity(90000.0f)
            .direction({0.6f, -1.0f, -0.8f})
            .castShadows(true)
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
        visual.material = mMaterial->createInstance();
        // The local player is tinted differently so it's obvious which capsule
        // the camera is following.
        const float3 colour = isLocalPlayer ? float3{0.25f, 0.55f, 0.85f}
                                            : float3{0.85f, 0.75f, 0.30f};
        visual.material->setParameter("baseColor", RgbType::LINEAR, colour);
        visual.material->setParameter("roughness", 0.55f);
        visual.material->setParameter("metallic", 0.0f);

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

    mPlaneMaterial = nullptr;
    mCubeMaterial = nullptr;
    mMaterial = nullptr;
    mIndirectLight = nullptr;
    mLoaded = false;
}

}  // namespace game
