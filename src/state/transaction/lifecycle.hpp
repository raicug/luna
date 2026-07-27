#pragma once

// clang-format off
#include <compare>
#include <cstdint>
#include <thread>
// clang-format on

namespace Luna::Detail {

enum class LifecyclePhase { Ready, Frozen };

class StateIdentity final {
public:
  constexpr StateIdentity() noexcept = default;

  [[nodiscard]] static StateIdentity Next() noexcept;

  [[nodiscard]] constexpr std::uint64_t Value() const noexcept {
    return ValueStorage;
  }

  [[nodiscard]] constexpr bool IsValid() const noexcept {
    return ValueStorage != 0;
  }

  [[nodiscard]] friend constexpr bool
  operator==(const StateIdentity &Left, const StateIdentity &Right) noexcept {
    return Left.ValueStorage == Right.ValueStorage;
  }

  [[nodiscard]] friend constexpr std::strong_ordering
  operator<=>(const StateIdentity &Left, const StateIdentity &Right) noexcept {
    return Left.ValueStorage <=> Right.ValueStorage;
  }

private:
  std::uint64_t ValueStorage = 0;
};

class StateLifecycle final {
public:
  StateLifecycle() noexcept : IdentityValue(StateIdentity::Next()) {}

  [[nodiscard]] StateIdentity Identity() const noexcept {
    return IdentityValue;
  }

  [[nodiscard]] std::uint64_t OwnerEpoch() const noexcept {
    return OwnerEpochValue;
  }

  void AdvanceOwnerEpoch() noexcept { ++OwnerEpochValue; }

  [[nodiscard]] std::uint64_t Generation() const noexcept {
    return GenerationValue;
  }

  void AdvanceGeneration() noexcept { ++GenerationValue; }

  [[nodiscard]] std::thread::id OwnerThread() const noexcept {
    return OwnerThreadValue;
  }

  [[nodiscard]] bool IsOwnerThread() const noexcept {
    return std::this_thread::get_id() == OwnerThreadValue;
  }

  [[nodiscard]] LifecyclePhase Phase() const noexcept { return PhaseValue; }

  [[nodiscard]] bool IsFrozen() const noexcept {
    return PhaseValue == LifecyclePhase::Frozen;
  }

  void Freeze() noexcept { PhaseValue = LifecyclePhase::Frozen; }

private:
  StateIdentity IdentityValue;
  std::thread::id OwnerThreadValue = std::this_thread::get_id();
  LifecyclePhase PhaseValue = LifecyclePhase::Ready;
  std::uint64_t OwnerEpochValue = 1;
  std::uint64_t GenerationValue = 0;
};

} // namespace Luna::Detail
