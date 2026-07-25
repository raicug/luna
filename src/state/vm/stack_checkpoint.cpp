// clang-format off
#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>
// clang-format on

namespace Luna::Detail {

StackCheckpoint::StackCheckpoint(lua_State *State) noexcept
    : State(State), Depth(State ? lua_gettop(State) : 0) {}

StackCheckpoint::~StackCheckpoint() {
  if (State)
    lua_settop(State, Depth);
}

} // namespace Luna::Detail
