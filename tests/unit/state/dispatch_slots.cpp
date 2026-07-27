// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_operator.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/dispatch/generation.hpp"
#include "state/testing/fault_injector.hpp"
#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/type/type_generation.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
// clang-format on

namespace {

using FaultPoint = Luna::Detail::StateFaultPoint;
using Hooks = Luna::Detail::StateTestHooks;
using Luna::Detail::DispatchEntry;
using Luna::Detail::DispatchGeneration;
using Luna::Detail::DispatchRetainer;
using Luna::Detail::DispatchRetention;
using Luna::Detail::DispatchSlotId;
using Luna::Detail::DispatchTable;

int FailureCount = 0;

std::uint64_t ExposedLifetime = 1;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "dispatch slot check failed: " << Description << '\n';
}

[[nodiscard]] Luna::Detail::BindingRecord *FakeTarget(std::uintptr_t Value) {
  return reinterpret_cast<Luna::Detail::BindingRecord *>(Value);
}

[[nodiscard]] DispatchEntry EntryFor(DispatchSlotId Slot, std::string Name,
                                     std::uintptr_t Target) {
  DispatchEntry Entry;
  Entry.Slot = Slot;
  Entry.QualifiedName = std::move(Name);
  Entry.Target = FakeTarget(Target);
  return Entry;
}

void TestPermanentSlotIdentity() {
  DispatchTable Table;
  Check(!Table.FindSlot("First").has_value(),
        "an unknown path owns no slot yet");
  Check(Table.IssuedSlotCount() == 0, "no slot is issued before any path");

  const DispatchSlotId First = Table.SlotFor("First");
  const DispatchSlotId Second = Table.SlotFor("Second");
  Check(First.IsValid() && Second.IsValid(), "issued slots are valid");
  Check(!(First == Second), "two paths never share one slot");
  Check(Table.SlotFor("First") == First, "one path keeps its slot identity");
  Check(Table.IssuedSlotCount() == 2, "exactly two slots were issued");
  Check(Table.FindSlot("Second").has_value() &&
            *Table.FindSlot("Second") == Second,
        "an issued slot is found by its path");
  Check(!DispatchSlotId{}.IsValid(), "the zero slot is never a valid identity");
}

void TestGenerationRetentionAndRetirement() {
  DispatchTable Table;
  const DispatchSlotId Slot = Table.SlotFor("Callable");
  const DispatchSlotId Other = Table.SlotFor("Other");

  Check(Table.Generation() == 0, "an empty table publishes generation zero");
  Check(Table.Resolve(Slot) == nullptr, "an unbound slot resolves to nothing");

  Table.Bind(Slot, "Callable", FakeTarget(0x1000), nullptr, nullptr);
  const auto First = Table.Capture();
  Check(First && First->Generation() == 1, "binding advances the generation");
  Check(Table.Resolve(Slot) == FakeTarget(0x1000),
        "a bound slot resolves to its target");
  Check(First->Find(Other) == nullptr, "an unbound slot has no entry");
  Check(First->Size() == 1 && First->AvailableCount() == 1,
        "one bound slot is one available entry");

  Table.Bind(Other, "Other", FakeTarget(0x2000), nullptr, nullptr);
  const auto Second = Table.Capture();
  Check(Second->Generation() == 2, "each publication advances the generation");
  Check(First->Size() == 1,
        "the retained generation is unchanged by a later publication");
  Check(First->Find(Slot) != nullptr &&
            First->Find(Slot)->Target == FakeTarget(0x1000),
        "the retained generation keeps its own target");

  Table.Retire(Slot);
  const auto Third = Table.Capture();
  Check(Third->Generation() == 3, "retirement publishes a new generation");
  Check(Table.Resolve(Slot) == nullptr, "a retired slot resolves to nothing");
  Check(Third->Find(Slot) != nullptr && !Third->Find(Slot)->IsAvailable(),
        "a retired slot keeps an entry that is unavailable");
  Check(Third->Find(Slot)->QualifiedName == "Callable",
        "a retired entry keeps its canonical name");
  Check(Third->AvailableCount() == 1,
        "only the retired slot became unavailable");
  Check(Second->Find(Slot) != nullptr &&
            Second->Find(Slot)->Target == FakeTarget(0x1000),
        "an invocation that captured the old generation keeps its target");
  Check(Table.FindSlot("Callable").has_value() &&
            *Table.FindSlot("Callable") == Slot,
        "retirement never releases the permanent slot identity");

  Table.Bind(Slot, "Callable", FakeTarget(0x3000), nullptr, nullptr);
  Check(Table.Resolve(Slot) == FakeTarget(0x3000),
        "a rebound slot resolves to the new target");
  Check(Table.Capture()->Size() == 2, "rebinding adds no second entry");
  Check(Third->Find(Slot)->IsAvailable() == false,
        "the retired generation stays retired");
}

