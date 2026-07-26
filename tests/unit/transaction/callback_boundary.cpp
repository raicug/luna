// Focused coverage of the two remaining boundaries of one registration
// transaction: the private callback boundary that contains everything a nested
// builder, module, or registration callback throws, and the isolation of every
// ordinary query taken from outside an attempt that is still in flight.
//
// It also covers the protected-resource allocation fault together with the undo
// journal, so an attempt that fails before installation stages nothing,
// journals nothing, and leaves every committed callable exactly as it was.

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
#include "state/transaction/transaction.hpp"

#include <cstddef>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace {

using Luna::Detail::CallbackBoundaryObservation;
using Luna::Detail::JoinedFunctionDeclaration;
using Luna::Detail::JoinedSubmissionReport;
using Luna::Detail::TransactionStatus;
using FaultPoint = Luna::Detail::StateFaultPoint;
using Hooks = Luna::Detail::StateTestHooks;

constexpr std::size_t NeverThrows = std::numeric_limits<std::size_t>::max();

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "callback boundary check failed: " << Description << '\n';
}

[[nodiscard]] int AddIntegers(int Left, int Right) { return Left + Right; }

[[nodiscard]] Luna::ErasedCallableDescriptor IntegerAdder() {
  return Luna::Detail::MakeErasedCallableDescriptor(&AddIntegers);
}

[[nodiscard]] std::vector<JoinedFunctionDeclaration>
Declarations(const std::vector<std::string> &Names) {
  std::vector<JoinedFunctionDeclaration> Group;
  Group.reserve(Names.size());
  for (const std::string &Name : Names)
    Group.emplace_back(Name, IntegerAdder());
  return Group;
}

[[nodiscard]] bool HasFailure(const Luna::RegistrationResult &Result,
                              Luna::ErrorCategory Category,
                              std::string_view Fragment) {
  return !Result.IsSuccess() && Result.Diagnostic() != nullptr &&
         Result.Diagnostic()->Category() == Category &&
         !Result.Diagnostic()->Message().empty() &&
         Result.Diagnostic()->Message().find(Fragment) != std::string::npos;
}

[[nodiscard]] bool
HasDiagnostic(const std::optional<Luna::ErrorDiagnostic> &Value,
              Luna::ErrorCategory Category, std::string_view Fragment) {
  return Value.has_value() && Value->Category() == Category &&
         !Value->Message().empty() &&
         Value->Message().find(Fragment) != std::string::npos;
}

[[nodiscard]] std::string PathKind(Luna::State &Owner,
                                   const std::string &Path) {
  return Hooks::ObserveVmPathValueKind(Owner, Path).value_or("<unavailable>");
}

[[nodiscard]] bool AllKindsAre(const std::vector<std::string> &Kinds,
                               std::string_view Expected, std::size_t Count) {
  if (Kinds.size() != Count)
    return false;
  for (const std::string &Kind : Kinds) {
    if (Kind != Expected)
      return false;
  }
  return true;
}

