// clang-format off
#include "state/transaction/installation.hpp"

#include "state/reflection/database.hpp"
#include "state/registration/plan.hpp"
#include "state/registration/record.hpp"
#include "state/registration/store.hpp"
#include "state/testing/fault_injector.hpp"
#include "state/testing/fault_point.hpp"
#include "state/transaction/generation_set.hpp"
#include "state/transaction/preparation.hpp"
#include "state/transaction/transaction.hpp"
#include "state/type/type_generation.hpp"
#include "state/vm/closure_installer.hpp"
#include "state/vm/namespace_table.hpp"
#include "state/vm/owner.hpp"
#include "state/vm/saved_value.hpp"
#include "state/vm/value_table.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {

std::string_view InstallationScopeText(InstallationScope Scope) noexcept {
  switch (Scope) {
  case InstallationScope::VirtualMachinePath:
    return "virtual_machine_path";
  case InstallationScope::Binding:
    return "binding";
  case InstallationScope::Type:
    return "type";
  case InstallationScope::Reflection:
    return "reflection";
  case InstallationScope::Dispatch:
    return "dispatch";
  case InstallationScope::Module:
    return "module";
  case InstallationScope::Metatable:
    return "metatable";
  case InstallationScope::IdentityCache:
    return "identity_cache";
  case InstallationScope::LookupCache:
    return "lookup_cache";
  }
  return "unknown";
}

std::string_view InstallationStatusText(InstallationStatus Status) noexcept {
  switch (Status) {
  case InstallationStatus::Installed:
    return "installed";
  case InstallationStatus::MissingStagedResource:
    return "missing_staged_resource";
  case InstallationStatus::JournalFailure:
    return "journal_failure";
  case InstallationStatus::StackCapacityFailure:
    return "stack_capacity_failure";
  case InstallationStatus::ProtectedFailure:
    return "protected_failure";
  case InstallationStatus::RestorationFailure:
    return "restoration_failure";
  }
  return "unknown";
}

std::string_view ConsistencyStatusText(ConsistencyStatus Status) noexcept {
  switch (Status) {
  case ConsistencyStatus::Consistent:
    return "consistent";
  case ConsistencyStatus::MissingCandidate:
    return "missing_candidate_generation";
  case ConsistencyStatus::GenerationMismatch:
    return "candidate_generation_mismatch";
  case ConsistencyStatus::SymbolCountMismatch:
    return "candidate_symbol_count_mismatch";
  case ConsistencyStatus::MissingSymbol:
    return "candidate_symbol_missing";
  case ConsistencyStatus::IdentityMismatch:
    return "candidate_symbol_identity_mismatch";
  case ConsistencyStatus::CategoryMismatch:
    return "candidate_symbol_category_mismatch";
  case ConsistencyStatus::PathMismatch:
    return "candidate_symbol_path_mismatch";
  case ConsistencyStatus::MissingBinding:
    return "staged_binding_missing";
  case ConsistencyStatus::UninstalledBinding:
    return "installed_binding_mismatch";
  case ConsistencyStatus::ReflectionGenerationMismatch:
    return "reflection_generation_mismatch";
  case ConsistencyStatus::ReflectionContentMismatch:
    return "reflection_content_mismatch";
  case ConsistencyStatus::InjectedContradiction:
    return "injected_contradiction";
  }
  return "unknown";
}

InstallationJournal::InstallationJournal(VirtualMachineOwner &Machine,
                                         BindingStore &Bindings,
                                         FaultInjector &Faults,
                                         int EntryStackDepth) noexcept
    : Machine(&Machine), Bindings(&Bindings), Faults(&Faults),
      EntryDepth(EntryStackDepth) {}

InstallationJournal::~InstallationJournal() noexcept {
  if (!CommittedFlag && !UndoneFlag)
    Undo();
  Release();
}

bool InstallationJournal::JournalVirtualMachinePath(std::string Path) {
  JournalEntry Entry;
  Entry.Scope = InstallationScope::VirtualMachinePath;
  Entry.Path = std::move(Path);

  // Nothing is written before the exact prior value or absence is recorded. A
  // nested namespace path records its prior field value the same way a
  // root-scope global records its prior value.
  if (!Machine->CaptureVmPath(Entry.Path, Entry.Prior))
    return false;

  Entries.push_back(std::move(Entry));
  return true;
}

