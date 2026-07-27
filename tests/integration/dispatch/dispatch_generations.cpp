// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;
std::uint64_t ExposedLifetime = 1;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "dispatch generation integration check failed: " << Description
            << '\n';
}

[[nodiscard]] int StackDepth(const Luna::State &Owner) {
  const std::optional<int> Depth = Hooks::ObserveRootStackDepth(Owner);
  return Depth ? *Depth : -1;
}

[[nodiscard]] std::string DiagnosticOf(const Luna::ExecutionResult &Result) {
  return Result.Diagnostic() == nullptr
             ? std::string()
             : std::string(Result.Diagnostic()->Message());
}

void CheckInFlightCallKeepsItsTargetWhileLaterCallsUseTheNewOne() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the host State is ready");
  const int EntryDepth = StackDepth(Owner);

  bool Published = false;
  std::size_t RetainersDuringCall = 0;
  Check(Owner.Bindings().Register("Successor", []() { return 2; }).IsSuccess(),
        "the successor callable registered");
  Check(Owner.Bindings()
            .Register("Compatible",
                      [&Owner, &Published, &RetainersDuringCall]() {
                        if (!Published)
                          Published = Hooks::RetargetDispatchSlot(
                              Owner, "Compatible", "Successor");
                        RetainersDuringCall =
                            Hooks::DispatchInvocationRetainerCount(Owner);
                        return 1;
                      })
            .IsSuccess(),
        "the compatible callable registered");

  const std::optional<std::uint64_t> Slot =
      Hooks::DispatchSlotOf(Owner, "Compatible");
  const std::optional<std::uintptr_t> OriginalRecord =
      Hooks::BindingRecordAddress(Owner, "Compatible");
  const std::optional<std::uintptr_t> SuccessorRecord =
      Hooks::BindingRecordAddress(Owner, "Successor");
  Check(Slot.has_value() && OriginalRecord.has_value() &&
            SuccessorRecord.has_value(),
        "both callables own a permanent slot and a record the store owns");
  Check(Hooks::InstalledBindingRecordAddress(Owner, "Compatible") ==
            OriginalRecord,
        "the installed path resolves to the store's own record before the "
        "publication");

  const std::uint64_t GenerationBefore = Hooks::DispatchGenerationOf(Owner);
  const Luna::ExecutionResult Executed =
      Owner.Execute("First = Compatible()\n"
                    "Second = Compatible()\n");
  Check(Executed.IsSuccess(), "the script runs both calls");
  Check(Published, "the mid-call publication happened");
  Check(RetainersDuringCall == 1,
        "exactly the running invocation retained a generation");
  Check(Hooks::ObserveIntegerGlobal(Owner, "First") == 1,
        "the call begun before the publication finished on its own target");
  Check(Hooks::ObserveIntegerGlobal(Owner, "Second") == 2,
        "the next call through the same closure used the new target");
  Check(Hooks::DispatchGenerationOf(Owner) == GenerationBefore + 1,
        "the publication advanced the dispatch generation exactly once");
  Check(StackDepth(Owner) == EntryDepth,
        "the script restores the root stack exactly");

  Check(Hooks::DispatchInvocationRetainerCount(Owner) == 0,
        "no invocation retains a generation between calls");
  Check(Hooks::SupersededDispatchGenerationCount(Owner) == 1,
        "the replaced generation is journaled");
  Check(Hooks::RetainedDispatchGenerationCount(Owner) == 0,
        "nothing retains the replaced generation once the calls are over");
  Check(Hooks::ReclaimDispatchGenerations(Owner) == 1,
        "the replaced generation is reclaimed exactly once");
  Check(Hooks::BindingRecordAddress(Owner, "Compatible") == OriginalRecord &&
            Hooks::BindingRecordAddress(Owner, "Successor") == SuccessorRecord,
        "publication never moves a record the store owns");
  Check(Hooks::InstalledDispatchSlotOf(Owner, "Compatible") == Slot,
        "the installed closure still carries exactly its permanent slot");

  Check(Hooks::RetargetDispatchSlot(Owner, "Compatible", "Compatible"),
        "the original target is published again");
  Check(Hooks::InstalledBindingRecordAddress(Owner, "Compatible") ==
            OriginalRecord,
        "the installed path resolves to the store's own record again");
  const Luna::ExecutionResult Restored = Owner.Execute("Third = Compatible()");
  Check(Restored.IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "Third") == 1,
        "the restored target answers every later call");
  Check(StackDepth(Owner) == EntryDepth,
        "every publication leaves the root stack exactly balanced");
}