void TestCanonicalEntryOrder() {
  DispatchTable Forward;
  DispatchTable Backward;
  const DispatchSlotId FirstForward = Forward.SlotFor("A");
  const DispatchSlotId SecondForward = Forward.SlotFor("B");
  const DispatchSlotId FirstBackward = Backward.SlotFor("A");
  const DispatchSlotId SecondBackward = Backward.SlotFor("B");

  Forward.Bind(FirstForward, "A", FakeTarget(0x10), nullptr, nullptr);
  Forward.Bind(SecondForward, "B", FakeTarget(0x20), nullptr, nullptr);
  Backward.Bind(SecondBackward, "B", FakeTarget(0x20), nullptr, nullptr);
  Backward.Bind(FirstBackward, "A", FakeTarget(0x10), nullptr, nullptr);

  const auto Left = Forward.Capture();
  const auto Right = Backward.Capture();
  Check(Left->Size() == Right->Size(), "both tables hold the same entries");
  bool Ordered = true;
  for (std::size_t Index = 0; Index < Left->Size(); ++Index) {
    const DispatchEntry &LeftEntry = Left->All()[Index];
    const DispatchEntry &RightEntry = Right->All()[Index];
    if (!(LeftEntry.Slot == RightEntry.Slot) ||
        LeftEntry.QualifiedName != RightEntry.QualifiedName ||
        LeftEntry.Target != RightEntry.Target)
      Ordered = false;
  }
  Check(Ordered, "entry order follows slot identity, not bind order");
}

void TestInstalledRootClosureCarriesItsSlot() {
  Luna::State Host;
  Check(Host.IsReady(), "the host State is ready");
  Check(Hooks::IssuedDispatchSlotCount(Host) == 0,
        "a fresh State has issued no slot");

  const auto Registered = Host.Bindings().Register(
      "Add", [](int Left, int Right) { return Left + Right; });
  Check(Registered.IsSuccess(), "the callable registered");

  const std::optional<std::uint64_t> Slot = Hooks::DispatchSlotOf(Host, "Add");
  const std::optional<std::uint64_t> Installed =
      Hooks::InstalledDispatchSlotOf(Host, "Add");
  Check(Slot.has_value(), "the registered path owns a permanent slot");
  Check(Installed.has_value(), "the installed closure carries a slot");
  Check(Slot == Installed, "the closure carries exactly its path's slot");
  Check(Hooks::DispatchSlotIsAvailable(Host, "Add"),
        "the published slot resolves to a target");
  Check(Hooks::DispatchGenerationOf(Host) >= 1,
        "registration published a dispatch generation");

  Check(Hooks::InstalledBindingRecordAddress(Host, "Add") ==
            Hooks::BindingRecordAddress(Host, "Add"),
        "the slot resolves to the store's own record");

  const auto Invoked = Hooks::InvokeBinding(Host, "Add", {19, 23});
  Check(Invoked.Succeeded && Invoked.ReturnCount == 1 &&
            Invoked.ReturnedValue &&
            std::get<int>(*Invoked.ReturnedValue) == 42,
        "the callable still invokes through its slot");
  Check(Invoked.FinalStackDepth == Invoked.EntryStackDepth,
        "invocation restores the stack exactly");
}

void TestSlotSurvivesFailedRegistration() {
  Luna::State Host;
  Check(Host.IsReady(), "the host State is ready");

  Hooks::InjectFault(Host, FaultPoint::BindingInstallation);
  const auto Refused = Host.Bindings().Register("Later", []() { return 1; });
  Check(!Refused.IsSuccess(), "the injected installation failure refused");

  const std::optional<std::uint64_t> Issued =
      Hooks::DispatchSlotOf(Host, "Later");
  Check(Issued.has_value(), "the failed attempt still issued a slot");
  Check(!Hooks::DispatchSlotIsAvailable(Host, "Later"),
        "the rolled-back slot is unavailable, never stale");
  Check(!Hooks::InstalledDispatchSlotOf(Host, "Later").has_value(),
        "the rolled-back path holds no closure");

  const auto Accepted = Host.Bindings().Register("Later", []() { return 7; });
  Check(Accepted.IsSuccess(), "the path registers after the failure");
  Check(Hooks::DispatchSlotOf(Host, "Later") == Issued,
        "the path keeps its permanent slot identity");
  Check(Hooks::InstalledDispatchSlotOf(Host, "Later") == Issued,
        "the new closure carries the same permanent slot");
  Check(Hooks::DispatchSlotIsAvailable(Host, "Later"),
        "the reused slot resolves to the new target");

  const auto Invoked = Hooks::InvokeBinding(Host, "Later", {});
  Check(Invoked.Succeeded && Invoked.ReturnedValue &&
            std::get<int>(*Invoked.ReturnedValue) == 7,
        "the reused slot dispatches to the new callable");
}

