// Editor application - full implementation lands with the graphics build.
// This TU compiles everywhere so the target graph stays valid; run() only
// exists in graphics builds.
#include "ignis/editor/editor_app.h"

#include "ignis/core/log.h"

namespace ignis {

EditorApp::EditorApp() = default;
EditorApp::~EditorApp() = default;

int EditorApp::run(const std::string& scene_path, const std::string& commands_path) {
#ifndef IGNIS_WITH_GRAPHICS
    (void)scene_path;
    (void)commands_path;
    IGNIS_ERROR("editor requires a graphics build (IGNIS_WITH_GRAPHICS=ON)");
    return 1;
#else
    // TODO(graphics): window + renderer + ImGui panels + gizmo + command file watcher.
    (void)scene_path;
    (void)commands_path;
    IGNIS_ERROR("editor not yet implemented in this build");
    return 1;
#endif
}

} // namespace ignis
