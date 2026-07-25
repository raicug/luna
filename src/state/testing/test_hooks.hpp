#pragma once

// clang-format off
#include <luna/binding/value.hpp>

#include "state/testing/fault_injector.hpp"
#include "state/testing/fault_point.hpp"
#include "state/testing/test_control.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna {
class State;
}

namespace Luna::Detail {

struct NativeInvocationObservation final {
  bool Succeeded = false;
  int ReturnCount = 0;
  std::optional<Value> ReturnedValue;
  std::string ErrorMessage;
  int EntryStackDepth = 0;
  int CompletionStackDepth = 0;
  int FinalStackDepth = 0;
};

class StateTestHooks final {
public:
  static void ResetLifecycle() noexcept;
  static void FailNextCreations(std::size_t Count = 1) noexcept;
  [[nodiscard]] static StateLifecycleCounters Lifecycle() noexcept;

  [[nodiscard]] static std::optional<int>
  ObserveRootStackDepth(const State &Owner) noexcept;
  [[nodiscard]] static bool SetRootStackDepth(State &Owner, int Depth) noexcept;
  [[nodiscard]] static std::size_t BindingCount(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  PendingBindingCount(const State &Owner) noexcept;
  [[nodiscard]] static bool
  BindingIsCommitted(const State &Owner, std::string_view GlobalName) noexcept;
  [[nodiscard]] static std::optional<std::uintptr_t>
  BindingRecordAddress(const State &Owner,
                       std::string_view GlobalName) noexcept;
  [[nodiscard]] static std::optional<std::uintptr_t>
  InstalledBindingRecordAddress(const State &Owner,
                                std::string_view GlobalName) noexcept;
  [[nodiscard]] static bool SetIntegerGlobal(State &Owner,
                                             const std::string &GlobalName,
                                             int Value) noexcept;
  [[nodiscard]] static std::optional<int>
  ObserveIntegerGlobal(const State &Owner,
                       const std::string &GlobalName) noexcept;
  [[nodiscard]] static NativeInvocationObservation
  InvokeBinding(State &Owner, std::string_view GlobalName,
                const std::vector<Value> &Arguments);
  [[nodiscard]] static std::optional<CallbackStackRestorationObservation>
  ObserveLastCallbackStackRestoration(const State &Owner) noexcept;

  static void InjectFault(State &Owner, StateFaultPoint Point,
                          std::size_t Count = 1) noexcept;
  [[nodiscard]] static bool ConsumeFault(State &Owner,
                                         StateFaultPoint Point) noexcept;
  [[nodiscard]] static std::size_t
  PendingFaults(const State &Owner, StateFaultPoint Point) noexcept;
};

} // namespace Luna::Detail
