#include "ignis/editor/editor_app.h"

#include <cstring>
#include <string>

int main(int argc, char** argv) {
    std::string scene_path = "scenes/test_scene.scene.json";
    std::string commands_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--commands" && i + 1 < argc) commands_path = argv[++i];
        else if (!a.empty() && a[0] != '-') scene_path = a;
    }
    ignis::EditorApp app;
    return app.run(scene_path, commands_path);
}