void TestScopedAndMemberClosuresCarrySlots() {
  struct Counter final {
    int Value = 0;

    [[nodiscard]] int Read() const { return Value; }
    [[nodiscard]] static int Zero() { return 0; }
  };

  Luna::State Host;
  Check(Host.IsReady(), "the host State is ready");

  Luna::NamespaceBuilder Space = Host.Bindings().RegisterNamespace("Space");
  const auto Scoped =
      Space.RegisterFunction("Inner", []() { return 5; }).Commit();
  Check(Scoped.IsSuccess(), "the scoped callable registered");

  Luna::ClassBuilder<Counter> Class = Host.Bindings().RegisterClass<Counter>(
      "Counter", Luna::StableTypeKey("Studio.DispatchCounter"));
  const auto Registered = Class.Method("Read", &Counter::Read)
                              .StaticMethod("Zero", &Counter::Zero)
                              .Commit();
  Check(Registered.IsSuccess(), "the class members registered");

  for (const std::string_view Path :
       {std::string_view("Space.Inner"), std::string_view("Counter.Read"),
        std::string_view("Counter.Zero")}) {
    const std::optional<std::uint64_t> Slot = Hooks::DispatchSlotOf(Host, Path);
    Check(Slot.has_value(), "a scoped or member path owns a permanent slot");
    Check(Hooks::InstalledDispatchSlotOf(Host, Path) == Slot,
          "the installed closure carries exactly that slot");
    Check(Hooks::DispatchSlotIsAvailable(Host, Path),
          "the published slot resolves to a target");
    Check(Hooks::InstalledBindingRecordAddress(Host, Path) ==
              Hooks::BindingRecordAddress(Host, Path),
          "the slot resolves to the store's own record");
  }

  const Luna::ExecutionResult Executed = Host.Execute("Result = Space.Inner()");
  Check(Executed.IsSuccess(), "the scoped callable runs through its slot");
  Check(Hooks::ObserveIntegerGlobal(Host, "Result") == 5,
        "the scoped callable produced its declared result");
}

void TestSlotsAreStateLocal() {
  Luna::State First;
  Luna::State Second;
  Check(First.IsReady() && Second.IsReady(), "both States are ready");

  Check(First.Bindings().Register("Shared", []() { return 1; }).IsSuccess(),
        "the first State registered");
  Check(Second.Bindings().Register("Other", []() { return 2; }).IsSuccess(),
        "the second State registered");
  Check(Second.Bindings().Register("Shared", []() { return 3; }).IsSuccess(),
        "the second State registered the shared name too");

  Check(!Hooks::DispatchSlotOf(First, "Other").has_value(),
        "one State never issues a slot for another State's path");
  Check(Hooks::IssuedDispatchSlotCount(First) == 1,
        "the first State issued exactly one slot");
  Check(Hooks::IssuedDispatchSlotCount(Second) == 2,
        "the second State issued exactly two slots");

  const auto FromFirst = Hooks::InvokeBinding(First, "Shared", {});
  const auto FromSecond = Hooks::InvokeBinding(Second, "Shared", {});
  Check(FromFirst.Succeeded && FromFirst.ReturnedValue &&
            std::get<int>(*FromFirst.ReturnedValue) == 1,
        "each State resolves its own target");
  Check(FromSecond.Succeeded && FromSecond.ReturnedValue &&
            std::get<int>(*FromSecond.ReturnedValue) == 3,
        "each State resolves its own target");
}

void TestSupersededGenerationsAwaitTheirRetainers() {
  DispatchTable Table;
  const DispatchSlotId Slot = Table.SlotFor("Callable");
  Table.Bind(Slot, "Callable", FakeTarget(0x1000), nullptr, nullptr);

  Check(Table.SupersededGenerationCount() == 0,
        "the first publication supersedes nothing reclaimable");
  Check(Table.TotalRetainerCount() == 0, "nothing retains anything yet");

  for (const DispatchRetainer Retainer :
       {DispatchRetainer::Invocation, DispatchRetainer::UserdataCleanup,
        DispatchRetainer::LifecycleJournal}) {
    const std::uint64_t Entered = Table.Generation();
    {
      DispatchRetention Retained = Table.Retain(Retainer);
      Check(Retained.IsHeld(), "a retention holds one generation");
      Check(Retained.GenerationNumber() == Entered,
            "a retention holds exactly the generation it was taken at");
      Check(Table.RetainerCount(Retainer) == 1,
            "the retention is accounted under its own retainer");
      Check(Table.TotalRetainerCount() == 1, "exactly one claim is live");

      Table.Bind(Slot, "Callable", FakeTarget(0x2000), nullptr, nullptr);
      Check(Table.Resolve(Slot) == FakeTarget(0x2000),
            "the published generation resolves the new target");
      Check(Retained.Find(Slot) != nullptr &&
                Retained.Find(Slot)->Target == FakeTarget(0x1000),
            "the retained generation keeps its own target");
      Check(Table.SupersededGenerationCount() == 1,
            "the replaced generation is journaled");
      Check(Table.RetainedGenerationCount() == 1,
            "the replaced generation is still retained");
      Check(Table.IsGenerationRetained(Entered),
            "the retained generation is named by its own number");
      Check(Table.ReclaimUnretained() == 0,
            "a retained generation is never reclaimed");
    }

    Check(Table.RetainerCount(Retainer) == 0,
          "the released claim is no longer accounted");
    Check(Table.RetainedGenerationCount() == 0,
          "nothing retains the superseded generation any more");
    Check(!Table.IsGenerationRetained(Entered),
          "the released generation is no longer retained");
    Check(Table.ReclaimUnretained() == 1,
          "an unretained generation is reclaimed exactly once");
    Check(Table.SupersededGenerationCount() == 0,
          "the journal is empty after reclamation");

    Table.Bind(Slot, "Callable", FakeTarget(0x1000), nullptr, nullptr);
    Check(Table.ReclaimUnretained() == 1, "the restoring publication reclaims");
  }

  DispatchRetention Early = Table.Retain(DispatchRetainer::LifecycleJournal);
  Table.Bind(Slot, "Callable", FakeTarget(0x3000), nullptr, nullptr);
  Check(Table.RetainedGenerationCount() == 1, "the early claim still retains");
  Early.Release();
  Check(!Early.IsHeld(), "a released retention holds nothing");
  Check(Table.RetainerCount(DispatchRetainer::LifecycleJournal) == 0,
        "the early claim is accounted as released");
  Check(Table.ReclaimUnretained() == 1,
        "the generation is reclaimed once the early claim is gone");
  Early.Release();
  Check(Table.RetainerCount(DispatchRetainer::LifecycleJournal) == 0,
        "releasing twice changes nothing");
}

