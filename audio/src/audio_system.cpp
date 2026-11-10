#include "ignis/audio/audio_system.h"

#include "ignis/core/log.h"
#include "ignis/ecs/world.h"
#include "ignis/scene/scene.h"

#include "lua_all.h"

namespace ignis {

AudioSystem* AudioSystem::active_ = nullptr;

AudioSystem::AudioSystem(AudioEngine& engine, Scene* scene) : engine_(engine), scene_(scene) {
    active_ = this;
}

AudioSystem::~AudioSystem() {
    if (active_ == this) active_ = nullptr;
}

void AudioSystem::register_components(World& world) {
    world.register_component_type<AudioSourceComponent>("AudioSource");
}

void AudioSystem::apply_params(u32 id, const AudioSourceComponent& comp, World& world, EntityId e) {
    engine_.set_sound_volume(id, comp.volume);
    engine_.set_sound_pitch(id, comp.pitch);
    if (comp.spatial) {
        engine_.set_sound_spatial(id, true, comp.min_distance, comp.max_distance);
        const math::Vec3 pos = glm::vec3(glm::vec4(0.f, 0.f, 0.f, 1.f) * world_matrix(world, e));
        engine_.set_sound_position(id, pos);
    } else {
        engine_.set_sound_spatial(id, false, comp.min_distance, comp.max_distance);
    }
}

u32 AudioSystem::start(EntityId e, World& world, const AudioSourceComponent& comp) {
    if (comp.path.empty()) return 0;
    auto r = engine_.play(comp.path, comp.volume, comp.loop);
    if (!r.ok()) {
        IGNIS_WARN("AudioSystem: cannot start source on entity {}: {}", e, r.error());
        return 0;
    }
    const u32 id = r.value();
    apply_params(id, comp, world, e);
    sources_[e] = id;
    started_.insert(e);
    return id;
}

u32 AudioSystem::play_source(EntityId e, World& world) {
    if (!world.alive(e)) return 0;
    const auto* comp = world.get<AudioSourceComponent>(e);
    if (!comp) return 0;
    auto it = sources_.find(e);
    if (it != sources_.end()) {
        engine_.stop(it->second);
        sources_.erase(it);
    }
    return start(e, world, *comp);
}

void AudioSystem::stop_source(EntityId e, World& world) {
    (void)world;
    auto it = sources_.find(e);
    if (it == sources_.end()) return;
    engine_.stop(it->second);
    sources_.erase(it);
}

bool AudioSystem::source_playing(EntityId e) const {
    auto it = sources_.find(e);
    return it != sources_.end() && engine_.is_playing(it->second);
}

void AudioSystem::set_listener(math::Vec3 pos, math::Vec3 forward) {
    engine_.set_listener(pos, forward);
}

void AudioSystem::stop_orphaned_sources(World& world) {
    for (auto it = sources_.begin(); it != sources_.end();) {
        const EntityId e = it->first;
        if (!world.alive(e) || !world.get<AudioSourceComponent>(e)) {
            engine_.stop(it->second);
            it = sources_.erase(it);
            started_.erase(e);
        } else {
            ++it;
        }
    }
}

void AudioSystem::update_source(EntityId e, World& world) {
    const auto* comp = world.get<AudioSourceComponent>(e);
    if (!comp) return;
    auto it = sources_.find(e);
    if (it != sources_.end()) {
        // Keep live params (volume/pitch/spatial position) in sync.
        apply_params(it->second, *comp, world, e);
        // A finished one-shot is released; play_on_start must not retrigger.
        if (!comp->loop && !engine_.is_playing(it->second)) {
            engine_.stop(it->second);
            sources_.erase(it);
        }
    } else if (comp->play_on_start && !started_.contains(e)) {
        start(e, world, *comp);
    }
}

void AudioSystem::update(World& world, f32 dt) {
    (void)dt;
    stop_orphaned_sources(world);
    for (EntityId e : world.view<AudioSourceComponent>()) update_source(e, world);
}

// --- Lua bindings --------------------------------------------------------------

namespace {

AudioSystem* sys_from_lua(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "ignis.audio_system");
    AudioSystem* s = static_cast<AudioSystem*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return s ? s : AudioSystem::active();
}

int l_audio_play_source(lua_State* L) {
    const EntityId e = static_cast<EntityId>(luaL_checkinteger(L, 1));
    AudioSystem* s = sys_from_lua(L);
    if (!s || !s->scene()) {
        lua_pushinteger(L, 0);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(s->play_source(e, s->scene()->world())));
    return 1;
}

int l_audio_stop_source(lua_State* L) {
    const EntityId e = static_cast<EntityId>(luaL_checkinteger(L, 1));
    if (AudioSystem* s = sys_from_lua(L)) {
        if (s->scene()) s->stop_source(e, s->scene()->world());
    }
    return 0;
}

int l_audio_source_playing(lua_State* L) {
    const EntityId e = static_cast<EntityId>(luaL_checkinteger(L, 1));
    AudioSystem* s = sys_from_lua(L);
    lua_pushboolean(L, (s && s->source_playing(e)) ? 1 : 0);
    return 1;
}

int l_audio_set_listener(lua_State* L) {
    const math::Vec3 pos{static_cast<f32>(luaL_checknumber(L, 1)),
                         static_cast<f32>(luaL_checknumber(L, 2)),
                         static_cast<f32>(luaL_checknumber(L, 3))};
    const math::Vec3 fwd{static_cast<f32>(luaL_checknumber(L, 4)),
                         static_cast<f32>(luaL_checknumber(L, 5)),
                         static_cast<f32>(luaL_checknumber(L, 6))};
    if (AudioSystem* s = sys_from_lua(L)) s->set_listener(pos, fwd);
    return 0;
}

} // namespace

void AudioSystem::register_lua_bindings(lua_State* L) {
    lua_getglobal(L, "ignis");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "ignis");
    }
    // ignis.audio must exist (AudioEngine::register_lua_bindings creates it);
    // create it if the engine bindings were not registered first.
    lua_getfield(L, -1, "audio");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, "audio");
    }
    lua_pushcfunction(L, l_audio_play_source); lua_setfield(L, -2, "play_source");
    lua_pushcfunction(L, l_audio_stop_source); lua_setfield(L, -2, "stop_source");
    lua_pushcfunction(L, l_audio_source_playing); lua_setfield(L, -2, "source_playing");
    lua_pushcfunction(L, l_audio_set_listener); lua_setfield(L, -2, "set_listener");
    lua_pop(L, 2);

    lua_pushlightuserdata(L, active_);
    lua_setfield(L, LUA_REGISTRYINDEX, "ignis.audio_system");
}

} // namespace ignis
