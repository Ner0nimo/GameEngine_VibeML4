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
end

function on_destroy(self)
    ignis.log(2, "mover on_destroy, entity=" .. self.entity)
end
