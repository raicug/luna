// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/callable_descriptor.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/detail/callable_adapter.hpp>
#include <luna/state/state.hpp>

#include "state/registration/submission.hpp"
#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/transaction/installation.hpp"
#include "state/transaction/transaction.hpp"

#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Luna::Detail::ConsistencyStatus;
using Luna::Detail::InstallationScope;
using Luna::Detail::InstallationStatus;
using Luna::Detail::JoinedFunctionDeclaration;
using Luna::Detail::JoinedSubmissionReport;
using Luna::Detail::PublicationObservation;
using Luna::Detail::TransactionStatus;
using FaultPoint = Luna::Detail::StateFaultPoint;
using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "installation and publication check failed: " << Description
            << '\n';
}

[[nodiscard]] int AddIntegers(int Left, int Right) { return Left + Right; }

[[nodiscard]] Luna::ErasedCallableDescriptor IntegerAdder() {
  return Luna::Detail::MakeErasedCallableDescriptor(&AddIntegers);
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

[[nodiscard]] std::vector<JoinedFunctionDeclaration>
Declarations(const std::vector<std::string> &Names) {
  std::vector<JoinedFunctionDeclaration> Group;
  Group.reserve(Names.size());
  for (const std::string &Name : Names)
    Group.emplace_back(Name, IntegerAdder());
  return Group;
}

void CheckSuccessfulPublicationIsAtomic() {
  Luna::State Owner;
  const auto Entry = Hooks::GenerationsOf(Owner);
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  Check(Entry != nullptr && Entry->Generation() == 0 && EntryDepth.has_value(),
        "a fresh State starts on the initial generation set");

  const auto Registered = Owner.Bindings().Register("Add", &AddIntegers);
  Check(Registered.IsSuccess(), "a complete declaration publishes");

  const auto Published = Hooks::GenerationsOf(Owner);
  Check(Published != Entry && Published->Generation() == 1,
        "publication advances the committed generation set exactly once");
  Check(Published->Symbols().Size() == 1 &&
            Published->Symbols().Contains("Add"),
        "the published generation describes the installed symbol");
  Check(Hooks::BindingIsCommitted(Owner, "Add") &&
            Hooks::PendingBindingCount(Owner) == 0,
        "publication commits the staged binding overlay");
  Check(Hooks::InstalledBindingRecordAddress(Owner, "Add") ==
            Hooks::BindingRecordAddress(Owner, "Add"),
        "the virtual machine and the canonical model describe one callable");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "publication leaves the root stack at its entry depth");
  Check(PathKind(Owner, "Add") == "function",
        "the published path holds the installed closure");

  const auto Execution = Owner.Execute("Observed = Add(20, 22)");
  Check(Execution.IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "Observed") == 42,
        "a published binding is invocable through the real virtual machine");

  Check(Owner.Bindings().Register("Second", &AddIntegers).IsSuccess() &&
            Hooks::GenerationsOf(Owner)->Generation() == 2 &&
            Hooks::GenerationsOf(Owner)->Symbols().Size() == 2,
        "each successful attempt publishes exactly one generation");

  Check(Hooks::ReflectionGeneration(Owner) == 2,
        "publication advances the reflection generation of every callable it "
        "publishes");
}

