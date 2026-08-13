#pragma once

// clang-format off
#include <luna/tooling/profiling_hook.hpp>

#include <atomic>
#include <mutex>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class ProfilingRegistry final {
public:
  void Install(ProfilingHook Hook) {
    const std::lock_guard<std::mutex> Guard(Barrier);
    HookValue = std::move(Hook);
    Installed.store(static_cast<bool>(HookValue), std::memory_order_release);
  }

  void Clear() noexcept {
    const std::lock_guard<std::mutex> Guard(Barrier);
    HookValue = ProfilingHook();
    Installed.store(false, std::memory_order_release);
  }

  [[nodiscard]] bool IsInstalled() const noexcept {
    if (!Installed.load(std::memory_order_acquire))
      return false;
    const std::lock_guard<std::mutex> Guard(Barrier);
    return static_cast<bool>(HookValue);
  }

  void Report(const ProfilingEvent &Event) noexcept {
    if (!Installed.load(std::memory_order_acquire))
      return;
    ProfilingHook Called;
    {
      const std::lock_guard<std::mutex> Guard(Barrier);
      if (!HookValue)
        return;
      Called = HookValue;
    }
    try {
      Called(Event);
    } catch (...) {
      const std::lock_guard<std::mutex> Guard(Barrier);
      HookValue = ProfilingHook();
      Installed.store(false, std::memory_order_release);
    }
  }

private:
  mutable std::mutex Barrier;
  std::atomic<bool> Installed{false};
  ProfilingHook HookValue;
};

[[nodiscard]] bool
PublishProfilingRegistry(lua_State *State,
                         ProfilingRegistry *Registry) noexcept;

[[nodiscard]] ProfilingRegistry *
ObserveProfilingRegistry(lua_State *State) noexcept;

} // namespace Luna::Detail
