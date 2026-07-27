// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/callable_descriptor.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/detail/callable_adapter.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>

#include "state/reflection/database.hpp"
#include "state/registration/submission.hpp"
#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/transaction/generation_set.hpp"
#include "state/transaction/transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Luna::Detail::CallbackBoundaryObservation;
using Luna::Detail::JoinedFunctionDeclaration;
using FaultPoint = Luna::Detail::StateFaultPoint;
using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "unified transaction integration check failed: " << Description
            << '\n';
}

int AdderCalls = 0;

[[nodiscard]] int AddIntegers(int Left, int Right) {
  ++AdderCalls;
  return Left + Right;
}

[[nodiscard]] bool HasFailure(const Luna::RegistrationResult &Result,
                              Luna::ErrorCategory Category,
                              std::string_view Fragment) {
  return !Result.IsSuccess() && Result.Diagnostic() != nullptr &&
         Result.Diagnostic()->Category() == Category &&
         !Result.Diagnostic()->Message().empty() &&
         Result.Diagnostic()->Message().find(Fragment) != std::string::npos;
}

[[nodiscard]] std::string PathKind(Luna::State &Owner,
                                   const std::string &Path) {
  return Hooks::ObserveVmPathValueKind(Owner, Path).value_or("<unavailable>");
}

struct CommittedBaseline final {
  std::shared_ptr<const Luna::Detail::GenerationSet> Generations;
  std::size_t Bindings = 0;
  std::uint64_t ReflectionGeneration = 0;
  std::optional<int> RootStackDepth;
  std::optional<std::uintptr_t> AdderRecord;
  std::optional<std::uintptr_t> CounterRecord;
  int CounterValue = 0;
};

[[nodiscard]] CommittedBaseline Capture(Luna::State &Owner) {
  CommittedBaseline Baseline;
  Baseline.Generations = Hooks::GenerationsOf(Owner);
  Baseline.Bindings = Hooks::BindingCount(Owner);
  Baseline.ReflectionGeneration = Hooks::ReflectionGeneration(Owner);
  Baseline.RootStackDepth = Hooks::ObserveRootStackDepth(Owner);
  Baseline.AdderRecord = Hooks::BindingRecordAddress(Owner, "Add");
  Baseline.CounterRecord = Hooks::BindingRecordAddress(Owner, "Counted");
  return Baseline;
}

void CheckCommittedModelSurvived(Luna::State &Owner,
                                 const CommittedBaseline &Baseline,
                                 const std::string &Attempted,
                                 std::string_view ExpectedKind,
                                 int &CounterCalls) {
  Check(Hooks::GenerationsOf(Owner) == Baseline.Generations,
        "a failed attempt publishes no generation set");
  Check(Hooks::BindingCount(Owner) == Baseline.Bindings &&
            Hooks::PendingBindingCount(Owner) == 0,
        "a failed attempt commits nothing and stages nothing");
  Check(Hooks::ReflectionGeneration(Owner) == Baseline.ReflectionGeneration,
        "a failed attempt publishes no reflection generation");
  Check(Hooks::ObserveRootStackDepth(Owner) == Baseline.RootStackDepth,
        "a failed attempt restores the exact root stack depth");
  Check(!Hooks::HasActiveTransaction(Owner),
        "a failed attempt leaves no active transaction");
  Check(PathKind(Owner, Attempted) == ExpectedKind,
        "a failed attempt leaves the canonical path exactly as it was");

  Check(Hooks::BindingRecordAddress(Owner, "Add") == Baseline.AdderRecord &&
            Hooks::InstalledBindingRecordAddress(Owner, "Add") ==
                Baseline.AdderRecord,
        "the committed function pointer keeps one identity everywhere");
  Check(Hooks::BindingRecordAddress(Owner, "Counted") ==
                Baseline.CounterRecord &&
            Hooks::InstalledBindingRecordAddress(Owner, "Counted") ==
                Baseline.CounterRecord,
        "the committed closure keeps one identity everywhere");

  const int AdderBefore = AdderCalls;
  const int CounterBefore = CounterCalls;
  Check(Owner.Execute("Preserved = Add(30, 12) + Counted(1)").IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "Preserved") == 44,
        "every original callable keeps its behavior after a failed attempt");
  Check(AdderCalls == AdderBefore + 1 && CounterCalls == CounterBefore + 1,
        "every original callable is invoked exactly once per call");
}