void TestRemovalPublishesImmutableUnavailableEntries() {
  DispatchTable Table;
  const DispatchSlotId First = Table.SlotFor("First");
  const DispatchSlotId Second = Table.SlotFor("Second");
  Table.Bind(First, "First", FakeTarget(0x10), nullptr, nullptr);
  Table.Bind(Second, "Second", FakeTarget(0x20), nullptr, nullptr);

  const DispatchRetention Retained = Table.Retain(DispatchRetainer::Invocation);
  const std::uint64_t Published = Table.Generation();

  Table.Retire(First);
  Check(Table.Generation() == Published + 1, "retirement publishes once");
  const DispatchEntry *Unavailable = Table.Capture()->Find(First);
  Check(Unavailable != nullptr && !Unavailable->IsAvailable(),
        "a retired slot keeps an unavailable entry");
  Check(Unavailable->QualifiedName == "First",
        "an unavailable entry still names its symbol");
  Check(Retained.Find(First) != nullptr &&
            Retained.Find(First)->Target == FakeTarget(0x10),
        "retirement never reaches a generation someone retains");

  Table.Retire(First);
  Check(Table.Generation() == Published + 1,
        "retiring an unavailable slot publishes nothing");

  const DispatchSlotId Unbound = Table.SlotFor("Unbound");
  Table.Retire(Unbound);
  const DispatchEntry *Named = Table.Capture()->Find(Unbound);
  Check(Named != nullptr && !Named->IsAvailable() &&
            Named->QualifiedName == "Unbound",
        "an issued slot retires into a named unavailable entry");

  const std::uint64_t Before = Table.Generation();
  Table.Retire(DispatchSlotId{9999});
  Check(Table.Generation() == Before, "a foreign slot retires nothing");

  Table.RetireEverything();
  const auto Retired = Table.Capture();
  Check(Retired->AvailableCount() == 0, "no slot is reachable any more");
  Check(Retired->Size() == 3, "every issued slot still owns an entry");
  Check(Retired->Find(Second) != nullptr &&
            Retired->Find(Second)->QualifiedName == "Second",
        "each unavailable entry keeps its canonical name");
  Check(Table.FindSlot("Second").has_value() &&
            *Table.FindSlot("Second") == Second,
        "retirement never releases a permanent slot identity");
  Check(Retained.Find(Second) != nullptr &&
            Retained.Find(Second)->Target == FakeTarget(0x20),
        "the retained generation is still exactly what it was published as");
}

