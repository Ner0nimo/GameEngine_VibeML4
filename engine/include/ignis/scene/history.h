#pragma once

#include "ignis/core/types.h"
#include "ignis/scene/scene.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace ignis {

// Undo/redo history for a scene, built on Scene::serialize()/load_json().
// A transaction captures the scene before and after one logical mutation:
//   history.begin("delete entity");
//   ...mutate the scene...
//   history.commit();   // pushes the entry; no-op changes are discarded
// Continuous edits (gizmo drags, inspector widgets) coalesce: calling begin()
// again with the same name while a transaction is open keeps the original
// "before" snapshot, so a whole drag becomes a single undo entry. Because
// load_json preserves entity ids, selections stay valid across undo/redo.
class SceneHistory {
public:
    struct Command {
        std::string name;
        nlohmann::json before;
        nlohmann::json after;
    };

    explicit SceneHistory(Scene& scene, u32 capacity = 50) : scene_(scene), capacity_(capacity) {}

    // Opens a transaction and captures the current state as "before". If a
    // transaction with the same name is already open it is coalesced (the
    // original "before" is kept); a different open transaction is committed
    // first.
    void begin(std::string name);
    // Closes the open transaction and captures "after". Entries where
    // before == after are discarded. Clears the redo stack.
    void commit();
    // Discards the open transaction without recording it (the scene itself
    // is not reverted).
    void abort();

    bool can_undo() const noexcept { return !undo_.empty(); }
    bool can_redo() const noexcept { return !redo_.empty(); }
    u32 undo_depth() const noexcept { return static_cast<u32>(undo_.size()); }
    u32 redo_depth() const noexcept { return static_cast<u32>(redo_.size()); }
    const std::string& undo_label() const noexcept {
        static const std::string empty;
        return undo_.empty() ? empty : undo_.back().name;
    }
    const std::string& redo_label() const noexcept {
        static const std::string empty;
        return redo_.empty() ? empty : redo_.back().name;
    }

    // Restores the state before the newest undo entry (or the state after it
    // for redo). Returns false when there is nothing to undo/redo or the
    // restore failed (reported via the log; the scene is left unchanged).
    bool undo();
    bool redo();

    void clear();

private:
    Scene& scene_;
    u32 capacity_;
    std::vector<Command> undo_;
    std::vector<Command> redo_;
    std::string pending_name_;
    nlohmann::json pending_before_;
    bool open_ = false;
};

} // namespace ignis
