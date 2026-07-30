// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/callable_descriptor.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/detail/callable_adapter.hpp>
#include <luna/state/state.hpp>

#include "state/registration/submission.hpp"
#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/transaction/installation.hpp"
#include "state/transaction/preparation.hpp"
#include "state/transaction/transaction.hpp"

#include <rapidcheck.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using CallbackObservation = Luna::Detail::CallbackBoundaryObservation;
using ConsistencyStatus = Luna::Detail::ConsistencyStatus;
using FaultPoint = Luna::Detail::StateFaultPoint;
using Hooks = Luna::Detail::StateTestHooks;
using InstallationScope = Luna::Detail::InstallationScope;
using InstallationStatus = Luna::Detail::InstallationStatus;
using JoinedDeclaration = Luna::Detail::JoinedFunctionDeclaration;
using PreparationStatus = Luna::Detail::PreparationStatus;
using PublicationObservation = Luna::Detail::PublicationObservation;
using SubmissionReport = Luna::Detail::JoinedSubmissionReport;
using TransactionStatus = Luna::Detail::TransactionStatus;

class ByteCursor final {
public:
  explicit ByteCursor(std::span<const std::uint8_t> Bytes) noexcept
      : BytesValue(Bytes) {}

  [[nodiscard]] std::uint8_t Next() noexcept {
    const std::size_t Index = IndexValue++;
    if (BytesValue.empty())
      return static_cast<std::uint8_t>(Index * 31U + 11U);
    return BytesValue[Index % BytesValue.size()];
  }

  [[nodiscard]] std::size_t Pick(std::size_t Count) noexcept {
    return Count == 0 ? 0 : static_cast<std::size_t>(Next()) % Count;
  }

private:
  std::span<const std::uint8_t> BytesValue;
  std::size_t IndexValue = 0;
};

[[nodiscard]] int AddIntegers(int Left, int Right) { return Left + Right; }

[[nodiscard]] Luna::ErasedCallableDescriptor IntegerAdder() {
  return Luna::Detail::MakeErasedCallableDescriptor(&AddIntegers);
}

[[nodiscard]] Luna::ErasedCallableDescriptor NullAdder() {
  int (*Missing)(int, int) = nullptr;
  return Luna::Detail::MakeErasedCallableDescriptor(Missing);
}

constexpr std::array GroupNames{"alpha", "bravo", "charlie",
                                "delta", "echo",  "foxtrot"};
constexpr std::array CommittedNames{"kept_one", "kept_two"};
constexpr std::array ProbeNames{"probe_one", "probe_two"};

constexpr std::array OverlayScopes{
    InstallationScope::Binding,       InstallationScope::Type,
    InstallationScope::Reflection,    InstallationScope::Dispatch,
    InstallationScope::Module,        InstallationScope::Metatable,
    InstallationScope::IdentityCache, InstallationScope::LookupCache};

enum class FailureMode : std::size_t {
  None,
  DuplicateInGroup,
  DuplicateCommitted,
  InvalidName,
  NullTarget,
  NestedPreparation,
  NestedAllocation,
  CallbackThrow,
  PublicationPreparation,
  PathJournal,
  Installation,
  Consistency,
  Count
};

[[nodiscard]] constexpr bool RequiresPublication(FailureMode Mode) noexcept {
  return Mode == FailureMode::PublicationPreparation ||
         Mode == FailureMode::PathJournal ||
         Mode == FailureMode::Installation || Mode == FailureMode::Consistency;
}

[[nodiscard]] constexpr std::optional<FaultPoint>
InjectedFault(FailureMode Mode) noexcept {
  switch (Mode) {
  case FailureMode::NestedPreparation:
    return FaultPoint::TransactionPreparation;
  case FailureMode::NestedAllocation:
    return FaultPoint::BindingRecordAllocation;
  case FailureMode::PublicationPreparation:
    return FaultPoint::TransactionPublication;
  case FailureMode::PathJournal:
    return FaultPoint::BindingPathJournal;
  case FailureMode::Installation:
    return FaultPoint::BindingInstallation;
  case FailureMode::Consistency:
    return FaultPoint::TransactionConsistency;
  default:
    break;
  }
  return std::nullopt;
}

struct PlannedDeclaration final {
  std::string Name;
  bool NameIsValid = true;
  bool HasTarget = true;
};

