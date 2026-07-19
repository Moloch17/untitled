#include "scene.h"

#include <filament/LightManager.h>
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

    // A warm point light off to one side, to show local falloff alongside the
    // directional sun. Point lights aren't subject to the single-light limit.
    mPointLight = entityManager.create();
    LightManager::Builder(LightManager::Type::POINT)
            .color(Color::toLinear<ACCURATE>(sRGBColor{1.0f, 0.55f, 0.25f}))
            .intensity(120000.0f)
            .falloff(8.0f)
            .position({-2.5f, 1.5f, 1.5f})
            .build(*engine, mPointLight);
    scene->addEntity(mPointLight);
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

    for (utils::Entity entity : {mPlane, mCube, mSun, mPointLight}) {
        scene->remove(entity);
        engine->destroy(entity);
        entityManager.destroy(entity);
    }

    scene->setIndirectLight(nullptr);
    engine->destroy(mIndirectLight);

    removeAbsentPlayers(engine, scene, {});

    mPlaneMesh.destroy(engine);
    mCubeMesh.destroy(engine);
    mCapsuleMesh.destroy(engine);

    engine->destroy(mPlaneMaterial);
    engine->destroy(mCubeMaterial);
    engine->destroy(mMaterial);

    mPlaneMaterial = nullptr;
    mCubeMaterial = nullptr;
    mMaterial = nullptr;
    mIndirectLight = nullptr;
    mLoaded = false;
}

}  // namespace game
