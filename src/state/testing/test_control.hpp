#pragma once

// clang-format off
#include <cstddef>
// clang-format on

namespace Luna::Detail {

struct StateLifecycleCounters {
  std::size_t CreationAttempts = 0;
  std::size_t SuccessfulCreations = 0;
  std::size_t Releases = 0;
};

class StateTestControl final {
public:
  static void RecordCreationAttempt() noexcept;
  static void RecordSuccessfulCreation() noexcept;
  static void RecordRelease() noexcept;
  [[nodiscard]] static bool ConsumeCreationFailure() noexcept;

  static void ResetLifecycle() noexcept;
  static void FailNextCreations(std::size_t Count) noexcept;
  [[nodiscard]] static StateLifecycleCounters Counters() noexcept;
};

} // namespace Luna::Detail