void CheckSuccessPathThroughTheRealVirtualMachine() {
  Luna::State Owner;
  Check(Owner.IsReady(), "a fresh State owns a ready virtual machine");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  auto *Database = Hooks::ReflectionDatabaseOf(Owner);
  Check(Database != nullptr, "a State owns one reflection database");
  if (!Database)
    return;
  const Luna::ReflectionSnapshot Before = Database->Snapshot();

  AdderCalls = 0;
  int CounterCalls = 0;
  int VoidCalls = 0;
  Check(Owner.Bindings().Register("Add", &AddIntegers).IsSuccess(),
        "a function pointer registers");
  Check(Owner.Bindings()
            .Register("Counted",
                      [&CounterCalls](int Value) {
                        ++CounterCalls;
                        return Value + 1;
                      })
            .IsSuccess(),
        "a closure registers");
  Check(Owner.Bindings()
            .Register("Observe", [&VoidCalls]() { ++VoidCalls; })
            .IsSuccess(),
        "a void closure registers");

  const auto Generations = Hooks::GenerationsOf(Owner);
  Check(Generations->Generation() == 3 && Generations->Symbols().Size() == 3,
        "each successful attempt publishes exactly one generation");
  for (const std::string &Name :
       std::vector<std::string>{"Add", "Counted", "Observe"}) {
    Check(Generations->Symbols().Contains(Name),
          "the published generation describes every registered symbol");
    Check(Hooks::BindingIsCommitted(Owner, Name) &&
              Hooks::InstalledBindingRecordAddress(Owner, Name) ==
                  Hooks::BindingRecordAddress(Owner, Name),
          "reflection, the virtual machine, and dispatch agree");
    Check(PathKind(Owner, Name) == "function",
          "every published declaration holds its installed closure");
  }
  Check(Hooks::PendingBindingCount(Owner) == 0 &&
            Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "publication stages nothing and leaves the entry stack depth");

  const auto Execution = Owner.Execute("Sum = Add(19, 23)\n"
                                       "Next = Counted(41)\n"
                                       "assert(select('#', Observe()) == 0)");
  Check(Execution.IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "Sum") == 42 &&
            Hooks::ObserveIntegerGlobal(Owner, "Next") == 42,
        "every published declaration runs through the real virtual machine");
  Check(AdderCalls == 1 && CounterCalls == 1 && VoidCalls == 1,
        "every published declaration is invoked exactly once");
  Check(Before.Generation() == 0 && Before.IsEmpty(),
        "a snapshot taken before the attempts keeps its own generation");
}