void CheckUndoRestoresTheExactPriorValue() {
  Luna::State Owner;
  const auto Seed = Owner.Execute("Slot = 7\nText = 'kept'");
  Check(Seed.IsSuccess() && PathKind(Owner, "Slot") == "number" &&
            PathKind(Owner, "Text") == "string",
        "the script-created paths hold their own values");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Hooks::InjectFault(Owner, FaultPoint::BindingInstallation);
  const auto Failed = Owner.Bindings().Register("Slot", &AddIntegers);
  Check(HasFailure(Failed, Luna::ErrorCategory::Internal, "Slot") &&
            HasFailure(Failed, Luna::ErrorCategory::Internal,
                       "installation failed") &&
            HasFailure(Failed, Luna::ErrorCategory::Internal, "rolled back"),
        "an installation failure reports the attempted global");
  Check(PathKind(Owner, "Slot") == "number" &&
            Hooks::ObserveIntegerGlobal(Owner, "Slot") == 7,
        "restoration puts back the exact prior value rather than clearing it");
  Check(Hooks::GenerationsOf(Owner)->Generation() == 0 &&
            Hooks::BindingCount(Owner) == 0 &&
            Hooks::PendingBindingCount(Owner) == 0,
        "a failed installation publishes and stages nothing");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "a failed installation leaves the root stack at its entry depth");

  Hooks::InjectFault(Owner, FaultPoint::BindingInstallation);
  const auto Absent = Owner.Bindings().Register("Fresh", &AddIntegers);
  Check(!Absent.IsSuccess() && PathKind(Owner, "Fresh") == "absent",
        "restoration reproduces the absence of a path the attempt created");
  Check(Owner.Execute("assert(Fresh == nil)").IsSuccess(),
        "the virtual machine agrees the created path is gone");

  Check(Owner.Bindings().Register("Slot", &AddIntegers).IsSuccess() &&
            PathKind(Owner, "Slot") == "function" &&
            Owner.Execute("Sum = Slot(2, 3)").IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "Sum") == 5,
        "a restored path can still be registered over afterwards");
  Check(PathKind(Owner, "Text") == "string",
        "an untouched path is never journalled or disturbed");
}

void CheckJoinedGroupPublishesAllOrNone() {
  Luna::State Owner;
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  const JoinedSubmissionReport Published = Hooks::PublishJoinedFunctions(
      Owner, Declarations({"Zulu", "Alpha", "Mike"}), false);
  Check(Published.Status == TransactionStatus::Committed &&
            Published.Publication.IsPublished,
        "a complete group publishes as one unit");
  Check(Published.Publication.PublishedGeneration == 1 &&
            Published.PublishedGenerationAfter == 1 &&
            Published.PublishedSymbolsAfter == 3,
        "three declarations publish one generation describing all of them");
  Check(Published.Publication.JournalledPaths == 3 &&
            Published.Publication.InstalledPaths == 3 &&
            Published.Publication.RestorationOrder.empty(),
        "a published group journals every path and restores none");
  Check(Published.CommittedBindingsAfter == 3 &&
            Published.StagedBindingsAfter == 0 &&
            Published.VmVisibleDeclarationsAfter == 3,
        "every declaration of a published group is committed and installed");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth &&
            Published.Publication.StackDepthAfter == *EntryDepth,
        "publication leaves the root stack at its entry depth");
  Check(Owner.Execute("Total = Alpha(1, 2) + Mike(3, 4) + Zulu(5, 6)")
                .IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "Total") == 21,
        "every published declaration is invocable");

  Luna::State Restored;
  Check(Restored.Execute("Alpha = 1\nZulu = 'text'").IsSuccess(),
        "the group's paths can hold script-created values");
  const auto RestoredDepth = Hooks::ObserveRootStackDepth(Restored);
  Hooks::InjectFault(Restored, FaultPoint::TransactionConsistency);

  const JoinedSubmissionReport Rejected = Hooks::PublishJoinedFunctions(
      Restored, Declarations({"Zulu", "Alpha", "Mike"}), false);
  Check(Rejected.Publication.Consistency ==
                ConsistencyStatus::InjectedContradiction &&
            !Rejected.Publication.IsPublished,
        "a contradiction before publication rejects the attempt");
  Check(HasDiagnostic(Rejected.Failure, Luna::ErrorCategory::Internal,
                      "internal metadata contradicted"),
        "a rejected attempt reports a non-empty internal diagnostic");
  Check(Rejected.Publication.RestoredEveryEntry &&
            Rejected.Publication.RestoredEntryStackDepth,
        "restoration undoes every journalled entry");

  const std::vector<std::string> ExpectedOrder{"Zulu", "Zulu",  "Mike",
                                               "Mike", "Alpha", "Alpha"};
  Check(Rejected.Publication.RestorationOrder == ExpectedOrder,
        "restoration visits journalled entries in reverse order");
  Check(Rejected.Publication.PriorValueKinds ==
            std::vector<std::string>{"number", "absent", "string"},
        "the journal records the exact prior kind of every touched path");

  Check(PathKind(Restored, "Alpha") == "number" &&
            Hooks::ObserveIntegerGlobal(Restored, "Alpha") == 1,
        "a restored path holds its exact prior value");
  Check(PathKind(Restored, "Mike") == "absent",
        "a restored path that was absent is absent again");
  Check(PathKind(Restored, "Zulu") == "string",
        "a restored path keeps its prior string value");
  Check(Hooks::GenerationsOf(Restored)->Generation() == 0 &&
            Rejected.PublishedSymbolsAfter == 0,
        "a rejected group publishes no generation");
  Check(Rejected.StagedBindingsAfter == 0 &&
            Rejected.CommittedBindingsAfter == 0,
        "a rejected group discards every staged overlay");
  Check(Hooks::ObserveRootStackDepth(Restored) == RestoredDepth,
        "a rejected group leaves the root stack at its entry depth");
  Check(Restored.Execute("assert(Alpha == 1 and Zulu == 'text')").IsSuccess(),
        "the virtual machine keeps every committed behavior");

  const JoinedSubmissionReport Retried = Hooks::PublishJoinedFunctions(
      Restored, Declarations({"Zulu", "Alpha", "Mike"}), false);
  Check(Retried.Publication.IsPublished &&
            Hooks::GenerationsOf(Restored)->Generation() == 1 &&
            Restored.Execute("Again = Alpha(4, 5)").IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Restored, "Again") == 9,
        "the State stays fully reusable after a rejected group");
}

