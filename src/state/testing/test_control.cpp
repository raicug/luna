// clang-format off
#include "state/testing/test_control.hpp"

#include <atomic>
// clang-format on

namespace Luna::Detail {
namespace {
std::atomic_size_t CreationAttempts{0};
std::atomic_size_t SuccessfulCreations{0};
std::atomic_size_t Releases{0};
std::atomic_size_t CreationFailures{0};
} // namespace

void StateTestControl::RecordCreationAttempt() noexcept { ++CreationAttempts; }
void StateTestControl::RecordSuccessfulCreation() noexcept {
  ++SuccessfulCreations;
}
void StateTestControl::RecordRelease() noexcept { ++Releases; }

bool StateTestControl::ConsumeCreationFailure() noexcept {
  auto Remaining = CreationFailures.load();
  while (Remaining != 0 &&
         !CreationFailures.compare_exchange_weak(Remaining, Remaining - 1)) {
  }
  return Remaining != 0;
}

void StateTestControl::ResetLifecycle() noexcept {
  CreationAttempts = 0;
  SuccessfulCreations = 0;
  Releases = 0;
  CreationFailures = 0;
}

void StateTestControl::FailNextCreations(std::size_t Count) noexcept {
  CreationFailures = Count;
}

StateLifecycleCounters StateTestControl::Counters() noexcept {
  return {CreationAttempts.load(), SuccessfulCreations.load(), Releases.load()};
}

} // namespace Luna::Detail