// Requirements 4.7, 4.8, 19.8: a callback that throws is contained at the
// private boundary, its attempt publishes nothing, and the State stays usable.
void CheckCallbackExceptionsAreContained() {
  const auto Attempt = [](std::size_t ThrowAfter, bool StandardException,
                          std::string_view Kind, std::string_view Fragment,
                          bool PublishWhenComplete) {
    Luna::State Owner;
    Check(Owner.Bindings().Register("Base", &AddIntegers).IsSuccess(),
          "the baseline declaration publishes");
    const auto Baseline = Hooks::GenerationsOf(Owner);
    const auto BaselineRecord = Hooks::BindingRecordAddress(Owner, "Base");
    const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

    const CallbackBoundaryObservation Observed = Hooks::SubmitThroughCallback(
        Owner, Declarations({"Zulu", "Alpha", "Mike"}), ThrowAfter,
        StandardException, PublishWhenComplete);

    Check(Observed.CallbackThrew && Observed.ExceptionContained &&
              Observed.ExceptionKind == Kind,
          "the private boundary contains what the callback threw");
    Check(HasDiagnostic(Observed.Failure, Luna::ErrorCategory::Internal,
                        Fragment),
          "a contained callback failure reports one deterministic diagnostic");
    Check(Observed.PlannedWhileOpen == ThrowAfter &&
              Observed.StagedWhileOpen == ThrowAfter &&
              Observed.PendingSymbolsInView == ThrowAfter,
          "only the declarations submitted before the throw joined the plan");
    Check(!Observed.CouldPublishWhileOpen,
          "a contained callback failure poisons the outer transaction");
    Check(!Observed.Published &&
              Observed.Status == TransactionStatus::RolledBack,
          "a poisoned attempt rolls back instead of publishing");
    Check(Observed.JournalledEntries == 0 && Observed.InstalledPaths == 0,
          "an attempt that fails before installation journals nothing");

    // Nothing the callback staged is observable, before or after.
    Check(Observed.GenerationWhileOpen == Baseline->Generation() &&
              Observed.GenerationSymbolsWhileOpen == Baseline->Symbols().Size(),
          "an ordinary generation query sees no pending symbol");
    Check(AllKindsAre(Observed.VmPathKindsWhileOpen, "absent", ThrowAfter) &&
              Observed.DispatchVisibleWhileOpen == 0,
          "no pending declaration reaches the virtual machine or dispatch");
    Check(Observed.StackDepthWhileOpen == Observed.EntryStackDepth,
          "a contained callback leaves the entry stack depth untouched");
    Check(Observed.GenerationAfter == Baseline->Generation() &&
              Observed.StagedAfter == 0 && Observed.CommittedAfter == 1,
          "a contained callback failure discards every staged resource");
    Check(AllKindsAre(Observed.VmPathKindsAfter, "absent", ThrowAfter) &&
              Observed.DispatchVisibleAfter == 0,
          "a contained callback failure installs no virtual-machine value");
    Check(Observed.StackDepthAfter == Observed.EntryStackDepth &&
              Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
          "a contained callback failure restores the exact entry stack depth");
    Check(Hooks::GenerationsOf(Owner) == Baseline &&
              !Hooks::HasActiveTransaction(Owner),
          "a contained callback failure leaves no trace on the State");

    // The committed callable is the same object it was, and still invocable.
    Check(Hooks::BindingRecordAddress(Owner, "Base") == BaselineRecord &&
              Hooks::InstalledBindingRecordAddress(Owner, "Base") ==
                  BaselineRecord,
          "the original callable is preserved in the model and the machine");
    Check(Owner.Execute("Kept = Base(20, 22)").IsSuccess() &&
              Hooks::ObserveIntegerGlobal(Owner, "Kept") == 42,
          "the original callable keeps its behavior");

    // The State is reusable, including for the names the callback abandoned.
    Check(Owner.Bindings().Register("Zulu", &AddIntegers).IsSuccess() &&
              Owner.Execute("After = Zulu(1, 2)").IsSuccess() &&
              Hooks::ObserveIntegerGlobal(Owner, "After") == 3,
          "the State stays reusable after a contained callback failure");
  };

  // A standard exception and an unknown one, thrown partway through the group.
  Attempt(2, true, "standard", "registration callback failed", false);
  Attempt(2, false, "unknown", "unknown reason", false);

  // A throw before the first declaration, and a throw after every declaration
  // has staged its protected resource and the attempt was asked to publish.
  Attempt(0, true, "standard", "registration callback failed", false);
  Attempt(3, true, "standard", "registration callback failed", true);
}

