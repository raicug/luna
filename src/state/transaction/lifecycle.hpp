#pragma once

// Logical identity, thread affinity, phase, and epochs of one State. These
// values live in `State::Impl`, so they travel with the implementation a move
// transfers: the logical State identity and the owner thread stay the same for
// the life of the implementation, while the owner-object epoch advances every
// time a different owner object takes it over. The lifecycle generation
// advances when the registered model is replaced, which is how a builder
// captured earlier detects that it has gone stale.

// clang-format off
#include <compare>
#include <cstdint>
#include <thread>
// clang-format on

namespace Luna::Detail {

// Phase of one State's registered model. `Ready` accepts mutation; `Frozen`
// rejects it unless an explicit dynamic lifecycle mode is active. Freeze is an
// explicit later operation, so nothing enters `Frozen` on its own; the phase
// lives here because every transaction captures it in one place.
enum class LifecyclePhase { Ready, Frozen };

// Logical identity of one State. It is process-monotonic, never an address, and
// never reused, so two live States can never share one identity.
class StateIdentity final {
public:
  constexpr StateIdentity() noexcept = default;

  [[nodiscard]] static StateIdentity Next() noexcept;

  [[nodiscard]] constexpr std::uint64_t Value() const noexcept {
    return ValueStorage;
  }

  // A default-constructed identity is the reserved unassigned value.
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

  // A new owner object took over this implementation, so every handle captured
  // against the previous owner object is stale.
  void AdvanceOwnerEpoch() noexcept { ++OwnerEpochValue; }

  [[nodiscard]] std::uint64_t Generation() const noexcept {
    return GenerationValue;
  }

  void AdvanceGeneration() noexcept { ++GenerationValue; }

  // The construction thread of the State. A move transfers this affinity
  // instead of adopting the thread that performed the move, because the value
  // travels with the implementation.
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

  // Entering the frozen phase. Only an explicit freeze operation calls this;
  // registration never changes the phase on its own.
  void Freeze() noexcept { PhaseValue = LifecyclePhase::Frozen; }

private:
  StateIdentity IdentityValue;
  std::thread::id OwnerThreadValue = std::this_thread::get_id();
  LifecyclePhase PhaseValue = LifecyclePhase::Ready;
  std::uint64_t OwnerEpochValue = 1;
  std::uint64_t GenerationValue = 0;
};

} // namespace Luna::Detail