struct PathPrior final {
  std::string Name;
  std::string Kind = "absent";
  int Number = 0;
  std::string Text;
};

[[nodiscard]] const PathPrior *FindPrior(const std::vector<PathPrior> &Priors,
                                         const std::string &Name) {
  const auto Match = std::find_if(
      Priors.begin(), Priors.end(),
      [&Name](const PathPrior &Prior) { return Prior.Name == Name; });
  return Match == Priors.end() ? nullptr : &*Match;
}

[[nodiscard]] std::string PriorKindOf(const std::vector<PathPrior> &Priors,
                                      const std::string &Name) {
  const PathPrior *Prior = FindPrior(Priors, Name);
  return Prior ? Prior->Kind : std::string("absent");
}

[[nodiscard]] std::string SeedScript(const std::vector<PathPrior> &Priors) {
  std::string Source;
  for (const PathPrior &Prior : Priors) {
    if (Prior.Kind == "number")
      Source += Prior.Name + " = " + std::to_string(Prior.Number) + "\n";
    else if (Prior.Kind == "string")
      Source += Prior.Name + " = '" + Prior.Text + "'\n";
  }
  return Source;
}

[[nodiscard]] std::string SumScript(const std::vector<std::string> &Names,
                                    const std::string &Target) {
  std::string Source = Target + " = 0";
  for (std::size_t Index = 0; Index < Names.size(); ++Index) {
    Source += "\n" + Target + " = " + Target + " + " + Names[Index] + "(" +
              std::to_string(Index + 1) + ", " + std::to_string(Index + 2) +
              ")";
  }
  return Source;
}

[[nodiscard]] int ExpectedSum(std::size_t Count) noexcept {
  int Total = 0;
  for (std::size_t Index = 0; Index < Count; ++Index)
    Total += static_cast<int>(2U * Index + 3U);
  return Total;
}

struct ExpectedAttempt final {
  std::size_t Submissions = 0;
  std::size_t Planned = 0;
  std::size_t Prepared = 0;
  std::size_t NestedFailures = 0;
  std::optional<Luna::ErrorCategory> FirstFailure;

  std::vector<std::string> CanonicalPaths;

  std::vector<std::string> SubmissionPaths;

  [[nodiscard]] bool CouldPublish() const noexcept {
    return NestedFailures == 0;
  }
};

[[nodiscard]] ExpectedAttempt
Simulate(const std::vector<PlannedDeclaration> &Group,
         const std::vector<std::string> &Committed, FailureMode Mode,
         bool IgnoreNestedFailures) {
  ExpectedAttempt Expected;
  bool PreparationFault = Mode == FailureMode::NestedPreparation;
  bool AllocationFault = Mode == FailureMode::NestedAllocation;
  std::vector<std::string> Pending;

  for (const PlannedDeclaration &Declaration : Group) {
    if (Expected.NestedFailures != 0 && !IgnoreNestedFailures)
      break;
    ++Expected.Submissions;

    std::optional<Luna::ErrorCategory> Failure;
    const bool Duplicate = std::find(Pending.begin(), Pending.end(),
                                     Declaration.Name) != Pending.end() ||
                           std::find(Committed.begin(), Committed.end(),
                                     Declaration.Name) != Committed.end();

    if (!Declaration.NameIsValid)
      Failure = Luna::ErrorCategory::InvalidGlobalName;
    else if (!Declaration.HasTarget)
      Failure = Luna::ErrorCategory::NullCallable;
    else if (Duplicate)
      Failure = Luna::ErrorCategory::DuplicateGlobalName;

    if (!Failure) {
      Pending.push_back(Declaration.Name);
      ++Expected.Planned;
      if (PreparationFault) {
        PreparationFault = false;
        Failure = Luna::ErrorCategory::Internal;
      } else if (AllocationFault) {
        AllocationFault = false;
        Failure = Luna::ErrorCategory::Internal;
      } else {
        ++Expected.Prepared;
      }
    }

    if (Failure) {
      ++Expected.NestedFailures;
      if (!Expected.FirstFailure)
        Expected.FirstFailure = *Failure;
    }
  }

  Expected.SubmissionPaths = Pending;
  Expected.CanonicalPaths = std::move(Pending);
  std::sort(Expected.CanonicalPaths.begin(), Expected.CanonicalPaths.end());
  return Expected;
}