void TestRetainedClosureResolvesTheCurrentTarget() {
  Luna::State Host;
  Check(Host.IsReady(), "the host State is ready");

  struct MidCall final {
    std::size_t Retainers = 0;
    std::size_t Superseded = 0;
    std::size_t Retained = 0;
    std::size_t Reclaimed = 0;
    std::size_t NestedRetainers = 0;
    int NestedResult = 0;
    bool NestedSucceeded = false;
    bool Published = false;
  };
  MidCall Observed;

  Check(Host.Bindings().Register("Replaced", []() { return 1; }).IsSuccess(),
        "the original callable registered");
  Check(Host.Bindings()
            .Register("Successor",
                      [&Host, &Observed]() {
                        const std::size_t Live =
                            Hooks::DispatchInvocationRetainerCount(Host);
                        if (Live > Observed.NestedRetainers)
                          Observed.NestedRetainers = Live;
                        return 2;
                      })
            .IsSuccess(),
        "the successor callable registered");
  Check(Host.Bindings()
            .Register("Reentrant",
                      [&Host, &Observed]() {
                        Observed.Published = Hooks::RetargetDispatchSlot(
                            Host, "Replaced", "Successor");
                        Observed.Retainers =
                            Hooks::DispatchInvocationRetainerCount(Host);
                        Observed.Superseded =
                            Hooks::SupersededDispatchGenerationCount(Host);
                        Observed.Retained =
                            Hooks::RetainedDispatchGenerationCount(Host);
                        Observed.Reclaimed =
                            Hooks::ReclaimDispatchGenerations(Host);

                        const auto Nested =
                            Hooks::InvokeBinding(Host, "Replaced", {});
                        Observed.NestedSucceeded =
                            Nested.Succeeded &&
                            Nested.FinalStackDepth == Nested.EntryStackDepth;
                        if (Nested.ReturnedValue)
                          Observed.NestedResult =
                              std::get<int>(*Nested.ReturnedValue);
                        return 3;
                      })
            .IsSuccess(),
        "the reentrant callable registered");

  const std::uint64_t Before = Hooks::DispatchGenerationOf(Host);
  const auto First = Hooks::InvokeBinding(Host, "Replaced", {});
  Check(First.Succeeded && First.ReturnedValue &&
            std::get<int>(*First.ReturnedValue) == 1,
        "the original target answers before the publication");

  const auto Reentrant = Hooks::InvokeBinding(Host, "Reentrant", {});
  Check(Reentrant.Succeeded && Reentrant.ReturnedValue &&
            std::get<int>(*Reentrant.ReturnedValue) == 3,
        "the call that published finishes under its own generation");
  Check(Reentrant.FinalStackDepth == Reentrant.EntryStackDepth,
        "the reentrant call restores the stack exactly");
  Check(Observed.Published, "the mid-call publication happened");
  Check(Observed.Retainers == 1,
        "exactly the running invocation retained a generation");
  Check(Observed.Superseded == 1, "the replaced generation was journaled");
  Check(Observed.Retained == 1,
        "the generation the running call entered under is retained");
  Check(Observed.Reclaimed == 0,
        "no generation is reclaimed while a call still retains one");
  Check(Observed.NestedSucceeded,
        "a nested call runs and restores its stack while another call holds a "
        "generation");
  Check(Observed.NestedRetainers == 2,
        "the nested call is accounted alongside the call that started it");
  Check(Observed.NestedResult == 2,
        "the nested call resolves the current target");
  Check(Hooks::DispatchGenerationOf(Host) == Before + 1,
        "the publication advanced the generation exactly once");

  Check(Hooks::DispatchInvocationRetainerCount(Host) == 0,
        "no invocation retains a generation between calls");
  Check(Hooks::RetainedDispatchGenerationCount(Host) == 0,
        "the completed call released its generation");
  Check(Hooks::ReclaimDispatchGenerations(Host) == 1,
        "the superseded generation is reclaimed after the call completed");

  const auto Second = Hooks::InvokeBinding(Host, "Replaced", {});
  Check(Second.Succeeded && Second.ReturnedValue &&
            std::get<int>(*Second.ReturnedValue) == 2,
        "the retained closure resolves the current target on its next call");
  Check(Second.FinalStackDepth == Second.EntryStackDepth,
        "the retargeted call restores the stack exactly");

  const Luna::ExecutionResult Executed = Host.Execute("Result = Replaced()");
  Check(Executed.IsSuccess(), "a script call resolves the current target too");
  Check(Hooks::ObserveIntegerGlobal(Host, "Result") == 2,
        "the script observed the current target's result");
}

void TestRemovedSymbolRefusesDeterministically() {
  Luna::State Host;
  Check(Host.IsReady(), "the host State is ready");
  Check(Host.Bindings().Register("Removed", []() { return 11; }).IsSuccess(),
        "the callable registered");

  const auto Before = Hooks::InvokeBinding(Host, "Removed", {});
  Check(Before.Succeeded && Before.ReturnedValue &&
            std::get<int>(*Before.ReturnedValue) == 11,
        "the callable answers while it is available");

  Check(Hooks::RetireDispatchSlot(Host, "Removed"), "the slot retired");
  Check(!Hooks::DispatchSlotIsAvailable(Host, "Removed"),
        "the retired slot resolves to nothing");
  Check(Hooks::DispatchSlotOf(Host, "Removed").has_value(),
        "the permanent slot identity survives removal");
  Check(Hooks::InstalledDispatchSlotOf(Host, "Removed") ==
            Hooks::DispatchSlotOf(Host, "Removed"),
        "the stale closure still carries exactly that slot");

  const auto After = Hooks::InvokeBinding(Host, "Removed", {});
  Check(!After.Succeeded, "the stale closure refuses");
  Check(After.ErrorMessage.find("Unavailable binding") != std::string::npos,
        "the refusal is the deterministic unavailable-binding diagnostic");
  Check(After.ErrorMessage.find("'Removed'") != std::string::npos,
        "the refusal names the symbol the closure was installed for");
  Check(After.FinalStackDepth == After.EntryStackDepth,
        "the refusal restores the stack exactly");

  const auto Repeated = Hooks::InvokeBinding(Host, "Removed", {});
  Check(!Repeated.Succeeded && Repeated.ErrorMessage == After.ErrorMessage,
        "the refusal is the same sentence every time");

  const Luna::ExecutionResult Executed = Host.Execute("Removed()");
  Check(!Executed.IsSuccess(), "a script call refuses too");
  Check(Host.IsReady(), "the State remains usable after the refusal");
  Check(Host.Bindings().Register("Fresh", []() { return 12; }).IsSuccess(),
        "the State still registers after the refusal");
}

