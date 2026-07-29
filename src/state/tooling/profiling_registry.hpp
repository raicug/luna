#pragma once

// clang-format off
#include <luna/tooling/profiling_hook.hpp>

#include <mutex>
// clang-format on

struct lua_State;

namespace Luna::Detail {

// Owner-thread-only storage for one installed profiling or debug-UI hook.
// Reporting an event never changes invocation semantics: it runs strictly
// after Luna has already produced the outcome it reports, and a hook that
// throws is contained and then uninstalled rather than left to corrupt a
// later report or escape into Luau.
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

// Publishes the registry to the virtual machine as an opaque light userdata
// pointer, the same way Luna's suspended-call and delegate registries are
// reached from the trampoline without a public VM detail crossing the
// boundary.
[[nodiscard]] bool
PublishProfilingRegistry(lua_State *State,
                         ProfilingRegistry *Registry) noexcept;

[[nodiscard]] ProfilingRegistry *
ObserveProfilingRegistry(lua_State *State) noexcept;

} // namespace Luna::Detail
