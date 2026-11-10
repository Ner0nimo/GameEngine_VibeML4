#pragma once

#include "ignis/audio/audio_engine.h"
#include "ignis/core/types.h"
#include "ignis/ecs/components.h"
#include "ignis/ecs/entity.h"
#include "ignis/math/math.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

struct lua_State;

namespace ignis {

class Scene;
class World;

// --- Component (registered as "AudioSource") ---------------------------------
// Scene-authored audio: plays a sound file (disk or pak) either on start or
// on demand via ignis.audio.play_source(entity). Runtime playback state lives
// in AudioSystem, not in the component, so serialization stays pure data.

struct AudioSourceComponent {
    std::string path;                 // audio file, relative to CWD or pak root
    f32 volume = 1.f;
    f32 pitch = 1.f;
    bool loop = false;
    bool play_on_start = false;       // start playing on the first update
    bool spatial = false;             // 3D positional audio from the Transform
    f32 min_distance = 1.f;           // spatial: full volume within this radius
    f32 max_distance = 50.f;          // spatial: silent beyond this radius
};

template <>
struct ComponentIO<AudioSourceComponent> {
    static nlohmann::json to_json(const AudioSourceComponent& c) {
        return {{"path", c.path},
                {"volume", c.volume},
                {"pitch", c.pitch},
                {"loop", c.loop},
                {"play_on_start", c.play_on_start},
                {"spatial", c.spatial},
                {"min_distance", c.min_distance},
                {"max_distance", c.max_distance}};
    }
    static AudioSourceComponent from_json(const nlohmann::json& j) {
        AudioSourceComponent c;
        if (j.contains("path")) c.path = j["path"].get<std::string>();
        if (j.contains("volume")) c.volume = j["volume"].get<f32>();
        if (j.contains("pitch")) c.pitch = j["pitch"].get<f32>();
        if (j.contains("loop")) c.loop = j["loop"].get<bool>();
        if (j.contains("play_on_start")) c.play_on_start = j["play_on_start"].get<bool>();
        if (j.contains("spatial")) c.spatial = j["spatial"].get<bool>();
        if (j.contains("min_distance")) c.min_distance = j["min_distance"].get<f32>();
        if (j.contains("max_distance")) c.max_distance = j["max_distance"].get<f32>();
        return c;
    }
};

// Drives AudioSourceComponent playback. Add as a scene system:
//   AudioSystem audio_sys(audio_engine, &scene);
//   scene.add_system([&](World& w, f32 dt){ audio_sys.update(w, dt); });
class AudioSystem {
public:
    explicit AudioSystem(AudioEngine& engine, Scene* scene = nullptr);
    ~AudioSystem();

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    void update(World& world, f32 dt);

    // Start (or restart) the source on an entity; returns the sound id or 0.
    u32 play_source(EntityId e, World& world);
    void stop_source(EntityId e, World& world);
    bool source_playing(EntityId e) const;

    void set_listener(math::Vec3 pos, math::Vec3 forward);

    Scene* scene() const { return scene_; }

    // Registers AudioSourceComponent on the (global) component registry.
    static void register_components(World& world);

    // Lua bindings: ignis.audio.play_source(entity) -> id,
    //               ignis.audio.stop_source(entity),
    //               ignis.audio.source_playing(entity) -> bool,
    //               ignis.audio.set_listener(px,py,pz, fx,fy,fz)
    static void register_lua_bindings(lua_State* L);
    static AudioSystem* active() { return active_; }

private:
    u32 start(EntityId e, World& world, const AudioSourceComponent& comp);
    void apply_params(u32 id, const AudioSourceComponent& comp, World& world, EntityId e);
    void stop_orphaned_sources(World& world); // dead entity or component removed
    void update_source(EntityId e, World& world); // one entity, one frame

    AudioEngine& engine_;
    Scene* scene_ = nullptr;
    std::unordered_map<EntityId, u32> sources_; // entity -> sound id
    std::unordered_set<EntityId> started_;      // play_on_start already fired
    static AudioSystem* active_;
};

} // namespace ignis
