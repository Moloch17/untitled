#pragma once

#include <filament/Engine.h>
#include <filament/IndirectLight.h>
#include <filament/Material.h>
#include <filament/Scene.h>
#include <utils/Entity.h>

#include <map>
#include <set>

#include <net/snapshot.h>

#include "mesh.h"

namespace game {

// The demo scene: a ground plane, a cube resting on it, and two directional
// lights. Owns everything it creates.
class DemoScene {
public:
    void build(filament::Engine* engine, filament::Scene* scene);
    void destroy(filament::Engine* engine, filament::Scene* scene);
    bool loaded() const { return mLoaded; }

    // Applies a transform received from the server. The cube's motion is
    // simulated on the server only; the client never computes it.
    void setEntityTransform(filament::Engine* engine, uint32_t entityId,
            const net::Vec3& position, const net::Quat& rotation);

    // Players come and go as they join and leave. A renderable is created the
    // first time an id is seen and destroyed when it stops being reported.
    void setPlayerTransform(filament::Engine* engine, filament::Scene* scene, uint32_t entityId,
            bool isLocalPlayer, const net::Vec3& position, const net::Quat& rotation);
    void removeAbsentPlayers(filament::Engine* engine, filament::Scene* scene,
            const std::set<uint32_t>& present);

private:
    filament::Material* mMaterial = nullptr;
    filament::MaterialInstance* mPlaneMaterial = nullptr;
    filament::MaterialInstance* mCubeMaterial = nullptr;

    Mesh mPlaneMesh;
    Mesh mCubeMesh;

    utils::Entity mPlane;
    utils::Entity mCube;
    utils::Entity mSun;
    utils::Entity mPointLight;

    // One shared capsule mesh; each player gets its own renderable and material
    // instance so they can be coloured individually.
    Mesh mCapsuleMesh;
    struct PlayerVisual {
        utils::Entity entity;
        filament::MaterialInstance* material = nullptr;
    };
    std::map<uint32_t, PlayerVisual> mPlayers;

    bool mLoaded = false;
    filament::IndirectLight* mIndirectLight = nullptr;
};

}  // namespace game
