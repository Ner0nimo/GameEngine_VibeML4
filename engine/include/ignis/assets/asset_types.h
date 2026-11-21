#pragma once

#include <string>
#include <vector>

namespace ignis {

// Central registry of asset types: extension -> icon/label/target component/
// spawn behavior, so the editor doesn't scatter extension checks around.
// `component` is the registered component type name a file assigns to
// ("" = not assignable to an entity, e.g. scene files). `can_spawn` means a
// viewport drop creates a new entity carrying it. `is_scene` means dropping
// offers to load the scene instead of spawning.
struct AssetTypeInfo {
    std::string extension; // ".gltf"
    const char* icon;      // short glyph shown in lists/grids
    const char* label;     // human-readable type name
    const char* component; // registered component name to assign, or ""
    bool can_spawn = false;
    bool is_scene = false;
};

// All registered asset types (built-in set: model, audio, script, scene, texture).
const std::vector<AssetTypeInfo>& asset_types();

// Lookup by plain extension (".gltf"). Returns nullptr for unknown types.
const AssetTypeInfo* find_asset_type(const std::string& extension);

// Lookup for a file name: recognizes the ".scene.json" suffix first, then the
// plain extension. Returns nullptr for unknown types.
const AssetTypeInfo* find_asset_type_for_file(const std::string& filename);

// Scan *.json files under dir (recursively) for textual references to
// asset_rel_path; returns the referencing files. Used to warn before
// deleting/renaming an asset that scenes still reference.
std::vector<std::string> find_asset_references(const std::string& dir, const std::string& asset_rel_path);

} // namespace ignis