void InstallationJournal::MarkInstalled() noexcept {
  for (std::size_t Index = Entries.size(); Index > 0; --Index) {
    JournalEntry &Entry = Entries[Index - 1];
    if (Entry.Scope != InstallationScope::VirtualMachinePath)
      continue;
    Entry.IsInstalled = true;
    return;
  }
}

void InstallationJournal::JournalStagedBinding(std::string Path,
                                               BindingRecord *Record) {
  JournalEntry Entry;
  Entry.Scope = InstallationScope::Binding;
  Entry.Path = std::move(Path);
  Entry.Staged = Record;
  Entries.push_back(std::move(Entry));
}

void InstallationJournal::JournalOverlay(InstallationScope Scope,
                                         std::string Key) {
  JournalEntry Entry;
  Entry.Scope = Scope;
  Entry.Path = std::move(Key);
  Entries.push_back(std::move(Entry));
}

void InstallationJournal::Undo() noexcept {
  if (CommittedFlag || UndoneFlag)
    return;

  RestoredOrder.clear();
  RestoredOrder.reserve(Entries.size());

  // Reverse order: the newest effect of the attempt is undone first, so a path
  // touched twice ends up holding the value it held before the attempt started.
  for (std::size_t Index = Entries.size(); Index > 0; --Index) {
    JournalEntry &Entry = Entries[Index - 1];
    switch (Entry.Scope) {
    case InstallationScope::VirtualMachinePath: {
      const bool InjectFailure =
          Faults->Consume(StateFaultPoint::TransactionUndo);
      Entry.IsRestored =
          !InjectFailure && Machine->RestoreVmPath(Entry.Path, Entry.Prior);
      break;
    }
    case InstallationScope::Binding:
      // Discarding the staged overlay is what makes the pending record
      // unreachable; it was never committed, so nothing else has to change.
      Entry.IsRestored = Entry.Staged == nullptr ||
                         Bindings->Rollback(Entry.Path, Entry.Staged);
      break;
    default:
      // The committed store of this category arrives with a later milestone.
      // Its overlay was staged privately and is discarded with the attempt.
      Entry.IsRestored = true;
      break;
    }
    RestoredOrder.push_back(Entry.Path);
  }

  // The root stack returns to the exact depth the attempt captured at entry.
  StackDepthRestored =
      !Machine->IsReady() || Machine->SetStackDepth(EntryDepth);
  UndoneFlag = true;
  Release();
}

void InstallationJournal::Commit() noexcept {
  if (UndoneFlag)
    return;
  CommittedFlag = true;
  Release();
}

void InstallationJournal::Release() noexcept {
  for (JournalEntry &Entry : Entries)
    Machine->ReleaseSavedValue(Entry.Prior);
}

std::size_t
InstallationJournal::CountOf(InstallationScope Scope) const noexcept {
  std::size_t Result = 0;
  for (const JournalEntry &Entry : Entries) {
    if (Entry.Scope == Scope)
      ++Result;
  }
  return Result;
}

bool InstallationJournal::RestoredEveryEntry() const noexcept {
  if (!UndoneFlag)
    return false;
  for (const JournalEntry &Entry : Entries) {
    if (!Entry.IsRestored)
      return false;
  }
  return true;
}

void ObserveJournal(const InstallationJournal &Journal,
                    PublicationObservation &Observed) {
  Observed.JournalledEntries = Journal.Size();
  Observed.JournalledPaths =
      Journal.CountOf(InstallationScope::VirtualMachinePath);
  Observed.JournalledOverlays = Journal.Size() - Observed.JournalledPaths -
                                Journal.CountOf(InstallationScope::Binding);
  Observed.InstalledPaths = 0;
  Observed.PriorValueKinds.clear();
  for (const JournalEntry &Entry : Journal.Journalled()) {
    if (Entry.Scope != InstallationScope::VirtualMachinePath)
      continue;
    if (Entry.IsInstalled)
      ++Observed.InstalledPaths;
    Observed.PriorValueKinds.emplace_back(VmValueKindText(Entry.PriorKind()));
  }
  Observed.RestorationOrder = Journal.RestorationOrder();
  Observed.RestoredEveryEntry = Journal.RestoredEveryEntry();
  Observed.RestoredEntryStackDepth = Journal.RestoredEntryStackDepth();
  Observed.EntryStackDepth = Journal.EntryStackDepth();
}

