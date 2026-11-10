#include "ignis/agent/commands.h"

#include "ignis/assets/asset_manager.h"
#include "ignis/core/log.h"
#include "ignis/ecs/component_registry.h"
#include "ignis/ecs/components.h"
#include "ignis/scene/scene.h"

#include <fstream>
#include <sstream>

namespace ignis {

namespace {

CommandResult ok(nlohmann::json data = nlohmann::json::object(), std::string msg = "") {
    return CommandResult{true, std::move(msg), std::move(data)};
}

CommandResult fail(std::string msg) {
    IGNIS_WARN("agent command failed: {}", msg);
    return CommandResult{false, std::move(msg), nlohmann::json::object()};
}

} // namespace

CommandResult CommandProcessor::execute(const nlohmann::json& cmd) {
    if (!cmd.is_object() || !cmd.contains("cmd")) return fail("missing 'cmd'");
    const std::string c = cmd["cmd"].get<std::string>();
    World& world = scene_.world();

    if (c == "create_entity") {
        EntityId e = world.create_entity(cmd.value("name", ""));
        return ok({{"entity", e}}, "created");
    }
    if (c == "destroy_entity") {
        EntityId e = cmd.value("entity", 0u);
        if (!world.alive(e)) return fail("entity not alive");
        world.destroy_entity(e);
        return ok({}, "destroyed");
    }
    if (c == "set_name") {
        EntityId e = cmd.value("entity", 0u);
        if (!world.alive(e)) return fail("entity not alive");
        world.set_name(e, cmd.value("name", ""));
        return ok({}, "renamed");
    }
    if (c == "set_parent") {
        EntityId child = cmd.value("entity", 0u);
        EntityId parent = cmd.value("parent", 0u);
        if (!world.alive(child)) return fail("entity not alive");
        if (!world.set_parent(child, parent))
            return fail("invalid parent (dead or would create a cycle)");
        return ok({}, "reparented");
    }
    if (c == "add_component" || c == "set_component") {
        EntityId e = cmd.value("entity", 0u);
        const std::string comp = cmd.value("component", "");
        if (!world.alive(e)) return fail("entity not alive");
        const ComponentTypeInfo* info = world.registry().find_by_name(comp);
        if (!info) return fail("unknown component '" + comp + "'");
        nlohmann::json props = cmd.value("props", nlohmann::json::object());
        if (!info->deserialize(world, e, props)) return fail("bad props for '" + comp + "'");
        return ok({}, "component set");
    }
    if (c == "remove_component") {
        EntityId e = cmd.value("entity", 0u);
        const std::string comp = cmd.value("component", "");
        const ComponentTypeInfo* info = world.registry().find_by_name(comp);
        if (!info) return fail("unknown component '" + comp + "'");
        if (!world.alive(e)) return fail("entity not alive");
        info->remove(world, e);
        return ok({}, "removed");
    }
    if (c == "get_entity") {
        EntityId e = cmd.value("entity", 0u);
        if (!world.alive(e)) return fail("entity not alive");
        nlohmann::json data;
        data["entity"] = e;
        data["name"] = world.name_of(e);
        data["parent"] = world.parent_of(e);
        data["components"] = nlohmann::json::array();
        for (const auto& [type, storage] : world.storages()) {
            if (!storage->has(e)) continue;
            const ComponentTypeInfo* info = world.registry().find_by_type(type);
            if (info) data["components"].push_back({{"type", info->name},
                                                    {"data", info->serialize(world, e)}});
        }
        return ok(std::move(data), "entity");
    }
    if (c == "list_entities") {
        nlohmann::json arr = nlohmann::json::array();
        for (EntityId e : world.all_entities()) {
            arr.push_back({{"entity", e}, {"name", world.name_of(e)}});
        }
        return ok({{"entities", std::move(arr)}}, "list");
    }
    if (c == "spawn_prefab") {
        const std::string name = cmd.value("prefab", "");
        EntityId e = scene_.spawn_prefab(name);
        if (e == NULL_ENTITY) return fail("unknown prefab '" + name + "'");
        return ok({{"entity", e}}, "spawned");
    }
    if (c == "add_prefab") {
        scene_.add_prefab(cmd.value("prefab", ""), cmd.value("template", nlohmann::json::object()));
        return ok({}, "prefab added");
    }
    if (c == "list_prefabs") {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& n : scene_.prefab_names()) arr.push_back(n);
        return ok({{"prefabs", std::move(arr)}}, "prefabs");
    }
    if (c == "save_scene") {
        const std::string path = cmd.value("path", "scene.json");
        auto r = scene_.save_to_file(path);
        if (!r.ok()) return fail(r.error());
        return ok({{"path", path}}, "saved");
    }
    if (c == "load_model") {
        const std::string path = cmd.value("path", "");
        auto model = assets_.load_model(path);
        if (!model) return fail("cannot load model '" + path + "'");
        return ok({{"primitives", model->mesh->primitives.size()},
                   {"materials", model->materials.size()},
                   {"textures", model->textures.size()}}, "model ok");
    }
    if (c == "load_texture") {
        const std::string path = cmd.value("path", "");
        auto tex = assets_.load_texture(path);
        if (!tex) return fail("cannot load texture '" + path + "'");
        return ok({{"width", tex->width}, {"height", tex->height}, {"hdr", tex->hdr}}, "texture ok");
    }
    if (c == "run_commands") {
        auto results = execute_file(cmd.value("file", ""));
        u32 succeeded = 0;
        for (const auto& r : results) if (r.ok) ++succeeded;
        return ok({{"total", results.size()}, {"succeeded", succeeded}}, "batch done");
    }
    return fail("unknown cmd '" + c + "'");
}

std::vector<CommandResult> CommandProcessor::execute_file(const std::string& jsonl_path) {
    std::vector<CommandResult> results;
    std::ifstream f(jsonl_path);
    if (!f) {
        results.push_back(fail("cannot open '" + jsonl_path + "'"));
        return results;
    }
    std::string line;
    u32 line_no = 0;
    while (std::getline(f, line)) {
        ++line_no;
        if (line.empty()) continue;
        try {
            results.push_back(execute(nlohmann::json::parse(line)));
        } catch (const std::exception& e) {
            results.push_back(fail(std::format("line {}: {}", line_no, e.what())));
        }
    }
    return results;
}

} // namespace ignis
