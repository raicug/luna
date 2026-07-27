#pragma once

struct lua_State;

namespace Luna::Detail {

[[nodiscard]] int NativeTrampoline(lua_State *State);

}
