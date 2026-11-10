-- Test script for the integration test scene.
-- Exercises: component get/set, entity create/destroy, prefab spawn,
-- physics raycast and audio playback from Lua.

function on_create(self)
    ignis.log(2, "mover on_create, entity=" .. self.entity)
end

function on_update(self, dt)
    -- Move along +X.
    local t = ignis.entity.get_component(self.entity, "Transform")
    if t then
        t.position = { t.position[1] + 2.0 * dt, t.position[2], t.position[3] }
        ignis.entity.add_component(self.entity, "Transform", t)
    end

    local f = ignis.frame()

    if f == 30 then
        ignis.scene.spawn_prefab("SpawnedPrefab")
    end

    if f == 60 then
        -- Destroy the "Target" entity (id 4 in the test scene).
        if ignis.entity.exists(4) then
            ignis.entity.destroy(4)
        end
    end

    if f == 90 then
        local hit = ignis.physics.raycast({ 0, 10, 0 }, { 0, -1, 0 }, 100)
        if hit then
            ignis.log(2, "raycast hit entity=" .. hit.entity .. " dist=" .. hit.distance)
        end
    end

    if f == 100 then
        local id = ignis.audio.play("assets/test.wav", 0.5, false)
        ignis.log(2, "audio play id=" .. id)
    end

    -- AudioSource component control: SoundEmitter is entity 7 with
    -- play_on_start=true, so it is already playing at frame 120.
    if f == 120 then
        mover_src_playing = ignis.audio.source_playing(7)
        ignis.audio.stop_source(7)
        mover_src_stopped = not ignis.audio.source_playing(7)
    end

    if f == 150 then
        ignis.audio.play_source(7)
        mover_src_restarted = ignis.audio.source_playing(7)
    end
end

function on_destroy(self)
    ignis.log(2, "mover on_destroy, entity=" .. self.entity)
end
