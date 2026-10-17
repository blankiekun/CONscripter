// lua.hpp
// Lua header files for C++
// <<extern "C">> not supplied automatically because Lua also compiles as C++

// <climits>: newlib's <limits.h> has no LLONG_MAX in C++ (devkitPro), Lua needs it
#include <climits>

extern "C" {
#include "lua/lua.h"
#include "lua/lualib.h"
#include "lua/lauxlib.h"
}
