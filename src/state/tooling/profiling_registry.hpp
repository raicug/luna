#pragma once

// clang-format off
#include <luna/tooling/profiling_hook.hpp>

#include <mutex>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class ProfilingRegistry final {
public:
  void Install(ProfilingHook Hook) {
    const std::lock_guard<std::mutex> Guard(Barrier);
    HookValue = std::move(Hook);
  }

  void Clear() noexcept {
    const std::lock_guard<std::mutex> Guard(Barrier);
    HookValue = ProfilingHook();
  }

  [[nodiscard]] bool IsInstalled() const noexcept {
    const std::lock_guard<std::mutex> Guard(Barrier);
    return static_cast<bool>(HookValue);
  }

  void Report(const ProfilingEvent &Event) noexcept {
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
    }
  }

private:
  mutable std::mutex Barrier;
  ProfilingHook HookValue;
};

[[nodiscard]] bool
PublishProfilingRegistry(lua_State *State,
                         ProfilingRegistry *Registry) noexcept;

[[nodiscard]] ProfilingRegistry *
ObserveProfilingRegistry(lua_State *State) noexcept;

} // namespace Luna::Detail