void CheckEveryFailureFamilyPreservesTheState() {
  Luna::State Owner;
  AdderCalls = 0;
  int CounterCalls = 0;
  Check(Owner.Bindings().Register("Add", &AddIntegers).IsSuccess() &&
            Owner.Bindings()
                .Register("Counted",
                          [&CounterCalls](int Value) {
                            ++CounterCalls;
                            return Value + 1;
                          })
                .IsSuccess(),
        "the baseline declarations publish");
  Check(Owner.Execute("Baseline = Add(1, 1) + Counted(1)").IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "Baseline") == 4,
        "the baseline declarations run before any failure");
  Check(Owner.Execute("Occupied = 'script'").IsSuccess(),
        "a script-created value occupies one canonical path");

  std::size_t Recoveries = 0;
  const auto Recover = [&Owner, &Recoveries]() {
    const std::string Name = "Recovered" + std::to_string(++Recoveries);
    const auto Registered = Owner.Bindings().Register(Name, &AddIntegers);
    const auto Execution = Owner.Execute(Name + "Result = " + Name + "(7, 8)");
    Check(Registered.IsSuccess() && Execution.IsSuccess() &&
              Hooks::ObserveIntegerGlobal(Owner, Name + "Result") == 15,
          "the State registers and executes after every failed attempt");
  };

  {
    const auto Baseline = Capture(Owner);
    const auto Invalid = Owner.Bindings().Register("Bad-Name", &AddIntegers);
    Check(HasFailure(Invalid, Luna::ErrorCategory::InvalidGlobalName, "byte 4"),
          "an invalid identifier is rejected deterministically");
    CheckCommittedModelSurvived(Owner, Baseline, "Bad-Name", "absent",
                                CounterCalls);
    Recover();
  }
  {
    const auto Baseline = Capture(Owner);
    const auto Duplicate = Owner.Bindings().Register("Add", &AddIntegers);
    Check(HasFailure(Duplicate, Luna::ErrorCategory::DuplicateGlobalName,
                     "already registered"),
          "a duplicate name is rejected from the canonical model");
    CheckCommittedModelSurvived(Owner, Baseline, "Add", "function",
                                CounterCalls);
    Recover();
  }
  {
    const auto Baseline = Capture(Owner);
    int (*NullTarget)(int, int) = nullptr;
    const auto Null = Owner.Bindings().Register("NullTarget", NullTarget);
    Check(HasFailure(Null, Luna::ErrorCategory::NullCallable,
                     "callable target is null"),
          "a null callable target is rejected deterministically");
    CheckCommittedModelSurvived(Owner, Baseline, "NullTarget", "absent",
                                CounterCalls);
    Recover();
  }

  struct FaultCase final {
    FaultPoint Point;
    std::string Name;
    std::string Fragment;
    std::string PriorKind;
    std::string_view Description;
  };

  const std::vector<FaultCase> Faults{
      {FaultPoint::TransactionPreparation, "Prepared", "preparation", "absent",
       "a preparation fault fails before anything is staged"},
      {FaultPoint::BindingRecordAllocation, "Allocated",
       "Could not allocate binding record", "absent",
       "an allocation fault fails before anything is installed"},
      {FaultPoint::BindingPathJournal, "Journalled",
       "could not journal the prior value", "absent",
       "a journal fault fails before the path is written"},
      {FaultPoint::BindingInstallation, "Installed", "installation failed",
       "absent", "an installation fault is rolled back"},
      {FaultPoint::TransactionPublication, "Published", "preparation", "absent",
       "a publication-preparation fault is rejected"},
      {FaultPoint::TransactionConsistency, "Checked",
       "internal metadata contradicted", "absent",
       "a metadata contradiction is rejected before publication"},
      {FaultPoint::BindingInstallation, "Occupied", "installation failed",
       "string", "an installation fault restores the exact prior value"}};

  for (const FaultCase &Case : Faults) {
    const auto Baseline = Capture(Owner);
    Hooks::InjectFault(Owner, Case.Point);
    const auto Failed = Owner.Bindings().Register(Case.Name, &AddIntegers);
    Check(HasFailure(Failed, Luna::ErrorCategory::Internal, Case.Fragment),
          Case.Description);
    Check(Hooks::PendingFaults(Owner, Case.Point) == 0,
          "the injected fault is consumed exactly once");
    CheckCommittedModelSurvived(Owner, Baseline, Case.Name, Case.PriorKind,
                                CounterCalls);
    Recover();
  }
  Check(Owner.Execute("assert(Occupied == 'script')").IsSuccess(),
        "the overwritten script value is exactly what it was");

  {
    const auto Baseline = Capture(Owner);
    Hooks::InjectFault(Owner, FaultPoint::BindingInstallation);
    Hooks::InjectFault(Owner, FaultPoint::TransactionUndo);
    const auto Failed = Owner.Bindings().Register("Unrestored", &AddIntegers);
    Check(HasFailure(Failed, Luna::ErrorCategory::Internal,
                     "internal rollback failed"),
          "a failed restoration is reported deterministically");
    Check(Hooks::GenerationsOf(Owner) == Baseline.Generations &&
              Hooks::BindingCount(Owner) == Baseline.Bindings &&
              Hooks::PendingBindingCount(Owner) == 0,
          "a failed restoration still publishes nothing and stages nothing");
    Check(!Hooks::HasActiveTransaction(Owner),
          "a failed restoration leaves no active transaction");
  }

  {
    Luna::State Grouped;
    AdderCalls = 0;
    Check(Grouped.Bindings().Register("Add", &AddIntegers).IsSuccess(),
          "the grouped baseline publishes");
    const auto EntryDepth = Hooks::ObserveRootStackDepth(Grouped);
    const auto Generations = Hooks::GenerationsOf(Grouped);

    std::vector<JoinedFunctionDeclaration> Group;
    Group.emplace_back(
        "Alpha", Luna::Detail::MakeErasedCallableDescriptor(&AddIntegers));
    Group.emplace_back(
        "Zulu", Luna::Detail::MakeErasedCallableDescriptor(&AddIntegers));
    const CallbackBoundaryObservation Observed =
        Hooks::SubmitThroughCallback(Grouped, std::move(Group), 1, true, true);
    Check(Observed.CallbackThrew && Observed.ExceptionContained &&
              !Observed.Published,
          "a throwing callback is contained and never publishes");
    Check(Hooks::GenerationsOf(Grouped) == Generations &&
              Hooks::PendingBindingCount(Grouped) == 0 &&
              Hooks::ObserveRootStackDepth(Grouped) == EntryDepth,
          "a contained callback failure leaves the committed model untouched");
    Check(Grouped.Execute("assert(Alpha == nil and Zulu == nil)").IsSuccess(),
          "the real virtual machine sees nothing of the abandoned group");
    Check(
        Grouped.Execute("Kept = Add(2, 3)").IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Grouped, "Kept") == 5 &&
            Grouped.Bindings().Register("Alpha", &AddIntegers).IsSuccess() &&
            Grouped.Execute("Later = Alpha(4, 5)").IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Grouped, "Later") == 9,
        "the State registers and executes after a contained callback failure");
  }
}