void CheckInjectedFaultsPreserveTheCommittedModel() {
  const auto Attempt = [](FaultPoint Point, std::string_view Fragment,
                          std::string_view Description) {
    Luna::State Owner;
    Check(Owner.Bindings().Register("Committed", &AddIntegers).IsSuccess(),
          "the baseline declaration publishes");
    const auto Baseline = Hooks::GenerationsOf(Owner);
    const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

    Hooks::InjectFault(Owner, Point);
    const auto Failed = Owner.Bindings().Register("Candidate", &AddIntegers);
    Check(HasFailure(Failed, Luna::ErrorCategory::Internal, Fragment),
          Description);
    Check(Hooks::PendingFaults(Owner, Point) == 0,
          "the injected fault is consumed exactly once");
    Check(Hooks::GenerationsOf(Owner) == Baseline &&
              Hooks::BindingCount(Owner) == 1 &&
              Hooks::PendingBindingCount(Owner) == 0,
          "a failed attempt publishes nothing and stages nothing");
    Check(PathKind(Owner, "Candidate") == "absent",
          "a failed attempt leaves its path absent");
    Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
          "a failed attempt restores the exact entry stack depth");
    Check(!Hooks::HasActiveTransaction(Owner),
          "a failed attempt leaves no active transaction");

    const auto Reused = Owner.Bindings().Register("Candidate", &AddIntegers);
    Check(Reused.IsSuccess() &&
              Owner.Execute("Reused = Candidate(8, 9) + Committed(1, 1)")
                  .IsSuccess() &&
              Hooks::ObserveIntegerGlobal(Owner, "Reused") == 19,
          "the State stays reusable and keeps its committed behavior");
  };

  Attempt(FaultPoint::TransactionPublication, "preparation",
          "a publication-preparation fault fails as an internal failure");
  Attempt(FaultPoint::BindingPathJournal, "could not journal the prior value",
          "a journal fault fails before anything is installed");
  Attempt(FaultPoint::BindingInstallation, "installation failed",
          "an installation fault is rolled back");
  Attempt(FaultPoint::TransactionConsistency, "internal metadata contradicted",
          "a metadata contradiction is rejected before publication");

  Luna::State Owner;
  Hooks::InjectFault(Owner, FaultPoint::BindingInstallation);
  Hooks::InjectFault(Owner, FaultPoint::TransactionUndo);
  const auto Failed = Owner.Bindings().Register("Unrestored", &AddIntegers);
  Check(HasFailure(Failed, Luna::ErrorCategory::Internal,
                   "internal rollback failed"),
        "a failed restoration is reported deterministically");
  Check(Hooks::GenerationsOf(Owner)->Generation() == 0 &&
            Hooks::PendingBindingCount(Owner) == 0,
        "a failed restoration still publishes nothing and stages nothing");
}

