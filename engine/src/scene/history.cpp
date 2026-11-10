#include "ignis/scene/history.h"

#include "ignis/core/log.h"

namespace ignis {

void SceneHistory::begin(std::string name) {
    if (open_) {
        if (pending_name_ == name) return; // coalesce a continuous edit (e.g. one drag)
        commit(); // finish the previous transaction before starting a new one
    }
    pending_name_ = std::move(name);
    pending_before_ = scene_.serialize();
    open_ = true;
}

void SceneHistory::commit() {
    if (!open_) return;
    open_ = false;
    nlohmann::json after = scene_.serialize();
    if (after == pending_before_) return; // nothing changed: discard the entry
    undo_.push_back({std::move(pending_name_), std::move(pending_before_), std::move(after)});
    if (undo_.size() > capacity_)
        undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(undo_.size() - capacity_));
    redo_.clear();
}

void SceneHistory::abort() {
    open_ = false;
    pending_name_.clear();
    pending_before_ = nullptr;
}

bool SceneHistory::undo() {
    if (undo_.empty()) return false;
    if (open_) commit();
    const Command& cmd = undo_.back();
    nlohmann::json current = scene_.serialize();
    // load_json is atomic: on failure the scene is unchanged, so the stacks
    // are only updated after a successful restore.
    if (auto r = scene_.load_json(cmd.before); !r.ok()) {
        IGNIS_ERROR("undo '{}' failed: {}", cmd.name, r.error());
        return false;
    }
    redo_.push_back({cmd.name, std::move(current), cmd.after});
    undo_.pop_back();
    return true;
}

bool SceneHistory::redo() {
    if (redo_.empty()) return false;
    if (open_) commit();
    const Command& cmd = redo_.back();
    nlohmann::json current = scene_.serialize();
    if (auto r = scene_.load_json(cmd.after); !r.ok()) {
        IGNIS_ERROR("redo '{}' failed: {}", cmd.name, r.error());
        return false;
    }
    undo_.push_back({cmd.name, cmd.before, std::move(current)});
    redo_.pop_back();
    return true;
}

void SceneHistory::clear() {
    abort();
    undo_.clear();
    redo_.clear();
}

} // namespace ignis