void CheckRemovedSymbolRefusesInsideTheMachine() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the host State is ready");
  const int EntryDepth = StackDepth(Owner);

  Check(Owner.Bindings().Register("Removed", []() { return 11; }).IsSuccess(),
        "the callable registered");
  Check(Owner.Execute("assert(Removed() == 11)").IsSuccess(),
        "the callable answers while it is available");

  Check(Hooks::RetireDispatchSlot(Owner, "Removed"), "the slot retired");
  const Luna::ExecutionResult Recovered =
      Owner.Execute("local Ok, Error = pcall(Removed)\n"
                    "Refused = (not Ok) and 1 or 0\n"
                    "Named = (type(Error) == 'string' and\n"
                    "  string.find(Error, 'Unavailable binding') ~= nil)\n"
                    "  and 1 or 0\n"
                    "Symbol = (type(Error) == 'string' and\n"
                    "  string.find(Error, \"'Removed'\") ~= nil) and 1 or 0\n");
  Check(Recovered.IsSuccess(), "the script recovers from the refusal itself");
  Check(Hooks::ObserveIntegerGlobal(Owner, "Refused") == 1,
        "the stale closure refuses inside the machine");
  Check(Hooks::ObserveIntegerGlobal(Owner, "Named") == 1,
        "the refusal is the deterministic unavailable-binding diagnostic");
  Check(Hooks::ObserveIntegerGlobal(Owner, "Symbol") == 1,
        "the refusal names the symbol the closure was installed for");
  Check(StackDepth(Owner) == EntryDepth,
        "the refused call restores the root stack exactly");

  const Luna::ExecutionResult Unprotected = Owner.Execute("return Removed()");
  Check(!Unprotected.IsSuccess(), "an unprotected script call refuses too");
  Check(DiagnosticOf(Unprotected).find("Unavailable binding") !=
            std::string::npos,
        "the unprotected refusal carries the same deterministic sentence");
  Check(StackDepth(Owner) == EntryDepth,
        "the unprotected refusal restores the root stack exactly");

  Check(Owner.Bindings().Register("Fresh", []() { return 12; }).IsSuccess(),
        "the State still registers after the refusal");
  Check(Owner.Execute("assert(Fresh() == 12)").IsSuccess(),
        "the State still executes after the refusal");
  Check(StackDepth(Owner) == EntryDepth,
        "reuse after a refusal leaves the root stack exactly balanced");
}

