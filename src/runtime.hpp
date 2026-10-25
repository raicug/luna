#pragma once

// Per-state runtime: the VM, the allocator accounting and the task list.

#include "model.hpp"

#include <luna/state.hpp>

#include <cstdlib>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace luna {
namespace detail {
struct Task {
  lua_State *Co = nullptr;
  int ThreadRef = LUA_NOREF;
  std::uint64_t Id = 0;
  double Due = 0.0;
  bool Dead = false;

  // True while the coroutine is inside lua_resume, so a nested pump skips it.
  bool Running = false;

  // Non-null while parked on an external event; only Wake clears it.
  const void *WaitKey = nullptr;

  // Pushes the values a Wake wants the task to resume with.
  std::function<int(void *)> Resume;

  // Arguments already on the coroutine's stack for its first resume.
  int StartArgs = 0;

  // A coroutine a script made that began waiting, so the scheduler took it
  // over.
  bool Adopted = false;

  // The call that parked the task returns the seconds that passed.
  bool Elapsed = false;
  double Since = 0.0;
};

struct MemoryArena {
  std::size_t Used = 0;
  std::size_t Limit = 0;
};

inline void *LunaAllocate(void *Ud, void *Pointer, std::size_t OldSize,
                          std::size_t NewSize) {
  auto *Arena = static_cast<MemoryArena *>(Ud);
  if (NewSize == 0) {
    std::free(Pointer);
    Arena->Used -= OldSize;
    return nullptr;
  }

  const std::size_t Base = Arena->Used - OldSize;
  if (Arena->Limit != 0 &&
      (NewSize > Arena->Limit || Base > Arena->Limit - NewSize))
    return nullptr;

  void *Fresh = std::realloc(Pointer, NewSize);
  if (!Fresh)
    return nullptr;

  Arena->Used = Base + NewSize;
  return Fresh;
}

struct Runtime {
  State *Owner = nullptr;
  StateOptions Settings;
  lua_State *L = nullptr;
  Model World;
  MemoryArena Memory;
  // Shared with delegates and chunks, which may outlive this object.
  std::shared_ptr<Liveness> Token = std::make_shared<Liveness>();
  std::atomic<bool> Stop{false};
  // Written by whichever thread asks for the stop, read by the VM thread.
  std::mutex StopLock;
  std::string StopReason;
  std::uint64_t Steps = 0;
  bool BudgetTripped = false;
  bool LastInterrupted = false;
  double Clock = 0.0;
  // A closure made at start-up that runs a job under lua_pcall, so setting a
  // guarded call up allocates nothing and cannot itself run out of memory.
  int GuardRef = LUA_NOREF;
  std::vector<Task> Tasks;
  // Wakes posted by other threads, applied on the state's own thread.
  std::mutex InboxLock;
  std::vector<std::pair<const void *, std::function<int(void *)>>> Inbox;
  std::atomic<bool> InboxFull{false};
  // Finished entries not yet reclaimed, and how many scheduler loops are
  // active.
  std::size_t DeadTasks = 0;
  int PumpDepth = 0;
  std::uint64_t NextId = 1;
  bool HasRunning = false;
  std::size_t RunningIndex = 0;
  explicit Runtime(State *InOwner) : Owner(InOwner), World(nullptr) {}

  ~Runtime() {
    Token->Alive.store(false, std::memory_order_release);
    World.Release();
    if (L)
      lua_close(L);
    L = nullptr;
  }

  void RequestStop(std::string Reason) {
    {
      std::lock_guard<std::mutex> Guard(StopLock);
      StopReason = std::move(Reason);
    }
    Stop.store(true, std::memory_order_release);
  }

  void ClearStop() {
    Stop.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> Guard(StopLock);
    StopReason.clear();
  }

  std::string StopText() {
    std::lock_guard<std::mutex> Guard(StopLock);
    return StopReason;
  }

  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
};

// Runs Work under lua_pcall on the main thread. A Lua error (out of memory, a
// read-only table) or a marshalling error comes back as text in Failure instead
// of unwinding into the host.
bool RunGuarded(Runtime &R, const std::function<void()> &Work,
                std::string &Failure);

inline Runtime *RuntimeFromLua(lua_State *L) noexcept {
  lua_Callbacks *Callbacks = lua_callbacks(L);
  return Callbacks ? static_cast<Runtime *>(Callbacks->userdata) : nullptr;
}

inline Model *ModelFromLua(lua_State *L) noexcept {
  Runtime *R = RuntimeFromLua(L);
  return R ? &R->World : nullptr;
}

int ClassIndex(lua_State *L);
int ClassNewIndex(lua_State *L);
int ClassIsA(lua_State *L);
int ClassIter(lua_State *L);
int ClassIterStep(lua_State *L);
} // namespace detail
} // namespace luna