struct ExpectedPublication final {
  bool Published = false;
  PreparationStatus Preparation = PreparationStatus::Prepared;
  InstallationStatus Installation = InstallationStatus::Installed;
  ConsistencyStatus Consistency = ConsistencyStatus::Consistent;
  std::size_t JournalledEntries = 0;
  std::size_t JournalledPaths = 0;
  std::size_t InstalledPaths = 0;
  std::vector<std::string> PriorValueKinds;
  std::vector<std::string> RestorationOrder;
  bool RestoredEverything = false;
  bool ObservesStackDepth = false;
};

[[nodiscard]] ExpectedPublication
ExpectedJournal(const ExpectedAttempt &Attempt,
                const std::vector<PathPrior> &Priors, FailureMode Mode,
                bool Publish) {
  ExpectedPublication Expected;
  if (!Publish || !Attempt.CouldPublish())
    return Expected;

  Expected.ObservesStackDepth = Mode != FailureMode::PublicationPreparation;

  if (Mode == FailureMode::PublicationPreparation) {
    Expected.Preparation = PreparationStatus::AllocationFailure;
    return Expected;
  }

  const std::vector<std::string> &Paths = Attempt.CanonicalPaths;

  if (Mode == FailureMode::PathJournal) {
    Expected.Installation = InstallationStatus::JournalFailure;
    Expected.JournalledEntries = 1;
    Expected.RestorationOrder = {Paths.front()};
    Expected.RestoredEverything = true;
    return Expected;
  }

  if (Mode == FailureMode::Installation) {
    Expected.Installation = InstallationStatus::ProtectedFailure;
    Expected.JournalledEntries = 2;
    Expected.JournalledPaths = 1;
    Expected.PriorValueKinds = {PriorKindOf(Priors, Paths.front())};
    Expected.RestorationOrder = {Paths.front(), Paths.front()};
    Expected.RestoredEverything = true;
    return Expected;
  }

  Expected.JournalledEntries = 2U * Paths.size();
  Expected.JournalledPaths = Paths.size();
  Expected.InstalledPaths = Paths.size();
  for (const std::string &Path : Paths)
    Expected.PriorValueKinds.push_back(PriorKindOf(Priors, Path));

  if (Mode == FailureMode::Consistency) {
    Expected.Consistency = ConsistencyStatus::InjectedContradiction;
    Expected.RestoredEverything = true;
    for (std::size_t Index = Paths.size(); Index > 0; --Index) {
      Expected.RestorationOrder.push_back(Paths[Index - 1]);
      Expected.RestorationOrder.push_back(Paths[Index - 1]);
    }
    return Expected;
  }

  Expected.Published = true;
  return Expected;
}

} // namespace

namespace {

[[nodiscard]] std::string InvalidIdentifier(std::size_t Choice) {
  switch (Choice) {
  case 0:
    return {};
  case 1:
    return "9invalid";
  default:
    return "invalid-name";
  }
}

void VerifyPathHoldsPrior(Luna::State &Owner, const PathPrior &Prior) {
  RC_ASSERT(Hooks::ObserveVmPathValueKind(Owner, Prior.Name) == Prior.Kind);
  if (Prior.Kind == "number")
    RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, Prior.Name) == Prior.Number);
}