// Requirements 4.5, 4.7, 19.8: an allocation fault fails before anything is
// installed, so the journal has nothing to restore and nothing is staged.
void CheckAllocationFaultsStageAndJournalNothing() {
  Luna::State Owner;
  Check(Owner.Bindings().Register("Base", &AddIntegers).IsSuccess(),
        "the baseline declaration publishes");
  const auto Baseline = Hooks::GenerationsOf(Owner);
  const auto BaselineRecord = Hooks::BindingRecordAddress(Owner, "Base");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Hooks::InjectFault(Owner, FaultPoint::BindingRecordAllocation);
  const auto Failed = Owner.Bindings().Register("Candidate", &AddIntegers);
  Check(HasFailure(Failed, Luna::ErrorCategory::Internal,
                   "Could not allocate binding record for global 'Candidate'"),
        "an allocation fault names the declaration it could not stage");
  Check(Hooks::PendingFaults(Owner, FaultPoint::BindingRecordAllocation) == 0,
        "the injected allocation fault is consumed exactly once");
  Check(Hooks::GenerationsOf(Owner) == Baseline &&
            Hooks::BindingCount(Owner) == 1 &&
            Hooks::PendingBindingCount(Owner) == 0,
        "an allocation fault publishes nothing and stages nothing");
  Check(PathKind(Owner, "Candidate") == "absent",
        "an allocation fault installs no virtual-machine value");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth &&
            !Hooks::HasActiveTransaction(Owner),
        "an allocation fault restores the entry stack depth and closes");
  Check(Hooks::BindingRecordAddress(Owner, "Base") == BaselineRecord &&
            Hooks::InstalledBindingRecordAddress(Owner, "Base") ==
                BaselineRecord,
        "an allocation fault preserves the committed callable");

  // The same fault inside a group taken all the way through publication: the
  // ignored nested failure poisons the attempt, so the journal is never opened.
  Hooks::InjectFault(Owner, FaultPoint::BindingRecordAllocation);
  const JoinedSubmissionReport Report = Hooks::PublishJoinedFunctions(
      Owner, Declarations({"Zulu", "Alpha", "Mike"}), true);
  Check(Report.Planned == 3 && Report.Prepared == 2 &&
            Report.NestedFailures == 1,
        "the declaration whose allocation failed stages no resource");
  Check(HasDiagnostic(Report.Failure, Luna::ErrorCategory::Internal,
                      "Could not allocate binding record for global 'Zulu'"),
        "the group keeps the first deterministic allocation diagnostic");
  Check(!Report.OuterCouldPublish && !Report.Publication.IsPublished &&
            Report.Status == TransactionStatus::RolledBack,
        "an allocation fault inside a group prevents publication");
  Check(Report.Publication.JournalledEntries == 0 &&
            Report.Publication.InstalledPaths == 0,
        "a group that fails before installation journals nothing");
  Check(Report.StagedBindingsAfter == 0 && Report.CommittedBindingsAfter == 1 &&
            Report.PublishedGenerationAfter == Baseline->Generation(),
        "a group that fails before installation discards every resource");
  Check(PathKind(Owner, "Zulu") == "absent" &&
            PathKind(Owner, "Alpha") == "absent" &&
            PathKind(Owner, "Mike") == "absent",
        "no declaration of the failed group reaches the virtual machine");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "a failed group leaves the root stack at its entry depth");

  // Everything still works: the same group publishes, and the committed
  // callable of the baseline is untouched.
  const JoinedSubmissionReport Retried = Hooks::PublishJoinedFunctions(
      Owner, Declarations({"Zulu", "Alpha", "Mike"}), false);
  Check(Retried.Publication.IsPublished && Retried.CommittedBindingsAfter == 4,
        "the group publishes once the allocation fault is gone");
  Check(Owner.Execute("Total = Base(1, 1) + Alpha(2, 3) + Zulu(4, 5) + "
                      "Mike(6, 7)")
                .IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "Total") == 29,
        "every published declaration and the baseline are invocable");
}

