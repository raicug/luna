#pragma once

struct lua_State;

namespace Luna::Detail {

[[nodiscard]] int NativeTrampoline(lua_State *State);

[[nodiscard]] int NativeTrampolineContinuation(lua_State *State, int Status);

} // namespace Luna::Detail