void CheckJournalCoversEveryOverlayCategory() {
  Luna::State Owner;
  Check(Owner.Execute("Kept = 5").IsSuccess(),
        "the probe can start from a script-created value");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  const std::vector<InstallationScope> Overlays{
      InstallationScope::Binding,       InstallationScope::Type,
      InstallationScope::Reflection,    InstallationScope::Dispatch,
      InstallationScope::Module,        InstallationScope::Metatable,
      InstallationScope::IdentityCache, InstallationScope::LookupCache};

  const PublicationObservation Restored = Hooks::ProbeInstallationJournal(
      Owner, {"Kept", "Created"}, Overlays, true);
  Check(Restored.JournalledEntries == Overlays.size() + 2 &&
            Restored.JournalledPaths == 2 && Restored.InstalledPaths == 2,
        "the journal records every path and every overlay of an attempt");
  Check(Restored.PriorValueKinds ==
            std::vector<std::string>{"number", "absent"},
        "the journal records the exact prior kind of every path");
  Check(Restored.RestoredEveryEntry && Restored.RestoredEntryStackDepth,
        "restoration undoes every scope, including the ones without a store");
  Check(Restored.RestorationOrder.size() == Overlays.size() + 2 &&
            Restored.RestorationOrder.front() == "lookup_cache" &&
            Restored.RestorationOrder.back() == "Kept",
        "restoration visits every journalled entry in reverse order");
  Check(PathKind(Owner, "Kept") == "number" &&
            Hooks::ObserveIntegerGlobal(Owner, "Kept") == 5 &&
            PathKind(Owner, "Created") == "absent",
        "restoration puts every path back exactly as it was");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth &&
            Restored.StackDepthAfter == *EntryDepth,
        "restoration returns the root stack to its entry depth");

  const PublicationObservation Committed = Hooks::ProbeInstallationJournal(
      Owner, {"Kept", "Created"}, Overlays, false);
  Check(Committed.IsPublished && Committed.RestorationOrder.empty(),
        "a committed journal restores nothing");
  Check(Hooks::ObserveIntegerGlobal(Owner, "Kept") == 4242 &&
            Hooks::ObserveIntegerGlobal(Owner, "Created") == 4242,
        "a committed journal keeps every installed value");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "a committed journal leaves the root stack at its entry depth");
}

void CheckPoisonedGroupNeverPublishes() {
  Luna::State Owner;
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  const JoinedSubmissionReport Report = Hooks::PublishJoinedFunctions(
      Owner, Declarations({"Alpha", "Alpha", "Zulu"}), true);
  Check(Report.NestedFailures == 1 && !Report.OuterCouldPublish &&
            !Report.Publication.IsPublished,
        "an ignored nested failure prevents publication");
  Check(Report.Status == TransactionStatus::RolledBack,
        "a poisoned group rolls back");
  Check(HasDiagnostic(Report.Failure, Luna::ErrorCategory::DuplicateGlobalName,
                      "already registered"),
        "the group keeps the first deterministic diagnostic");
  Check(Report.PublishedGenerationAfter == 0 &&
            Report.PublishedSymbolsAfter == 0 &&
            Report.StagedBindingsAfter == 0 &&
            Report.CommittedBindingsAfter == 0,
        "a poisoned group publishes nothing and stages nothing");
  Check(PathKind(Owner, "Alpha") == "absent" &&
            PathKind(Owner, "Zulu") == "absent",
        "a poisoned group installs no virtual-machine value");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "a poisoned group leaves the root stack at its entry depth");
  Check(Owner.Bindings().Register("Alpha", &AddIntegers).IsSuccess() &&
            Owner.Execute("After = Alpha(7, 8)").IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "After") == 15,
        "the State stays reusable after a poisoned group");
}

} // namespace

int RunTransactionInstallationAndPublicationTests() {
  FailureCount = 0;
  CheckSuccessfulPublicationIsAtomic();
  CheckUndoRestoresTheExactPriorValue();
  CheckJoinedGroupPublishesAllOrNone();
  CheckInjectedFaultsPreserveTheCommittedModel();
  CheckJournalCoversEveryOverlayCategory();
  CheckPoisonedGroupNeverPublishes();
  return FailureCount == 0 ? 0 : 1;
}