InstallationOutcome
InstallPlannedDeclarations(const RegistrationTransaction &Transaction,
                           const TypeGeneration &Types, BindingStore &Bindings,
                           VirtualMachineOwner &Machine, FaultInjector &Faults,
                           InstallationJournal &Journal) {
  InstallationOutcome Outcome;

  // Canonical order, never submission order: an equivalent plan installs its
  // declarations in one identical sequence.
  for (const std::size_t Index : Transaction.Plan().CanonicalOrder()) {
    const DescriptorPlanEntry *Entry = Transaction.Plan().At(Index);
    if (!Entry)
      continue;

    // A declaration that installs one converted value or one Luna-owned
    // immutable table: a constant, and the table of one enumeration. Both are
    // journalled first and neither ever replaces a value Luna does not own.
    if (Entry->InstalledValue || Entry->InstalledTable) {
      if (Faults.Consume(StateFaultPoint::BindingPathJournal) ||
          !Journal.JournalVirtualMachinePath(Entry->VmPath)) {
        Outcome.Status = InstallationStatus::JournalFailure;
        Outcome.Path = Entry->VmPath;
        return Outcome;
      }

      const ValueInstallationStatus Installed =
          Faults.Consume(StateFaultPoint::BindingInstallation)
              ? ValueInstallationStatus::ProtectedFailure
          : Entry->InstalledTable
              ? Machine.InstallImmutableTable(Entry->VmPath, Types,
                                              *Entry->InstalledTable)
              : Machine.InstallValue(Entry->VmPath, Types,
                                     *Entry->InstalledValue);
      switch (Installed) {
      case ValueInstallationStatus::Installed:
        Journal.MarkInstalled();
        ++Outcome.Installed;
        continue;
      case ValueInstallationStatus::StackCapacityFailure:
        Outcome.Status = InstallationStatus::StackCapacityFailure;
        break;
      default:
        Outcome.Status = InstallationStatus::ProtectedFailure;
        break;
      }
      Outcome.Path = Entry->VmPath;
      return Outcome;
    }

    // A namespace owns one Luna table at its exact path, and so does a class:
    // its constructors, factories, and static members declare themselves inside
    // it. Canonical order visits a parent before its children, so nested tables
    // are created from parent to child and restoration removes them in reverse
    // order.
    if (Entry->Category == PlanEntryKind::Scope ||
        Entry->Category == PlanEntryKind::ClassSymbol) {
      if (Faults.Consume(StateFaultPoint::BindingPathJournal) ||
          !Journal.JournalVirtualMachinePath(Entry->VmPath)) {
        Outcome.Status = InstallationStatus::JournalFailure;
        Outcome.Path = Entry->VmPath;
        return Outcome;
      }

      const NamespaceTableInstallation Installed =
          Faults.Consume(StateFaultPoint::BindingInstallation)
              ? NamespaceTableInstallation()
              : Machine.InstallNamespaceTable(Entry->VmPath);
      Machine.ReleaseNamespaceTable(Installed.Reference);
      switch (Installed.Status) {
      case NamespaceTableStatus::Created:
      case NamespaceTableStatus::Reopened:
        Journal.MarkInstalled();
        ++Outcome.Installed;
        continue;
      case NamespaceTableStatus::StackCapacityFailure:
        Outcome.Status = InstallationStatus::StackCapacityFailure;
        break;
      default:
        Outcome.Status = InstallationStatus::ProtectedFailure;
        break;
      }
      Outcome.Path = Entry->VmPath;
      return Outcome;
    }

    // A loaded module publishes metadata only: its exported symbols install
    // themselves as their own declarations. Its overlay is journalled in the
    // module scope, so a failed load discards the module entry with the
    // attempt.
    if (Entry->Category == PlanEntryKind::Module) {
      Journal.JournalOverlay(InstallationScope::Module, Entry->VmPath);
      continue;
    }

    // The metatable identity of one class installs no virtual-machine value:
    // the class metatable itself is created by the first exposure of a value.
    // The staged identity is journalled in the metatable scope, so a failed
    // attempt discards it with everything else.
    if (Entry->Category == PlanEntryKind::Metatable) {
      Journal.JournalOverlay(InstallationScope::Metatable, Entry->VmPath);
      continue;
    }

    // Only function declarations own another virtual-machine value today. Every
    // other category installs nothing until its milestone lands, so its overlay
    // is journalled without a virtual-machine effect.
    if (Entry->Category != PlanEntryKind::Function) {
      Journal.JournalOverlay(InstallationScope::Reflection, Entry->VmPath);
      continue;
    }

    BindingRecord *Record = Bindings.Find(Entry->VmPath);
    if (!Record || !Record->HasStagedCandidate()) {
      Outcome.Status = InstallationStatus::MissingStagedResource;
      Outcome.Path = Entry->VmPath;
      return Outcome;
    }

    Journal.JournalStagedBinding(Entry->VmPath, Record);

    // A candidate joining an overload set whose closure is already installed -
    // by an earlier generation or by an earlier declaration of this same
    // attempt - installs no second virtual-machine value: the path already
    // holds exactly this record's closure, and the new candidate becomes
    // reachable through it at publication.
    if (Machine.ObserveInstalledBinding(Entry->VmPath) == Record)
      continue;

    if (Faults.Consume(StateFaultPoint::BindingPathJournal) ||
        !Journal.JournalVirtualMachinePath(Entry->VmPath)) {
      Outcome.Status = InstallationStatus::JournalFailure;
      Outcome.Path = Entry->VmPath;
      return Outcome;
    }

    const bool InjectFailure =
        Faults.Consume(StateFaultPoint::BindingInstallation);
    switch (Machine.InstallBindingClosure(*Record, InjectFailure)) {
    case ClosureInstallationStatus::Success:
      Journal.MarkInstalled();
      ++Outcome.Installed;
      continue;
    case ClosureInstallationStatus::StackCapacityFailure:
      Outcome.Status = InstallationStatus::StackCapacityFailure;
      Outcome.Path = Entry->VmPath;
      return Outcome;
    case ClosureInstallationStatus::RollbackFailure:
      Outcome.Status = InstallationStatus::RestorationFailure;
      Outcome.Path = Entry->VmPath;
      return Outcome;
    case ClosureInstallationStatus::ProtectedFailure:
      break;
    }

    Outcome.Status = InstallationStatus::ProtectedFailure;
    Outcome.Path = Entry->VmPath;
    return Outcome;
  }

  return Outcome;
}