void VerifyOverlayJournal(Luna::State &Owner, ByteCursor &Cursor) {
  std::vector<PathPrior> Priors;
  std::vector<std::string> Paths;
  const std::size_t PathCount = 1U + Cursor.Pick(ProbeNames.size());
  for (std::size_t Index = 0; Index < PathCount; ++Index) {
    PathPrior Prior;
    Prior.Name = ProbeNames[Index];
    if (Cursor.Pick(2) == 0) {
      Prior.Kind = "number";
      Prior.Number = 5 + static_cast<int>(Index);
    }
    Paths.push_back(Prior.Name);
    Priors.push_back(std::move(Prior));
  }

  const std::string Seed = SeedScript(Priors);
  if (!Seed.empty())
    RC_ASSERT(Owner.Execute(Seed).IsSuccess());

  std::vector<InstallationScope> Overlays;
  std::vector<std::string> OverlayKeys;
  for (const InstallationScope Scope : OverlayScopes) {
    if (Cursor.Pick(2) == 0)
      continue;
    Overlays.push_back(Scope);
    OverlayKeys.emplace_back(Luna::Detail::InstallationScopeText(Scope));
  }
  if (Overlays.empty()) {
    Overlays.push_back(InstallationScope::Reflection);
    OverlayKeys.emplace_back(
        Luna::Detail::InstallationScopeText(InstallationScope::Reflection));
  }

  const std::size_t StagedOverlays = static_cast<std::size_t>(
      std::count(Overlays.begin(), Overlays.end(), InstallationScope::Binding));
  const bool Restore = Cursor.Pick(2) == 0;
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  RC_ASSERT(EntryDepth.has_value());

  const PublicationObservation Observed =
      Hooks::ProbeInstallationJournal(Owner, Paths, Overlays, Restore);

  std::vector<std::string> ExpectedKinds;
  for (const PathPrior &Prior : Priors)
    ExpectedKinds.push_back(Prior.Kind);

  RC_ASSERT(Observed.JournalledEntries == Paths.size() + Overlays.size());
  RC_ASSERT(Observed.JournalledPaths == Paths.size());
  RC_ASSERT(Observed.JournalledOverlays == Overlays.size() - StagedOverlays);
  RC_ASSERT(Observed.InstalledPaths == Paths.size());
  RC_ASSERT(Observed.PriorValueKinds == ExpectedKinds);
  RC_ASSERT(Observed.StackDepthAfter == *EntryDepth);
  RC_ASSERT(Hooks::ObserveRootStackDepth(Owner) == EntryDepth);

  if (Restore) {
    std::vector<std::string> ExpectedOrder;
    for (std::size_t Index = OverlayKeys.size(); Index > 0; --Index)
      ExpectedOrder.push_back(OverlayKeys[Index - 1]);
    for (std::size_t Index = Paths.size(); Index > 0; --Index)
      ExpectedOrder.push_back(Paths[Index - 1]);

    RC_ASSERT(!Observed.IsPublished);
    RC_ASSERT(Observed.RestorationOrder == ExpectedOrder);
    RC_ASSERT(Observed.RestoredEveryEntry);
    RC_ASSERT(Observed.RestoredEntryStackDepth);
    for (const PathPrior &Prior : Priors)
      VerifyPathHoldsPrior(Owner, Prior);
    return;
  }

  RC_ASSERT(Observed.IsPublished);
  RC_ASSERT(Observed.RestorationOrder.empty());
  RC_ASSERT(!Observed.RestoredEveryEntry);
  for (const std::string &Path : Paths)
    RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, Path) == 4242);
}