void CheckEveryCallableCategoryResolvesThroughItsSlot() {
  struct Gauge final {
    int Charge = 4;

    [[nodiscard]] int Read() const { return Charge; }
    [[nodiscard]] int Add(int Other) const { return Charge + Other; }
    [[nodiscard]] static int Zero() { return 0; }
  };

  Luna::State Owner;
  Check(Owner.IsReady(), "the host State is ready");
  const int EntryDepth = StackDepth(Owner);
  Luna::BindingRegistry Registry = Owner.Bindings();

  Check(Registry.Register("Root", []() { return 1; }).IsSuccess(),
        "the root callable registered");
  Check(Registry.Register("Successor", []() { return 7; }).IsSuccess(),
        "the successor callable registered");

  Luna::NamespaceBuilder Space = Registry.RegisterNamespace("Space");
  Check(
      Space.RegisterFunction("Inner", []() { return 2; }).Commit().IsSuccess(),
      "the scoped callable registered");

  Luna::ClassBuilder<Gauge> Class = Registry.RegisterClass<Gauge>(
      "Gauge", Luna::StableTypeKey("Integration.DispatchGauge"));
  Check(Class.Method("Read", &Gauge::Read)
            .StaticMethod("Zero", &Gauge::Zero)
            .Operator(Luna::ClassOperator::Add, &Gauge::Add)
            .Commit()
            .IsSuccess(),
        "the class member, static, and operator candidates registered");

  const std::optional<Luna::SemanticVersion> Version =
      Luna::SemanticVersion::TryParse("2.1.0");
  const std::optional<Luna::ModuleManifest> Manifest =
      Luna::ModuleManifest::TryCreate(
          "integration.dispatch", Version ? *Version : Luna::SemanticVersion(),
          {}, std::string(), {});
  Check(Manifest.has_value(), "the module manifest is well formed");
  Check(Registry
            .RegisterModule(*Manifest,
                            [](Luna::NamespaceBuilder &Builder) {
                              Luna::NamespaceBuilder Module =
                                  Builder.RegisterNamespace("Module");
                              static_cast<void>(Module.RegisterFunction(
                                  "Read", []() { return 3; }));
                            })
            .IsSuccess(),
        "the module loaded");

  Gauge Value;
  Luna::Detail::ClassExposureRequest Request;
  Request.QualifiedName = "Gauge";
  Request.Path = "GaugeObject";
  Request.Storage = &Value;
  Request.Ownership = Luna::Detail::OwnershipModel::Borrowed;
  Request.Access = Luna::Detail::ConstAccess::Mutable;
  Request.LifetimeGeneration = &ExposedLifetime;
  Check(Hooks::ExposeClassUserdata(Owner, Request).Status == "created",
        "the class value is exposed exactly once");

  for (const std::string_view Path :
       {std::string_view("Root"), std::string_view("Space.Inner"),
        std::string_view("Gauge.Read"), std::string_view("Gauge.Zero"),
        std::string_view("Gauge.__LunaOperatorAdd"),
        std::string_view("Module.Read")}) {
    const std::optional<std::uint64_t> Slot =
        Hooks::DispatchSlotOf(Owner, Path);
    const std::optional<std::uintptr_t> Record =
        Hooks::BindingRecordAddress(Owner, Path);
    Check(Slot.has_value(), "every canonical callable path owns a slot");
    Check(Hooks::InstalledDispatchSlotOf(Owner, Path) == Slot,
          "every installed closure carries exactly its own slot");
    Check(Record.has_value() &&
              Hooks::InstalledBindingRecordAddress(Owner, Path) == Record,
          "every slot resolves to the store's own record");
    Check(Hooks::DispatchSlotIsAvailable(Owner, Path),
          "every published slot resolves to a target");
  }

  const Luna::ExecutionResult Every =
      Owner.Execute("assert(Root() == 1)\n"
                    "assert(Space.Inner() == 2)\n"
                    "assert(Module.Read() == 3)\n"
                    "assert(Gauge.Read(GaugeObject) == 4)\n"
                    "assert(Gauge.Zero() == 0)\n"
                    "assert(GaugeObject + 5 == 9)\n");
  Check(Every.IsSuccess(), "every callable category runs through its slot");
  Check(StackDepth(Owner) == EntryDepth,
        "exercising every category restores the root stack exactly");

  const std::uint64_t Before = Hooks::DispatchGenerationOf(Owner);
  Check(Hooks::RetargetDispatchSlot(Owner, "Gauge.Zero", "Successor"),
        "the static member's slot is retargeted");
  Check(Hooks::DispatchGenerationOf(Owner) == Before + 1,
        "the publication advanced the generation exactly once");
  const Luna::ExecutionResult After =
      Owner.Execute("assert(Root() == 1)\n"
                    "assert(Space.Inner() == 2)\n"
                    "assert(Module.Read() == 3)\n"
                    "assert(Gauge.Read(GaugeObject) == 4)\n"
                    "assert(Gauge.Zero() == 7)\n"
                    "assert(GaugeObject + 5 == 9)\n");
  Check(After.IsSuccess(),
        "only the retargeted slot changed what its closure dispatches to");
  Check(StackDepth(Owner) == EntryDepth,
        "the publication leaves the root stack exactly balanced");
}

} // namespace

int RunDispatchGenerationIntegrationTests() {
  FailureCount = 0;
  CheckInFlightCallKeepsItsTargetWhileLaterCallsUseTheNewOne();
  CheckRemovedSymbolRefusesInsideTheMachine();
  CheckEveryCallableCategoryResolvesThroughItsSlot();
  return FailureCount == 0 ? 0 : 1;
}