void TestRetainedGenerationKeepsItsCleanupMetadata() {
  DispatchTable Table;
  Luna::Detail::FaultInjector EnteredFaults;
  Luna::Detail::FaultInjector PublishedFaults;
  Luna::Detail::TypeGenerationSource EnteredTypes;
  Luna::Detail::TypeGenerationSource PublishedTypes;

  const DispatchSlotId Slot = Table.SlotFor("Callable");
  Table.Bind(Slot, "Callable", FakeTarget(0x1000), &EnteredFaults,
             &EnteredTypes);

  DispatchRetention Retained = Table.Retain(DispatchRetainer::Invocation);
  const DispatchEntry *Entered = Retained.Find(Slot);
  Check(Entered != nullptr && Entered->Faults == &EnteredFaults &&
            Entered->Types == &EnteredTypes,
        "an entry carries the fault context and canonical type source one "
        "invocation of it needs");

  Table.Bind(Slot, "Callable", FakeTarget(0x2000), &PublishedFaults,
             &PublishedTypes);
  Check(Entered->Faults == &EnteredFaults && Entered->Types == &EnteredTypes,
        "publication never rewrites the metadata a running call entered with");
  const DispatchEntry *Published = Table.Capture()->Find(Slot);
  Check(Published != nullptr && Published->Faults == &PublishedFaults &&
            Published->Types == &PublishedTypes,
        "the published entry names its own metadata");

  Entered->Faults->RecordCallbackStackRestoration(3, 3, 4);
  const auto Observed = Entered->Faults->LastCallbackStackRestoration();
  Check(Observed && Observed->EntryDepth == 3 && Observed->RestoredDepth == 3 &&
            Observed->ErrorDepth == 4,
        "the retained fault context is the one this call records through");
  Check(!PublishedFaults.LastCallbackStackRestoration().has_value(),
        "the successor's fault context recorded nothing for the earlier call");
  Check(Entered->Types->Capture() != nullptr,
        "the retained canonical type source still answers with a generation");

  Retained.Release();
  Check(Table.ReclaimUnretained() == 1,
        "the superseded generation is reclaimed once its call is over");
}

void TestFailingCallCompletesUnderTheGenerationItEntered() {
  Luna::State Host;
  Check(Host.IsReady(), "the host State is ready");

  bool Published = false;
  Check(Host.Bindings().Register("Successor", []() { return 24; }).IsSuccess(),
        "the successor callable registered");
  Check(Host.Bindings()
            .Register("Failing",
                      [&Host, &Published]() -> int {
                        Published = Hooks::RetargetDispatchSlot(Host, "Failing",
                                                                "Successor");
                        throw std::runtime_error("declared failure");
                      })
            .IsSuccess(),
        "the failing callable registered");

  const auto Failed = Hooks::InvokeBinding(Host, "Failing", {});
  Check(Published, "the mid-call publication happened");
  Check(!Failed.Succeeded, "the failing call refuses");
  Check(Failed.ErrorMessage.find("declared failure") != std::string::npos &&
            Failed.ErrorMessage.find("'Failing'") != std::string::npos,
        "the refusal is translated under the name the call entered under");
  Check(Failed.FinalStackDepth == Failed.EntryStackDepth,
        "the failing call restores the stack exactly");

  const auto Restoration = Hooks::ObserveLastCallbackStackRestoration(Host);
  Check(Restoration && Restoration->EntryDepth == Restoration->RestoredDepth,
        "the retained fault context recorded this call's exact restoration");
  Check(Hooks::DispatchInvocationRetainerCount(Host) == 0,
        "the failing call released the generation it retained");
  Check(Hooks::RetainedDispatchGenerationCount(Host) == 0,
        "nothing retains the superseded generation after the failure");

  const auto Next = Hooks::InvokeBinding(Host, "Failing", {});
  Check(Next.Succeeded && Next.ReturnedValue &&
            std::get<int>(*Next.ReturnedValue) == 24,
        "the next call through the same closure resolves the new target");
}

void ExposeBorrowed(Luna::State &Host, std::string QualifiedName,
                    std::string Path, void *Storage) {
  Luna::Detail::ClassExposureRequest Request;
  Request.QualifiedName = std::move(QualifiedName);
  Request.Path = std::move(Path);
  Request.Storage = Storage;
  Request.Ownership = Luna::Detail::OwnershipModel::Borrowed;
  Request.Access = Luna::Detail::ConstAccess::Mutable;
  Request.LifetimeGeneration = &ExposedLifetime;
  Check(Hooks::ExposeClassUserdata(Host, Request).Status == "created",
        "the borrowed value is exposed exactly once");
}