void CheckLifecycleFamiliesPreserveTheState() {
  Luna::State Owner;
  AdderCalls = 0;
  int CounterCalls = 0;
  Check(Owner.Bindings().Register("Add", &AddIntegers).IsSuccess() &&
            Owner.Bindings()
                .Register("Counted",
                          [&CounterCalls](int Value) {
                            ++CounterCalls;
                            return Value + 1;
                          })
                .IsSuccess(),
        "the baseline declarations publish");

  const auto Baseline = Capture(Owner);
  Luna::RegistrationResult Foreign = Luna::RegistrationResult::Success();
  std::thread Other([&Owner, &Foreign] {
    Foreign = Owner.Bindings().Register("Foreign", &AddIntegers);
  });
  Other.join();
  Check(HasFailure(Foreign, Luna::ErrorCategory::StateNotReady, "owner thread"),
        "a foreign thread is rejected deterministically");
  CheckCommittedModelSurvived(Owner, Baseline, "Foreign", "absent",
                              CounterCalls);
  Check(Owner.Bindings().Register("Foreign", &AddIntegers).IsSuccess() &&
            Owner.Execute("ForeignResult = Foreign(2, 2)").IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "ForeignResult") == 4,
        "the owner thread still registers and executes afterwards");

  const auto Frozen = Capture(Owner);
  Check(Hooks::MarkFrozen(Owner), "the State can enter the frozen phase");
  const auto Rejected = Owner.Bindings().Register("Late", &AddIntegers);
  Check(HasFailure(Rejected, Luna::ErrorCategory::StateNotReady, "frozen"),
        "a frozen State rejects registration deterministically");
  CheckCommittedModelSurvived(Owner, Frozen, "Late", "absent", CounterCalls);
  Check(Owner.Execute("FrozenResult = Add(6, 6)").IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "FrozenResult") == 12,
        "a frozen State keeps executing its committed behavior");
}