ConsistencyStatus CheckPublicationConsistency(
    const RegistrationTransaction &Transaction,
    const PreparedGenerations &Prepared, const ReflectionDatabase &Reflection,
    const BindingStore &Bindings, const VirtualMachineOwner &Machine,
    FaultInjector &Faults) {
  if (Faults.Consume(StateFaultPoint::TransactionConsistency))
    return ConsistencyStatus::InjectedContradiction;

  if (!Prepared.IsPrepared())
    return ConsistencyStatus::MissingCandidate;

  const GenerationSet &Captured = Transaction.Captured();
  const GenerationSet &Candidate = *Prepared.Candidate;
  const DescriptorPlan &Plan = Transaction.Plan();

  if (Candidate.Generation() != Captured.Generation() + 1)
    return ConsistencyStatus::GenerationMismatch;
  if (Candidate.Symbols().Size() != Captured.Symbols().Size() + Plan.Size())
    return ConsistencyStatus::SymbolCountMismatch;

  for (const DescriptorPlanEntry &Entry : Plan.PlannedEntries()) {
    // The candidate symbol is located by its identity, because one qualified
    // name may own several overload candidates and each one is its own symbol.
    const CommittedSymbol *Symbol = Candidate.Symbols().Find(Entry.Identity);
    if (!Symbol)
      return ConsistencyStatus::MissingSymbol;
    if (Symbol->Symbol.QualifiedName != Entry.Symbol.QualifiedName)
      return ConsistencyStatus::IdentityMismatch;
    if (Symbol->Category != Entry.Category)
      return ConsistencyStatus::CategoryMismatch;
    if (Symbol->VmPath != Entry.VmPath)
      return ConsistencyStatus::PathMismatch;

    if (Entry.Category != PlanEntryKind::Function)
      continue;

    // The virtual machine and the canonical model must describe the same
    // callable before either becomes visible.
    const BindingRecord *Record = Bindings.Find(Entry.VmPath);
    if (!Record)
      return ConsistencyStatus::MissingBinding;
    if (Machine.ObserveInstalledBinding(Record->GlobalName()) != Record)
      return ConsistencyStatus::UninstalledBinding;
  }

  if (!Prepared.ReflectionAdvances)
    return ConsistencyStatus::Consistent;

  if (!Prepared.Reflection)
    return ConsistencyStatus::MissingCandidate;
  if (Prepared.Reflection->Generation() != Reflection.Generation() + 1)
    return ConsistencyStatus::ReflectionGenerationMismatch;
  if (Prepared.Reflection->RecordCount() !=
      Reflection.Count() + PlannedReflectionRecordCount(Plan))
    return ConsistencyStatus::ReflectionContentMismatch;
  return ConsistencyStatus::Consistent;
}

} // namespace Luna::Detail
