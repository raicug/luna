// clang-format off
#include <luna/state/state.hpp>

#include "state/testing/test_hooks.hpp"

#include <rapidcheck.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

constexpr std::size_t SlotCount = 4;

enum class TransitionKind : std::uint32_t {
  Create,
  MoveConstruct,
  MoveAssign,
  Destroy,
  Count
};

struct ReferenceSlot final {
  bool Exists = false;
  bool OwnsVirtualMachine = false;
};

struct OwnershipModel final {
  std::array<ReferenceSlot, SlotCount> Slots{};
  std::size_t CreationAttempts = 0;
  std::size_t SuccessfulCreations = 0;
  std::size_t Releases = 0;
};

[[nodiscard]] std::size_t NormalizeSlot(int Value) noexcept {
  return static_cast<std::uint32_t>(Value) % SlotCount;
}

[[nodiscard]] TransitionKind NormalizeKind(int Value) noexcept {
  return static_cast<TransitionKind>(
      static_cast<std::uint32_t>(Value) %
      static_cast<std::uint32_t>(TransitionKind::Count));
}

void VerifyAgainstModel(
    const std::array<std::optional<Luna::State>, SlotCount> &Actual,
    const OwnershipModel &Model) {
  const auto Counters = Hooks::Lifecycle();
  RC_ASSERT(Counters.CreationAttempts == Model.CreationAttempts);
  RC_ASSERT(Counters.SuccessfulCreations == Model.SuccessfulCreations);
  RC_ASSERT(Counters.Releases == Model.Releases);

  for (std::size_t Index = 0; Index < SlotCount; ++Index) {
    RC_ASSERT(Actual[Index].has_value() == Model.Slots[Index].Exists);
    if (Model.Slots[Index].Exists) {
      RC_ASSERT(Actual[Index]->IsReady() ==
                Model.Slots[Index].OwnsVirtualMachine);
    }
  }
}

void DestroySlot(std::array<std::optional<Luna::State>, SlotCount> &Actual,
                 OwnershipModel &Model, std::size_t Slot) {
  if (!Model.Slots[Slot].Exists)
    return;

  Actual[Slot].reset();
  if (Model.Slots[Slot].OwnsVirtualMachine)
    ++Model.Releases;
  Model.Slots[Slot] = {};
}

} // namespace

int RunStateOwnershipTransitionsProperties() {
  // **Validates: Requirements 1.2, 1.3, 1.5, 1.6, 1.7, 1.9**
  // clang-format off
  // Feature: luau-binding-foundation, Property 1: State ownership follows the move-only model
  const bool Passed = rc::check(
      // clang-format on
      "State ownership follows the move-only model",
      [](const std::vector<std::array<int, 4>> &GeneratedTransitions) {
        Hooks::ResetLifecycle();

        std::array<std::optional<Luna::State>, SlotCount> Actual{};
        OwnershipModel Model;

        for (const auto &Generated : GeneratedTransitions) {
          const auto Kind = NormalizeKind(Generated[0]);
          const auto Source = NormalizeSlot(Generated[1]);
          auto Destination = NormalizeSlot(Generated[2]);
          if (Destination == Source)
            Destination = (Destination + 1) % SlotCount;
          const bool CreationSucceeds = (Generated[3] & 1) != 0;

          switch (Kind) {
          case TransitionKind::Create:
            if (!Model.Slots[Source].Exists) {
              if (!CreationSucceeds)
                Hooks::FailNextCreations();

              Actual[Source].emplace();
              Model.Slots[Source] = {true, CreationSucceeds};
              ++Model.CreationAttempts;
              if (CreationSucceeds)
                ++Model.SuccessfulCreations;
            }
            break;

          case TransitionKind::MoveConstruct:
            if (Model.Slots[Source].Exists &&
                !Model.Slots[Destination].Exists) {
              Actual[Destination].emplace(std::move(*Actual[Source]));
              Model.Slots[Destination] = {
                  true, Model.Slots[Source].OwnsVirtualMachine};
              Model.Slots[Source].OwnsVirtualMachine = false;
            }
            break;

          case TransitionKind::MoveAssign:
            if (Model.Slots[Source].Exists && Model.Slots[Destination].Exists) {
              *Actual[Destination] = std::move(*Actual[Source]);
              if (Model.Slots[Destination].OwnsVirtualMachine)
                ++Model.Releases;
              Model.Slots[Destination].OwnsVirtualMachine =
                  Model.Slots[Source].OwnsVirtualMachine;
              Model.Slots[Source].OwnsVirtualMachine = false;
            }
            break;

          case TransitionKind::Destroy:
            DestroySlot(Actual, Model, Source);
            break;

          case TransitionKind::Count:
            break;
          }

          VerifyAgainstModel(Actual, Model);
        }

        for (std::size_t Slot = 0; Slot < SlotCount; ++Slot)
          DestroySlot(Actual, Model, Slot);

        VerifyAgainstModel(Actual, Model);
        RC_ASSERT(Model.Releases == Model.SuccessfulCreations);
      });

  return Passed ? 0 : 1;
}
