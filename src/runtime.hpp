#pragma once

// luna : private runtime state.

#include "model.hpp"

#include <luna/state.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace luna {
namespace detail {

// One runnable chunk. Execute() owns exactly one; Spawn() adds more.
struct Task {
  lua_State *Co = nullptr;
  int ThreadRef = LUA_NOREF;
  std::uint64_t Id = 0;
  double Due = 0.0;
  bool Dead = false;
};

struct Runtime {
  State *Owner = nullptr;
  lua_State *L = nullptr;
  Model World;

  std::atomic<bool> Stop{false};
  std::string StopReason;

  // Virtual clock. A suspension records a due time against it, so nothing on the
  // call path ever sleeps.
  double Clock = 0.0;
  std::vector<Task> Tasks;
  std::uint64_t NextId = 1;

  // Index into Tasks of the coroutine being resumed, if any. An index rather
  // than a pointer because Tasks reallocates when a suspension spawns work.
  bool HasRunning = false;
  std::size_t RunningIndex = 0;

  explicit Runtime(State *InOwner) : Owner(InOwner), World(nullptr) {}
};

inline Runtime *RuntimeFromLua(lua_State *L) noexcept {
  lua_Callbacks *Callbacks = lua_callbacks(L);
  return Callbacks ? static_cast<Runtime *>(Callbacks->userdata) : nullptr;
}

inline Model *ModelFromLua(lua_State *L) noexcept {
  Runtime *R = RuntimeFromLua(L);
  return R ? &R->World : nullptr;
}

// Metamethods installed on every class metatable. Defined in userdata.cpp.
int ClassIndex(lua_State *L);
int ClassNewIndex(lua_State *L);
int ClassIter(lua_State *L);
int ClassIterStep(lua_State *L);

} // namespace detail
} // namespace luna
