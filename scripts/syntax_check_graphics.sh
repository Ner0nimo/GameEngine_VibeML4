#!/bin/sh
# Syntax-check (compile, no link) the graphics-only TUs with the same strict
# warning flags as the real build, using the FetchContent headers. Used on
# machines without X11/Vulkan dev packages where the full preset cannot build.
# Usage: scripts/syntax_check_graphics.sh [file.cpp ...]
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/build/headless/_deps"
GEN="$ROOT/build/headless/generated"

INCLUDES="-I$ROOT/window/include -I$ROOT/renderer/include -I$ROOT/editor/include \
 -I$ROOT/engine/include -I$ROOT/engine/src -I$ROOT/scripting/include \
 -I$ROOT/physics/include -I$ROOT/audio/include -I$GEN"
SYSINCLUDES="-isystem $DEPS/glfw-src/include -isystem $DEPS/nvrhi-src/include \
 -isystem $DEPS/vulkan_headers_ignis-src/include -isystem $DEPS/spirv_headers-src/include \
 -isystem $DEPS/imgui-src -isystem $DEPS/imgui-src/backends -isystem $DEPS/imguizmo-src/src \
 -isystem $DEPS/glm-src -isystem $DEPS/json-src/include -isystem $DEPS/cgltf-src \
 -isystem $DEPS/stb-src -isystem $DEPS/miniaudio-src -isystem $DEPS/lua-src \
 -isystem $DEPS/reactphysics3d-src/include \
 -isystem $DEPS/googletest-src/googletest/include \
 -isystem $DEPS/googletest-src/googlemock/include"

FLAGS="-std=c++20 -fPIC -fsyntax-only -DGLM_ENABLE_EXPERIMENTAL \
 -DIGNIS_WITH_GRAPHICS -DIGNIS_WINDOW_VULKAN -DIGNIS_RENDERER_VULKAN \
 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wnon-virtual-dtor \
 -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual -Wnull-dereference \
 -Wdouble-promotion -Wformat=2 -Werror"

if [ $# -gt 0 ]; then
    FILES="$*"
else
    FILES="$ROOT/window/src/window.cpp $ROOT/renderer/src/renderer.cpp \
$ROOT/renderer/src/vulkan_renderer.cpp $ROOT/renderer/src/vulkan_passes.cpp \
$ROOT/renderer/src/vulkan_dispatch.cpp $ROOT/renderer/src/ibl.cpp \
$ROOT/renderer/src/shader_embed.cpp $ROOT/editor/src/main.cpp $ROOT/editor/src/editor_app.cpp \
$ROOT/tests/src/test_render.cpp"
fi

for f in $FILES; do
    echo "== $f"
    EXTRA=""
    case "$f" in *editor*|*tests*) EXTRA="-DGLFW_INCLUDE_NONE -DIGNIS_TEST_DATA_DIR=\"\\\"$ROOT/tests/data\\\"\" -DIGNIS_SCENES_DIR=\"\\\"$ROOT/scenes\\\"\" -DIGNIS_SCRIPTS_DIR=\"\\\"$ROOT/scripts\\\"\"" ;; esac
    /usr/bin/c++ $FLAGS $EXTRA $INCLUDES $SYSINCLUDES -c "$f" -o /dev/null
done
echo "ALL OK"
