#pragma once

struct lua_State;

namespace Luna::Detail {

[[nodiscard]] int NativeTrampoline(lua_State *State);

// Resumes one suspended native call after Luna's owner-thread pump settled
// the work it started.
[[nodiscard]] int NativeTrampolineContinuation(lua_State *State, int Status);

} // namespace Luna::Detail
