// clang-format off
#include "state/tooling/profiling_registry.hpp"

#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr const char *ProfilingRegistrySlot = "Luna.Profiling";

} // namespace

bool PublishProfilingRegistry(lua_State *State,
                              ProfilingRegistry *Registry) noexcept {
  if (!State || !Registry)
    return false;
  if (!lua_checkstack(State, 4))
    return false;

  StackCheckpoint Checkpoint(State);
  lua_pushlightuserdata(State, Registry);
  lua_rawsetfield(State, LUA_REGISTRYINDEX, ProfilingRegistrySlot);
  return true;
}

ProfilingRegistry *ObserveProfilingRegistry(lua_State *State) noexcept {
  if (!State || !lua_checkstack(State, 2))
    return nullptr;

  StackCheckpoint Checkpoint(State);
  lua_rawgetfield(State, LUA_REGISTRYINDEX, ProfilingRegistrySlot);
  return static_cast<ProfilingRegistry *>(lua_tolightuserdata(State, -1));
}

} // namespace Luna::Detail
