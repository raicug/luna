// clang-format off
#include <luna/state/state.hpp>

#include "state/testing/test_hooks.hpp"

#include <array>
#include <iostream>
#include <string_view>
#include <type_traits>
#include <utility>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

[[nodiscard]] bool HasCounters(std::size_t Attempts, std::size_t Successes,
                               std::size_t Releases) {
  const auto Counters = Hooks::Lifecycle();
  return Counters.CreationAttempts == Attempts &&
         Counters.SuccessfulCreations == Successes &&
         Counters.Releases == Releases;
}

[[nodiscard]] bool OneCreationAttempt() {
  Hooks::ResetLifecycle();
  {
    Luna::State State;
    if (!State.IsReady() || !HasCounters(1, 1, 0))
      return false;
  }
  return HasCounters(1, 1, 1);
}

[[nodiscard]] bool FailedCreationStaysEmpty() {
  Hooks::ResetLifecycle();
  Hooks::FailNextCreations();
  {
    Luna::State State;
    if (State.IsReady() || !HasCounters(1, 0, 0))
      return false;
  }
  return HasCounters(1, 0, 0);
}

[[nodiscard]] bool SelfMovePreservesOwnership() {
  Hooks::ResetLifecycle();
  {
    Luna::State Owner;
    Owner = std::move(Owner);
    if (!Owner.IsReady() || !HasCounters(1, 1, 0))
      return false;
  }
  if (!HasCounters(1, 1, 1))
    return false;

  Hooks::ResetLifecycle();
  Hooks::FailNextCreations();
  Luna::State Empty;
  Empty = std::move(Empty);
  return !Empty.IsReady() && HasCounters(1, 0, 0);
}

[[nodiscard]] bool MoveConstructionEmptiesSource() {
  Hooks::ResetLifecycle();
  {
    Luna::State Source;
    Luna::State Destination(std::move(Source));
    if (Source.IsReady() || !Destination.IsReady() || !HasCounters(1, 1, 0))
      return false;
  }
  return HasCounters(1, 1, 1);
}

[[nodiscard]] bool DestinationReplacementReleasesOldOwner() {
  Hooks::ResetLifecycle();
  {
    Luna::State Destination;
    Luna::State Source;
    Destination = std::move(Source);
    if (!Destination.IsReady() || Source.IsReady() || !HasCounters(2, 2, 1))
      return false;
  }
  return HasCounters(2, 2, 2);
}

[[nodiscard]] bool EmptyDestinationNeedsNoReplacementRelease() {
  Hooks::ResetLifecycle();
  Hooks::FailNextCreations();
  {
    Luna::State Destination;
    Luna::State Source;
    if (Destination.IsReady() || !Source.IsReady() || !HasCounters(2, 1, 0))
      return false;

    Destination = std::move(Source);
    if (!Destination.IsReady() || Source.IsReady() || !HasCounters(2, 1, 0))
      return false;
  }
  return HasCounters(2, 1, 1);
}

[[nodiscard]] bool DestructionReleasesOnlyOwners() {
  Hooks::ResetLifecycle();
  {
    Luna::State Owner;
  }
  if (!HasCounters(1, 1, 1))
    return false;

  Hooks::ResetLifecycle();
  Hooks::FailNextCreations();
  {
    Luna::State Empty;
  }
  return HasCounters(1, 0, 0);
}

struct TestCase final {
  std::string_view Category;
  bool (*Run)();
};

} // namespace

static_assert(!std::is_copy_constructible_v<Luna::State>);
static_assert(!std::is_copy_assignable_v<Luna::State>);
static_assert(std::is_move_constructible_v<Luna::State>);
static_assert(std::is_move_assignable_v<Luna::State>);
static_assert(std::is_nothrow_move_constructible_v<Luna::State>);
static_assert(std::is_nothrow_move_assignable_v<Luna::State>);

int RunStateOwnershipTests() {
  constexpr std::array Tests{
      TestCase{"creation/one-attempt", OneCreationAttempt},
      TestCase{"creation/failure", FailedCreationStaysEmpty},
      TestCase{"move/self-move", SelfMovePreservesOwnership},
      TestCase{"move/moved-from-readiness", MoveConstructionEmptiesSource},
      TestCase{"move/destination-replacement",
               DestinationReplacementReleasesOldOwner},
      TestCase{"move/empty-destination",
               EmptyDestinationNeedsNoReplacementRelease},
      TestCase{"destruction/exact-releases", DestructionReleasesOnlyOwners},
  };

  for (const auto &Test : Tests) {
    if (!Test.Run()) {
      std::cerr << "State ownership test failed: " << Test.Category << '\n';
      return 1;
    }
  }
  return 0;
}
