#pragma once

// The lua/lua repo ships no lua.hpp; wrap the C headers ourselves.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