void CheckRegistrationInsideALiveCallback() {
  Luna::State Owner;
  AdderCalls = 0;
  int CounterCalls = 0;
  Check(Owner.Bindings().Register("Add", &AddIntegers).IsSuccess() &&
            Owner.Bindings()
                .Register("Counted",
                          [&CounterCalls](int Value) {
                            ++CounterCalls;
                            return Value + 1;
                          })
                .IsSuccess(),
        "the baseline declarations publish");

  struct NestedAttempt final {
    bool Attempted = false;
    bool Succeeded = false;
    std::string Message;
    std::optional<int> DepthBefore;
    std::optional<int> DepthAfter;
  };

  NestedAttempt Nested;
  std::string NestedName = "FromCallback";
  bool InjectInstallationFault = false;

  Check(Owner.Bindings()
            .Register("Nest",
                      [&](int Value) {
                        Nested.DepthBefore =
                            Hooks::ObserveRootStackDepth(Owner);
                        if (InjectInstallationFault)
                          Hooks::InjectFault(Owner,
                                             FaultPoint::BindingInstallation);
                        const auto Registered =
                            Owner.Bindings().Register(NestedName, &AddIntegers);
                        Nested.Attempted = true;
                        Nested.Succeeded = Registered.IsSuccess();
                        if (Registered.Diagnostic())
                          Nested.Message = Registered.Diagnostic()->Message();
                        Nested.DepthAfter = Hooks::ObserveRootStackDepth(Owner);
                        return Value * 2;
                      })
            .IsSuccess(),
        "a callback that registers can itself be published");

  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  const auto Generations = Hooks::GenerationsOf(Owner);

  Check(Owner.Execute("Doubled = Nest(21)").IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "Doubled") == 42,
        "a callback that registers still returns its own value");
  Check(Nested.Attempted && Nested.Succeeded && Nested.Message.empty(),
        "a nested attempt publishes from inside a live callback");
  Check(Nested.DepthBefore.has_value() &&
            Nested.DepthBefore == Nested.DepthAfter,
        "a published attempt restores the exact callback stack depth");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "a published attempt leaves the root stack at its entry depth");
  Check(Hooks::GenerationsOf(Owner)->Generation() ==
                Generations->Generation() + 1 &&
            Hooks::BindingIsCommitted(Owner, "FromCallback") &&
            Hooks::InstalledBindingRecordAddress(Owner, "FromCallback") ==
                Hooks::BindingRecordAddress(Owner, "FromCallback") &&
            PathKind(Owner, "FromCallback") == "function",
        "reflection, the virtual machine, and dispatch agree afterwards");
  Check(Owner.Execute("Nested = FromCallback(20, 22)").IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "Nested") == 42,
        "the declaration published from a callback runs in a later script");

  const auto Baseline = Capture(Owner);
  Nested = NestedAttempt{};
  NestedName = "FailedFromCallback";
  InjectInstallationFault = true;
  Check(Owner.Execute("Attempted = Nest(3)").IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "Attempted") == 6,
        "a callback whose nested attempt fails still returns its own value");
  Check(Nested.Attempted && !Nested.Succeeded &&
            Nested.Message.find("installation failed") != std::string::npos,
        "a nested attempt fails deterministically inside a live callback");
  Check(Nested.DepthBefore.has_value() &&
            Nested.DepthBefore == Nested.DepthAfter,
        "a failed attempt restores the exact callback stack depth");
  Check(Hooks::PendingFaults(Owner, FaultPoint::BindingInstallation) == 0,
        "the injected fault is consumed exactly once");
  InjectInstallationFault = false;
  CheckCommittedModelSurvived(Owner, Baseline, "FailedFromCallback", "absent",
                              CounterCalls);

  Nested = NestedAttempt{};
  Check(Owner.Execute("local Ok, Message = pcall(Nest, 'text')\n"
                      "assert(not Ok and type(Message) == 'string')\n"
                      "Caught = 1")
                .IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "Caught") == 1,
        "a native failure inside a callback is catchable from the script");
  Check(!Nested.Attempted,
        "a callback that fails validation never reaches its nested attempt");
  const auto Restoration = Hooks::ObserveLastCallbackStackRestoration(Owner);
  Check(Restoration.has_value() &&
            Restoration->RestoredDepth == Restoration->EntryDepth &&
            Restoration->ErrorDepth == Restoration->EntryDepth + 1,
        "a native failure restores the callback checkpoint exactly");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth &&
            Hooks::GenerationsOf(Owner) == Baseline.Generations &&
            Hooks::PendingBindingCount(Owner) == 0,
        "a caught native failure publishes nothing and stages nothing");
  Check(
      Owner.Bindings().Register("AfterCallback", &AddIntegers).IsSuccess() &&
          Owner.Execute("Later = AfterCallback(1, 2) + Nest(4)").IsSuccess() &&
          Hooks::ObserveIntegerGlobal(Owner, "Later") == 11,
      "the State registers and executes after every callback outcome");
}

} // namespace

int RunUnifiedTransactionIntegrationTests() {
  FailureCount = 0;
  CheckSuccessPathThroughTheRealVirtualMachine();
  CheckEveryFailureFamilyPreservesTheState();
  CheckRegistrationInsideALiveCallback();
  CheckLifecycleFamiliesPreserveTheState();
  return FailureCount == 0 ? 0 : 1;
}