void VerifyJoinedAttempt(const SubmissionReport &Report,
                         const ExpectedAttempt &Attempt,
                         const ExpectedPublication &Publication,
                         std::size_t SubmittedCount, std::size_t CommittedCount,
                         bool Publish, int SeedDepth) {
  RC_ASSERT(Report.Submitted == SubmittedCount);
  RC_ASSERT(Report.JoinedSubmissions == Attempt.Submissions);
  RC_ASSERT(Report.Planned == Attempt.Planned);
  RC_ASSERT(Report.Prepared == Attempt.Prepared);
  RC_ASSERT(Report.NestedFailures == Attempt.NestedFailures);
  RC_ASSERT(Report.OuterCouldPublish == Attempt.CouldPublish());

  std::optional<Luna::ErrorCategory> ExpectedCategory = Attempt.FirstFailure;
  if (!ExpectedCategory && Publish && !Publication.Published)
    ExpectedCategory = Luna::ErrorCategory::Internal;
  RC_ASSERT(Report.Failure.has_value() == ExpectedCategory.has_value());
  if (ExpectedCategory) {
    RC_ASSERT(Report.Failure->Category() == *ExpectedCategory);
    RC_ASSERT(!Report.Failure->Message().empty());
  }
  RC_ASSERT(Report.Status == (Publication.Published
                                  ? TransactionStatus::Committed
                                  : TransactionStatus::RolledBack));

  RC_ASSERT(Report.CommittedSymbolsInView == CommittedCount);
  RC_ASSERT(Report.PendingSymbolsInView == Attempt.Planned);
  RC_ASSERT(Report.PublishedGenerationWhileOpen == CommittedCount);
  RC_ASSERT(Report.PublishedSymbolsWhileOpen == CommittedCount);
  RC_ASSERT(Report.ReflectionGenerationWhileOpen == CommittedCount);
  RC_ASSERT(Report.StagedBindingsWhileOpen == Attempt.Prepared);
  RC_ASSERT(Report.VmVisibleDeclarationsWhileOpen == 0);
  RC_ASSERT(Report.EntryStackDepth == SeedDepth);
  RC_ASSERT(Report.StackDepthWhileOpen == SeedDepth);

  RC_ASSERT(Report.Preparation == PreparationStatus::Prepared);
  RC_ASSERT(Report.CandidateGeneration == CommittedCount + 1);
  RC_ASSERT(Report.CandidateSymbols == CommittedCount + Attempt.Planned);

  const PublicationObservation &Observed = Report.Publication;
  RC_ASSERT(Observed.IsPublished == Publication.Published);
  RC_ASSERT(Observed.Preparation == Publication.Preparation);
  RC_ASSERT(Observed.Installation == Publication.Installation);
  RC_ASSERT(Observed.Consistency == Publication.Consistency);
  RC_ASSERT(Observed.JournalledEntries == Publication.JournalledEntries);
  RC_ASSERT(Observed.JournalledPaths == Publication.JournalledPaths);
  RC_ASSERT(Observed.JournalledOverlays == 0);
  RC_ASSERT(Observed.InstalledPaths == Publication.InstalledPaths);
  RC_ASSERT(Observed.PriorValueKinds == Publication.PriorValueKinds);
  RC_ASSERT(Observed.RestorationOrder == Publication.RestorationOrder);
  RC_ASSERT(Observed.RestoredEveryEntry == Publication.RestoredEverything);
  RC_ASSERT(Observed.RestoredEntryStackDepth == Publication.RestoredEverything);
  RC_ASSERT(Observed.EntryStackDepth == (Publish ? SeedDepth : 0));
  if (Publication.ObservesStackDepth)
    RC_ASSERT(Observed.StackDepthAfter == SeedDepth);
  const bool ReflectionAdvances = Publication.Published && Attempt.Planned != 0;
  RC_ASSERT(Observed.ReflectionAdvanced == ReflectionAdvances);
  if (Publication.Published) {
    RC_ASSERT(Observed.PublishedGeneration == CommittedCount + 1);
    RC_ASSERT(Observed.PublishedSymbols == CommittedCount + Attempt.Planned);
    RC_ASSERT(Observed.PublishedReflectionGeneration ==
              CommittedCount + (ReflectionAdvances ? 1U : 0U));
  }

  RC_ASSERT(Report.PublishedGenerationAfter ==
            CommittedCount + (Publication.Published ? 1U : 0U));
  RC_ASSERT(Report.PublishedSymbolsAfter ==
            CommittedCount + (Publication.Published ? Attempt.Planned : 0U));
  RC_ASSERT(Report.StagedBindingsAfter == 0);
  RC_ASSERT(Report.CommittedBindingsAfter ==
            CommittedCount + (Publication.Published ? Attempt.Planned : 0U));
  RC_ASSERT(Report.VmVisibleDeclarationsAfter ==
            (Publication.Published ? Attempt.Planned : 0U));
}