void TestOperatorClosuresResolveThroughTheirSlots() {
  struct Operand final {
    int Value = 4;

    [[nodiscard]] int Add(int Other) const { return Value + Other; }
    [[nodiscard]] int Negate() const { return -Value; }
  };

  Luna::State Host;
  Check(Host.IsReady(), "the host State is ready");

  Luna::ClassBuilder<Operand> Class = Host.Bindings().RegisterClass<Operand>(
      "Operand", Luna::StableTypeKey("Studio.DispatchOperand"));
  Check(Class.Operator(Luna::ClassOperator::Add, &Operand::Add)
            .Operator(Luna::ClassOperator::Negate, &Operand::Negate)
            .Commit()
            .IsSuccess(),
        "the operator candidates registered");

  const std::string_view AddPath = "Operand.__LunaOperatorAdd";
  const std::string_view NegatePath = "Operand.__LunaOperatorNegate";
  for (const std::string_view Path : {AddPath, NegatePath}) {
    const std::optional<std::uint64_t> Slot = Hooks::DispatchSlotOf(Host, Path);
    Check(Slot.has_value(), "an operator candidate owns a permanent slot");
    Check(Hooks::InstalledDispatchSlotOf(Host, Path) == Slot,
          "the installed operator closure carries exactly that slot");
    Check(Hooks::DispatchSlotIsAvailable(Host, Path),
          "the published operator slot resolves to a target");
    const std::optional<std::uintptr_t> Record =
        Hooks::BindingRecordAddress(Host, Path);
    Check(Record.has_value() &&
              Hooks::InstalledBindingRecordAddress(Host, Path) == Record,
          "the operator slot resolves to the store's own record");
  }

  Operand Value;
  ExposeBorrowed(Host, "Operand", "OperandObject", &Value);
  const Luna::ExecutionResult Both =
      Host.Execute("assert(OperandObject + 3 == 7)\n"
                   "assert(-OperandObject == -4)\n");
  Check(Both.IsSuccess(), "both operators dispatch through their slots");

  Check(Hooks::RetireDispatchSlot(Host, AddPath), "the operator slot retired");
  Check(!Hooks::DispatchSlotIsAvailable(Host, AddPath),
        "the retired operator slot resolves to nothing");
  Check(Hooks::InstalledDispatchSlotOf(Host, AddPath) ==
            Hooks::DispatchSlotOf(Host, AddPath),
        "the stale operator closure still carries its permanent slot");

  const Luna::ExecutionResult Removed =
      Host.Execute("return OperandObject + 3");
  Check(!Removed.IsSuccess(), "the removed operator refuses");
  const std::string Diagnostic =
      Removed.Diagnostic() ? std::string(Removed.Diagnostic()->Message())
                           : std::string();
  Check(Diagnostic.find("Unavailable binding") != std::string::npos,
        "the operator refusal is the deterministic unavailable-binding "
        "diagnostic");
  Check(Diagnostic.find("__LunaOperatorAdd") != std::string::npos,
        "the operator refusal names the symbol it was installed for");

  const Luna::ExecutionResult Sibling =
      Host.Execute("assert(-OperandObject == -4)");
  Check(Sibling.IsSuccess(),
        "removing one operator leaves every other slot exactly as it was");
  Check(Host.IsReady(), "the State remains usable after the operator refusal");
}

void TestModuleContributedClosuresResolveThroughTheirSlots() {
  Luna::State Host;
  Check(Host.IsReady(), "the host State is ready");
  Luna::BindingRegistry Registry = Host.Bindings();

  const auto Version = Luna::SemanticVersion::TryParse("1.0.0");
  const auto Manifest = Luna::ModuleManifest::TryCreate(
      "studio.dispatch", Version ? *Version : Luna::SemanticVersion(), {},
      std::string(), {});
  Check(Manifest.has_value(), "the module manifest is well formed");
  Check(Registry
            .RegisterModule(*Manifest,
                            [](Luna::NamespaceBuilder &Builder) {
                              Luna::NamespaceBuilder Space =
                                  Builder.RegisterNamespace("Dispatch");
                              static_cast<void>(Space.RegisterFunction(
                                  "Read", []() { return 5; }));
                            })
            .IsSuccess(),
        "the module loaded");

  const std::string_view Path = "Dispatch.Read";
  const std::optional<std::uint64_t> Slot = Hooks::DispatchSlotOf(Host, Path);
  Check(Slot.has_value(), "the module callable owns a permanent slot");
  Check(Hooks::InstalledDispatchSlotOf(Host, Path) == Slot,
        "the closure the module installed carries exactly that slot");
  const std::optional<std::uintptr_t> Record =
      Hooks::BindingRecordAddress(Host, Path);
  Check(Record.has_value() &&
            Hooks::InstalledBindingRecordAddress(Host, Path) == Record,
        "the module callable's slot resolves to the store's own record");

  Check(Host.Execute("Result = Dispatch.Read()").IsSuccess(),
        "the module callable runs through its slot");
  Check(Hooks::ObserveIntegerGlobal(Host, "Result") == 5,
        "the module callable produced its declared result");

  Check(Registry.Register("Successor", []() { return 6; }).IsSuccess(),
        "the successor callable registered");
  const std::uint64_t Before = Hooks::DispatchGenerationOf(Host);
  Check(Hooks::RetargetDispatchSlot(Host, Path, "Successor"),
        "the module callable's slot is retargeted");
  Check(Hooks::DispatchGenerationOf(Host) == Before + 1,
        "the publication advanced the generation exactly once");
  Check(Hooks::InstalledDispatchSlotOf(Host, Path) == Slot,
        "retargeting never touches the closure the module installed");

  Check(Host.Execute("Result = Dispatch.Read()").IsSuccess(),
        "the retargeted module callable still runs");
  Check(Hooks::ObserveIntegerGlobal(Host, "Result") == 6,
        "the module callable's closure resolves the current target");
}