// Requirements 3.2, 4.4, 4.7: every ordinary query taken from outside an
// in-flight attempt observes only the committed model.
void CheckOrdinaryQueriesStayIsolated() {
  Luna::State Owner;
  Check(Owner.Bindings().Register("Base", &AddIntegers).IsSuccess(),
        "the baseline declaration publishes");
  Check(Owner.Execute("Alpha = 'script'").IsSuccess(),
        "a script-created value can sit beside the attempt");

  const auto Baseline = Hooks::GenerationsOf(Owner);
  auto *Database = Hooks::ReflectionDatabaseOf(Owner);
  Check(Database != nullptr, "a State owns one reflection database");
  if (!Database || !Baseline)
    return;
  const Luna::ReflectionSnapshot Before = Database->Snapshot();
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  // An attempt that stages three declarations and then rolls back.
  const CallbackBoundaryObservation Open = Hooks::SubmitThroughCallback(
      Owner, Declarations({"Zulu", "Charlie", "Mike"}), NeverThrows, true,
      false);
  Check(!Open.CallbackThrew && Open.PlannedWhileOpen == 3 &&
            Open.StagedWhileOpen == 3 && Open.CouldPublishWhileOpen,
        "a callback that returns normally stages every declaration");
  Check(Open.GenerationWhileOpen == Baseline->Generation() &&
            Open.GenerationSymbolsWhileOpen == Baseline->Symbols().Size(),
        "the committed generation set never describes a pending symbol");
  Check(
      Open.SnapshotGenerationWhileOpen == Before.Generation() &&
          Open.SnapshotSymbolsWhileOpen == Before.Size(),
      "an owning reflection snapshot taken while open sees no pending record");
  Check(Open.ForeignSnapshotGenerationWhileOpen ==
                Open.SnapshotGenerationWhileOpen &&
            Open.ForeignSnapshotSymbolsWhileOpen ==
                Open.SnapshotSymbolsWhileOpen,
        "another thread taking the same query sees the same committed model");
  Check(AllKindsAre(Open.VmPathKindsWhileOpen, "absent", 3) &&
            Open.DispatchVisibleWhileOpen == 0,
        "no pending declaration is installed or dispatchable while open");
  Check(Open.CommittedWhileOpen == 1 &&
            Open.StackDepthWhileOpen == Open.EntryStackDepth,
        "an in-flight attempt commits nothing and disturbs no stack depth");
  Check(Open.Status == TransactionStatus::RolledBack &&
            Open.GenerationAfter == Baseline->Generation() &&
            Open.StagedAfter == 0 && Open.CommittedAfter == 1,
        "an attempt that does not publish discards every staged resource");
  Check(Hooks::GenerationsOf(Owner) == Baseline &&
            Hooks::ReflectionGeneration(Owner) == Before.Generation(),
        "a discarded attempt leaves the committed model untouched");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "a discarded attempt leaves the root stack at its entry depth");

  // The same isolation across a successful publication: the snapshot taken
  // while the attempt was open keeps observing its own generation afterwards.
  const CallbackBoundaryObservation Published = Hooks::SubmitThroughCallback(
      Owner, Declarations({"Zulu", "Charlie"}), NeverThrows, true, true);
  Check(Published.Published && Published.Status == TransactionStatus::Committed,
        "a callback that returns normally publishes its group");
  Check(Published.SnapshotGenerationWhileOpen == Before.Generation() &&
            Published.SnapshotSymbolsWhileOpen == Before.Size(),
        "the query taken before publication saw only the committed model");
  Check(Published.SnapshotWhileOpen.Generation() == Before.Generation() &&
            Published.SnapshotWhileOpen.Size() == Before.Size(),
        "the retained snapshot is unchanged by the publication that followed");
  Check(Published.GenerationAfter == Baseline->Generation() + 1 &&
            Published.GenerationSymbolsAfter ==
                Baseline->Symbols().Size() + 2 &&
            Published.StagedAfter == 0 && Published.CommittedAfter == 3,
        "publication makes the whole group visible at once");
  Check(AllKindsAre(Published.VmPathKindsAfter, "function", 2) &&
            Published.DispatchVisibleAfter == 2,
        "publication installs every declaration for the virtual machine");
  Check(Published.StackDepthAfter == Published.EntryStackDepth &&
            Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "publication leaves the root stack at its entry depth");
  Check(Owner.Execute("Sum = Zulu(1, 2) + Charlie(3, 4)").IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "Sum") == 10,
        "every published declaration is invocable");
  Check(Owner.Execute("assert(Alpha == 'script')").IsSuccess(),
        "an untouched script-created path keeps its value");
}

} // namespace

int RunTransactionCallbackBoundaryTests() {
  FailureCount = 0;
  CheckCallbackExceptionsAreContained();
  CheckAllocationFaultsStageAndJournalNothing();
  CheckOrdinaryQueriesStayIsolated();
  return FailureCount == 0 ? 0 : 1;
}