void VerifyCallbackAttempt(const CallbackObservation &Observed,
                           const ExpectedAttempt &Attempt,
                           const std::vector<PathPrior> &Priors,
                           std::size_t SubmittedCount,
                           std::size_t CommittedCount, bool ThrowStandard,
                           int SeedDepth) {
  RC_ASSERT(Observed.Submitted == SubmittedCount);
  RC_ASSERT(Observed.CallbackThrew);
  RC_ASSERT(Observed.ExceptionContained);
  RC_ASSERT(Observed.ExceptionKind ==
            std::string(ThrowStandard ? "standard" : "unknown"));

  RC_ASSERT(Observed.PlannedWhileOpen == Attempt.Planned);
  RC_ASSERT(Observed.PendingSymbolsInView == Attempt.Planned);
  RC_ASSERT(Observed.NestedFailures == Attempt.NestedFailures);
  RC_ASSERT(!Observed.CouldPublishWhileOpen);

  RC_ASSERT(Observed.GenerationWhileOpen == CommittedCount);
  RC_ASSERT(Observed.GenerationSymbolsWhileOpen == CommittedCount);
  RC_ASSERT(Observed.SnapshotGenerationWhileOpen == CommittedCount);
  RC_ASSERT(Observed.SnapshotSymbolsWhileOpen == CommittedCount * 2);
  RC_ASSERT(Observed.ForeignSnapshotGenerationWhileOpen ==
            Observed.SnapshotGenerationWhileOpen);
  RC_ASSERT(Observed.ForeignSnapshotSymbolsWhileOpen ==
            Observed.SnapshotSymbolsWhileOpen);
  RC_ASSERT(Observed.StagedWhileOpen == Attempt.Prepared);
  RC_ASSERT(Observed.CommittedWhileOpen == CommittedCount);
  RC_ASSERT(Observed.DispatchVisibleWhileOpen == 0);
  RC_ASSERT(Observed.EntryStackDepth == SeedDepth);
  RC_ASSERT(Observed.StackDepthWhileOpen == SeedDepth);

  std::vector<std::string> ExpectedKinds;
  for (const std::string &Path : Attempt.SubmissionPaths)
    ExpectedKinds.push_back(PriorKindOf(Priors, Path));
  RC_ASSERT(Observed.VmPathKindsWhileOpen == ExpectedKinds);
  RC_ASSERT(Observed.VmPathKindsAfter == ExpectedKinds);

  RC_ASSERT(!Observed.Published);
  RC_ASSERT(Observed.Status == TransactionStatus::RolledBack);
  RC_ASSERT(Observed.Failure.has_value());
  RC_ASSERT(Observed.Failure->Category() == Luna::ErrorCategory::Internal);
  RC_ASSERT(!Observed.Failure->Message().empty());
  RC_ASSERT(Observed.JournalledEntries == 0);
  RC_ASSERT(Observed.InstalledPaths == 0);
  RC_ASSERT(!Observed.RestoredEveryEntry);
  RC_ASSERT(!Observed.RestoredEntryStackDepth);

  RC_ASSERT(Observed.GenerationAfter == CommittedCount);
  RC_ASSERT(Observed.GenerationSymbolsAfter == CommittedCount);
  RC_ASSERT(Observed.StagedAfter == 0);
  RC_ASSERT(Observed.CommittedAfter == CommittedCount);
  RC_ASSERT(Observed.DispatchVisibleAfter == 0);
  RC_ASSERT(Observed.StackDepthAfter == SeedDepth);
}

} // namespace

