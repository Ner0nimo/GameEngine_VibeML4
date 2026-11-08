// Editor application - full implementation lands with the graphics build.
// This TU compiles everywhere so the target graph stays valid; run() only
// exists in graphics builds.
#include "ignis/editor/editor_app.h"

#include "ignis/core/log.h"

#ifdef IGNIS_WITH_GRAPHICS
#include <sys/wait.h>
#include <unistd.h>
#endif

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
    // Probe the Vulkan stack in a forked child first (some driver stacks crash
    // inside NVRHI dispatcher calls - fail with a message, not a segfault).
    {
        auto probe = []() -> bool {
            const pid_t pid = fork();
            if (pid == 0) {
                VulkanRenderer probeRenderer;
                std::string probeErr;
                if (!probeRenderer.init_offscreen(64, 64, probeErr)) _exit(2);
                probeRenderer.shutdown();
                _exit(0);
            }
            int status = 0;
            waitpid(pid, &status, 0);
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        };
        if (!probe()) {
            IGNIS_ERROR("Vulkan/NVRHI device not usable on this system (driver crash or no GPU).");
            return 1;
        }
    }
    // TODO(graphics): window + renderer + ImGui panels + gizmo + command file watcher.
    (void)scene_path;
    (void)commands_path;
    IGNIS_ERROR("editor UI not yet implemented in this build");
    return 1;
#endif
}

} // namespace ignis