void TestRetainedGenerationOutlivesTheClosuresItNamed() {
  DispatchRetention Journal;
  std::uint64_t Number = 0;
  DispatchSlotId Root;
  DispatchSlotId Scoped;

  {
    Luna::State Host;
    Check(Host.IsReady(), "the host State is ready");
    Check(Host.Bindings().Register("Root", []() { return 1; }).IsSuccess(),
          "the root callable registered");
    Luna::NamespaceBuilder Space = Host.Bindings().RegisterNamespace("Space");
    Check(Space.RegisterFunction("Inner", []() { return 2; })
              .Commit()
              .IsSuccess(),
          "the scoped callable registered");

    const std::optional<std::uint64_t> RootSlot =
        Hooks::DispatchSlotOf(Host, "Root");
    const std::optional<std::uint64_t> ScopedSlot =
        Hooks::DispatchSlotOf(Host, "Space.Inner");
    Check(RootSlot.has_value() && ScopedSlot.has_value(),
          "both paths own permanent slots");
    Root = DispatchSlotId{RootSlot ? *RootSlot : 0};
    Scoped = DispatchSlotId{ScopedSlot ? *ScopedSlot : 0};

    Journal = Hooks::RetainDispatchGeneration(
        Host, DispatchRetainer::LifecycleJournal);
    Number = Journal.GenerationNumber();
    Check(Journal.IsHeld() && Number != 0,
          "the journal holds the generation it was taken at");
    Check(Journal.Find(Root) != nullptr && Journal.Find(Root)->IsAvailable(),
          "the retained generation resolves the callable it was taken over");
  }

  const auto Destroyed = Hooks::ObserveLastStateDestruction();
  Check(Destroyed.Observed && Destroyed.RefusedNewInvocations,
        "destruction refused every new invocation before the machine closed");
  Check(Destroyed.RetainedCleanupMetadata && Destroyed.IncompleteMetadata == 0,
        "every cleanup step kept the metadata it requires");

  Check(Journal.IsHeld(), "the retained generation survives its State");
  Check(Journal.GenerationNumber() == Number,
        "the retained generation is still exactly the one that was held");
  const DispatchEntry *RootEntry = Journal.Find(Root);
  const DispatchEntry *ScopedEntry = Journal.Find(Scoped);
  Check(RootEntry != nullptr && RootEntry->QualifiedName == "Root",
        "the retained generation still owns the canonical names it published");
  Check(ScopedEntry != nullptr && ScopedEntry->QualifiedName == "Space.Inner",
        "every retained entry keeps its own canonical name");

  Journal.Release();
  Check(!Journal.IsHeld(), "the claim is given back after its table is gone");
  Journal.Release();
  Check(!Journal.IsHeld(), "releasing twice changes nothing");
}

} // namespace

int RunDispatchSlotIndirectionTests() {
  FailureCount = 0;
  TestPermanentSlotIdentity();
  TestGenerationRetentionAndRetirement();
  TestCanonicalEntryOrder();
  TestInstalledRootClosureCarriesItsSlot();
  TestSlotSurvivesFailedRegistration();
  TestScopedAndMemberClosuresCarrySlots();
  TestSlotsAreStateLocal();
  TestSupersededGenerationsAwaitTheirRetainers();
  TestRemovalPublishesImmutableUnavailableEntries();
  TestRetainedClosureResolvesTheCurrentTarget();
  TestRemovedSymbolRefusesDeterministically();
  TestRetainedGenerationKeepsItsCleanupMetadata();
  TestFailingCallCompletesUnderTheGenerationItEntered();
  TestOperatorClosuresResolveThroughTheirSlots();
  TestModuleContributedClosuresResolveThroughTheirSlots();
  TestRetainedGenerationOutlivesTheClosuresItNamed();
  return FailureCount == 0 ? 0 : 1;
}