int RunGeneralizedTransactionRollbackProperties() {

  const bool Passed = rc::check(

      "Outermost registration transactions publish all changes or none",
      [](const std::vector<std::uint8_t> &PlanShape,
         const std::vector<std::uint8_t> &FailureShape) {
        ByteCursor Plan(PlanShape);
        ByteCursor Choice(FailureShape);

        const auto Mode = static_cast<FailureMode>(
            Choice.Pick(static_cast<std::size_t>(FailureMode::Count)));
        const bool IgnoreNestedFailures = Choice.Pick(2) == 0;

        const bool Publish = RequiresPublication(Mode) || Choice.Pick(2) == 0;
        const int SeedDepth = static_cast<int>(Plan.Pick(5));

        std::size_t CommittedCount = Plan.Pick(CommittedNames.size() + 1);
        if (Mode == FailureMode::DuplicateCommitted && CommittedCount == 0)
          CommittedCount = 1;

        std::vector<std::string> AvailableNames(GroupNames.begin(),
                                                GroupNames.end());
        std::vector<PlannedDeclaration> Group;
        std::vector<PathPrior> Priors;
        const std::size_t GroupSize = 1U + Plan.Pick(4);
        for (std::size_t Index = 0; Index < GroupSize; ++Index) {
          const std::size_t Position = Plan.Pick(AvailableNames.size());
          PlannedDeclaration Declaration;
          Declaration.Name = AvailableNames[Position];
          AvailableNames.erase(AvailableNames.begin() +
                               static_cast<std::ptrdiff_t>(Position));

          PathPrior Prior;
          Prior.Name = Declaration.Name;
          switch (Plan.Pick(3)) {
          case 1:
            Prior.Kind = "number";
            Prior.Number = 1 + static_cast<int>(Plan.Pick(90));
            break;
          case 2:
            Prior.Kind = "string";
            Prior.Text = "text_" + std::to_string(Index);
            break;
          default:
            break;
          }

          Group.push_back(std::move(Declaration));
          Priors.push_back(std::move(Prior));
        }

        if (Mode == FailureMode::DuplicateInGroup ||
            Mode == FailureMode::DuplicateCommitted ||
            Mode == FailureMode::InvalidName ||
            Mode == FailureMode::NullTarget) {
          PlannedDeclaration Failing;
          switch (Mode) {
          case FailureMode::DuplicateInGroup:
            Failing.Name = Group[Choice.Pick(Group.size())].Name;
            break;
          case FailureMode::DuplicateCommitted:
            Failing.Name = CommittedNames[Choice.Pick(CommittedCount)];
            break;
          case FailureMode::InvalidName:
            Failing.Name = InvalidIdentifier(Choice.Pick(3));
            Failing.NameIsValid = false;
            break;
          default:
            Failing.Name = "golf";
            Failing.HasTarget = false;
            break;
          }
          const std::size_t Position = Choice.Pick(Group.size() + 1);
          Group.insert(Group.begin() + static_cast<std::ptrdiff_t>(Position),
                       std::move(Failing));
        }

        Luna::State Owner;
        RC_ASSERT(Owner.IsReady());

        std::vector<std::string> Committed;
        for (std::size_t Index = 0; Index < CommittedCount; ++Index) {
          Committed.emplace_back(CommittedNames[Index]);
          RC_ASSERT(Owner.Bindings()
                        .Register(Committed.back(), &AddIntegers)
                        .IsSuccess());
        }

        const std::string CommittedCheck =
            SumScript(Committed, "committed_total");
        const int CommittedSum = ExpectedSum(Committed.size());
        RC_ASSERT(Owner.Execute(CommittedCheck).IsSuccess());
        RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "committed_total") ==
                  CommittedSum);

        std::vector<std::optional<std::uintptr_t>> CommittedRecords;
        for (const std::string &Name : Committed)
          CommittedRecords.push_back(Hooks::BindingRecordAddress(Owner, Name));

        const std::string Seed = SeedScript(Priors);
        if (!Seed.empty())
          RC_ASSERT(Owner.Execute(Seed).IsSuccess());
        for (const PathPrior &Prior : Priors)
          VerifyPathHoldsPrior(Owner, Prior);

        const auto EntryGenerations = Hooks::GenerationsOf(Owner);
        RC_ASSERT(EntryGenerations != nullptr);
        RC_ASSERT(EntryGenerations->Generation() == Committed.size());
        RC_ASSERT(Hooks::ReflectionGeneration(Owner) == Committed.size());
        RC_ASSERT(Hooks::SetRootStackDepth(Owner, SeedDepth));
        RC_ASSERT(Hooks::ObserveRootStackDepth(Owner) == SeedDepth);

        if (const auto Fault = InjectedFault(Mode))
          Hooks::InjectFault(Owner, *Fault);

        const bool UsedCallback = Mode == FailureMode::CallbackThrow;
        const std::size_t ThrowAfter =
            UsedCallback ? Choice.Pick(Group.size() + 1) : 0;
        const bool ThrowStandard = Choice.Pick(2) == 0;

        std::vector<PlannedDeclaration> Attempted = Group;
        if (UsedCallback) {
          Attempted.erase(Attempted.begin() +
                              static_cast<std::ptrdiff_t>(ThrowAfter),
                          Attempted.end());
        }

        std::vector<JoinedDeclaration> Declarations;
        Declarations.reserve(Group.size());
        for (const PlannedDeclaration &Declaration : Group) {
          Declarations.emplace_back(Declaration.Name, Declaration.HasTarget
                                                          ? IntegerAdder()
                                                          : NullAdder());
        }

        std::optional<SubmissionReport> JoinedReport;
        std::optional<CallbackObservation> CallbackReport;
        if (UsedCallback)
          CallbackReport =
              Hooks::SubmitThroughCallback(Owner, std::move(Declarations),
                                           ThrowAfter, ThrowStandard, Publish);
        else if (Publish)
          JoinedReport = Hooks::PublishJoinedFunctions(
              Owner, std::move(Declarations), IgnoreNestedFailures);
        else
          JoinedReport = Hooks::SubmitJoinedFunctions(
              Owner, std::move(Declarations), IgnoreNestedFailures);

        const ExpectedAttempt Attempt = Simulate(
            Attempted, Committed, UsedCallback ? FailureMode::None : Mode,
            UsedCallback || IgnoreNestedFailures);
        const ExpectedPublication Publication =
            ExpectedJournal(Attempt, Priors, Mode, Publish && !UsedCallback);

        if (CallbackReport) {
          VerifyCallbackAttempt(*CallbackReport, Attempt, Priors, Group.size(),
                                Committed.size(), ThrowStandard, SeedDepth);
        } else {
          VerifyJoinedAttempt(*JoinedReport, Attempt, Publication, Group.size(),
                              Committed.size(), Publish, SeedDepth);
        }

        const std::uint64_t ExpectedGeneration =
            Committed.size() + (Publication.Published ? 1U : 0U);
        const std::size_t ExpectedSymbols =
            Committed.size() + (Publication.Published ? Attempt.Planned : 0U);

        RC_ASSERT(!Hooks::HasActiveTransaction(Owner));
        RC_ASSERT(Hooks::PendingBindingCount(Owner) == 0);
        RC_ASSERT(Hooks::BindingCount(Owner) == ExpectedSymbols);
        const auto AfterGenerations = Hooks::GenerationsOf(Owner);
        RC_ASSERT(AfterGenerations->Generation() == ExpectedGeneration);
        RC_ASSERT(AfterGenerations->Symbols().Size() == ExpectedSymbols);
        RC_ASSERT(
            Hooks::ReflectionGeneration(Owner) ==
            Committed.size() +
                ((Publication.Published && Attempt.Planned != 0) ? 1U : 0U));
        RC_ASSERT(Hooks::ObserveRootStackDepth(Owner) == SeedDepth);
        if (const auto Fault = InjectedFault(Mode))
          RC_ASSERT(Hooks::PendingFaults(Owner, *Fault) == 0);

        if (!Publication.Published)
          RC_ASSERT(AfterGenerations == EntryGenerations);

        RC_ASSERT(Hooks::SetRootStackDepth(Owner, 0));

        RC_ASSERT(Owner.Execute(CommittedCheck).IsSuccess());
        RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "committed_total") ==
                  CommittedSum);
        for (std::size_t Index = 0; Index < Committed.size(); ++Index) {
          RC_ASSERT(Hooks::BindingIsCommitted(Owner, Committed[Index]));
          RC_ASSERT(Hooks::BindingRecordAddress(Owner, Committed[Index]) ==
                    CommittedRecords[Index]);
        }

        if (Publication.Published) {
          for (const std::string &Path : Attempt.CanonicalPaths) {
            RC_ASSERT(Hooks::ObserveVmPathValueKind(Owner, Path) ==
                      std::string("function"));
            RC_ASSERT(Hooks::BindingIsCommitted(Owner, Path));
            RC_ASSERT(Hooks::InstalledBindingRecordAddress(Owner, Path) ==
                      Hooks::BindingRecordAddress(Owner, Path));
          }
          RC_ASSERT(
              Owner.Execute(SumScript(Attempt.CanonicalPaths, "group_total"))
                  .IsSuccess());
          RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "group_total") ==
                    ExpectedSum(Attempt.CanonicalPaths.size()));
        } else {
          for (const PathPrior &Prior : Priors) {
            VerifyPathHoldsPrior(Owner, Prior);
            RC_ASSERT(!Hooks::BindingIsCommitted(Owner, Prior.Name));
          }
        }

        RC_ASSERT(
            Owner.Bindings().Register("reuse_after", &AddIntegers).IsSuccess());
        RC_ASSERT(Hooks::GenerationsOf(Owner)->Generation() ==
                  ExpectedGeneration + 1);
        RC_ASSERT(Owner.Execute("reuse_sum = reuse_after(11, 31)").IsSuccess());
        RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "reuse_sum") == 42);

        VerifyOverlayJournal(Owner, Choice);

        RC_ASSERT(Hooks::GenerationsOf(Owner)->Generation() ==
                  ExpectedGeneration + 1);
        RC_ASSERT(Hooks::PendingBindingCount(Owner) == 0);
        RC_ASSERT(Owner.Execute(CommittedCheck).IsSuccess());
        RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "committed_total") ==
                  CommittedSum);
      });

  return Passed ? 0 : 1;
}
