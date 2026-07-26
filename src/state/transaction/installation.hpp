#pragma once

// Phases four and five of one registration transaction: protected installation
// behind an undo journal, and atomic publication.
//
// The journal is the reason installation is safe. Before the installer writes
// anything, every canonical virtual-machine path it is about to touch records
// its exact prior value or its absence, and every pending overlay - binding,
// type, reflection, dispatch, module, metatable, identity cache, and lookup
// cache - records the entry it staged. On any failure the journal restores
// those paths in reverse order, discards every overlay, and returns the root
// stack to its exact entry depth, so a failed attempt is indistinguishable from
// one that never started.
//
// Publication happens only after every installation of the attempt succeeds and
// only after the internal consistency check accepts the candidate metadata. It
// is the single visibility point: before it, no ordinary virtual-machine,
// reflection, or dispatch query can observe any part of the attempt.
//
// Categories whose stores arrive with later milestones - types beyond the
// migrated foundation converters, dispatch indirection, modules, metatables,
// and caches - already have a journal scope here so their overlays are recorded
// and restored through the same reverse-order path as the ones that exist
// today.

// clang-format off
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include "state/reflection/database.hpp"
#include "state/registration/plan.hpp"
#include "state/transaction/preparation.hpp"
#include "state/transaction/transaction.hpp"
#include "state/vm/owner.hpp"
#include "state/vm/saved_value.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

class BindingRecord;
class BindingStore;
class FaultInjector;
class TypeGeneration;

// Which part of the pending model one journal entry owns. Every category
// registration will ever install has a scope, so nothing is installed without a
// recorded way back.
enum class InstallationScope {
  VirtualMachinePath,
  Binding,
  Type,
  Reflection,
  Dispatch,
  Module,
  Metatable,
  IdentityCache,
  LookupCache
};

[[nodiscard]] std::string_view
InstallationScopeText(InstallationScope Scope) noexcept;

// One journalled effect of an attempt.
struct JournalEntry final {
  InstallationScope Scope = InstallationScope::VirtualMachinePath;

  // The canonical virtual-machine path, or the overlay key, this entry owns.
  std::string Path;

  // The exact prior contents of the path, including its absence.
  SavedVmValue Prior;

  // The installer actually wrote this entry's path.
  bool IsInstalled = false;

  // Restoration put the prior value back, or discarded the staged overlay.
  bool IsRestored = false;

  // The staged, still uncommitted binding record of a function declaration.
  BindingRecord *Staged = nullptr;

  [[nodiscard]] bool PriorValueExisted() const noexcept {
    return Prior.Existed();
  }

  [[nodiscard]] VmValueKind PriorKind() const noexcept { return Prior.Kind; }
};

class InstallationJournal final {
public:
  InstallationJournal(VirtualMachineOwner &Machine, BindingStore &Bindings,
                      FaultInjector &Faults, int EntryStackDepth) noexcept;

  InstallationJournal(const InstallationJournal &) = delete;
  InstallationJournal &operator=(const InstallationJournal &) = delete;
  InstallationJournal(InstallationJournal &&) = delete;
  InstallationJournal &operator=(InstallationJournal &&) = delete;

  // A journal that was neither published nor restored restores itself, so an
  // early return or an exception can never leave a half-installed attempt.
  ~InstallationJournal() noexcept;

  // Records the exact prior value or absence of one canonical path before it is
  // installed over. A failed capture journals nothing.
  [[nodiscard]] bool JournalVirtualMachinePath(std::string Path);

  // The most recently journalled path was written by the installer.
  void MarkInstalled() noexcept;

  // Records one staged binding overlay so restoration can discard it.
  void JournalStagedBinding(std::string Path, BindingRecord *Record);

  // Records one pending overlay of a category that has no committed store yet.
  void JournalOverlay(InstallationScope Scope, std::string Key);

  // Restores every journalled effect in reverse order and returns the root
  // stack to its exact entry depth.
  void Undo() noexcept;

  // Keeps every installed value and releases the captured prior values.
  void Commit() noexcept;

  [[nodiscard]] bool IsCommitted() const noexcept { return CommittedFlag; }
  [[nodiscard]] bool IsUndone() const noexcept { return UndoneFlag; }

  [[nodiscard]] std::size_t Size() const noexcept { return Entries.size(); }
  [[nodiscard]] std::span<const JournalEntry> Journalled() const noexcept {
    return Entries;
  }
  [[nodiscard]] std::size_t CountOf(InstallationScope Scope) const noexcept;

  // The order restoration actually visited, newest journalled entry first.
  [[nodiscard]] const std::vector<std::string> &
  RestorationOrder() const noexcept {
    return RestoredOrder;
  }

  [[nodiscard]] bool RestoredEveryEntry() const noexcept;
  [[nodiscard]] bool RestoredEntryStackDepth() const noexcept {
    return StackDepthRestored;
  }
  [[nodiscard]] int EntryStackDepth() const noexcept { return EntryDepth; }

private:
  void Release() noexcept;

  VirtualMachineOwner *Machine;
  BindingStore *Bindings;
  FaultInjector *Faults;
  int EntryDepth;
  std::vector<JournalEntry> Entries;
  std::vector<std::string> RestoredOrder;
  bool CommittedFlag = false;
  bool UndoneFlag = false;
  bool StackDepthRestored = false;
};

// Deterministic outcome of the protected installation phase.
enum class InstallationStatus {
  Installed,
  MissingStagedResource,
  JournalFailure,
  StackCapacityFailure,
  ProtectedFailure,
  RestorationFailure
};

[[nodiscard]] std::string_view
InstallationStatusText(InstallationStatus Status) noexcept;

struct InstallationOutcome final {
  InstallationStatus Status = InstallationStatus::Installed;

  // The declaration that failed, when one did.
  std::string Path;
  std::size_t Installed = 0;

  [[nodiscard]] bool IsInstalled() const noexcept {
    return Status == InstallationStatus::Installed;
  }
};

// Installs every planned declaration of the transaction in canonical order,
// journalling each touched path first. The first failure stops the phase; the
// caller restores the journal.
//
// `Types` is the candidate type generation of the same attempt, because a
// declaration that installs one converted value - a constant, or the table of
// an enumeration the same plan declares - must convert through exactly the
// generation this attempt is about to publish.
[[nodiscard]] InstallationOutcome
InstallPlannedDeclarations(const RegistrationTransaction &Transaction,
                           const TypeGeneration &Types, BindingStore &Bindings,
                           VirtualMachineOwner &Machine, FaultInjector &Faults,
                           InstallationJournal &Journal);

// Deterministic reason the candidate metadata of one attempt is coherent or
// contradictory. Every non-`Consistent` value rejects publication.
enum class ConsistencyStatus {
  Consistent,
  MissingCandidate,
  GenerationMismatch,
  SymbolCountMismatch,
  MissingSymbol,
  IdentityMismatch,
  CategoryMismatch,
  PathMismatch,
  MissingBinding,
  UninstalledBinding,
  ReflectionGenerationMismatch,
  ReflectionContentMismatch,
  InjectedContradiction
};

[[nodiscard]] std::string_view
ConsistencyStatusText(ConsistencyStatus Status) noexcept;

// What the installation and publication phases of one attempt observed. Private
// test hooks read it; no public API can reach it.
struct PublicationObservation final {
  bool IsPublished = false;
  std::uint64_t PublishedGeneration = 0;
  std::size_t PublishedSymbols = 0;
  bool ReflectionAdvanced = false;
  std::uint64_t PublishedReflectionGeneration = 0;

  PreparationStatus Preparation = PreparationStatus::Prepared;
  InstallationStatus Installation = InstallationStatus::Installed;
  ConsistencyStatus Consistency = ConsistencyStatus::Consistent;

  // The journal of the attempt: what it recorded, what it installed, and what
  // restoration did with it.
  std::size_t JournalledEntries = 0;
  std::size_t JournalledPaths = 0;
  std::size_t JournalledOverlays = 0;
  std::size_t InstalledPaths = 0;
  std::vector<std::string> PriorValueKinds;
  std::vector<std::string> RestorationOrder;
  bool RestoredEveryEntry = false;
  bool RestoredEntryStackDepth = false;
  int EntryStackDepth = 0;
  int StackDepthAfter = 0;
};

// Records the journal of one attempt into an observation, in canonical journal
// order, so a failed attempt can be inspected without exposing the journal.
void ObserveJournal(const InstallationJournal &Journal,
                    PublicationObservation &Observed);

// Compares the candidate generation set, the candidate reflection generation,
// the staged binding overlays, and the installed virtual-machine paths against
// the canonical plan. It runs after installation and before publication, so a
// contradiction is reported instead of published.
[[nodiscard]] ConsistencyStatus CheckPublicationConsistency(
    const RegistrationTransaction &Transaction,
    const PreparedGenerations &Prepared, const ReflectionDatabase &Reflection,
    const BindingStore &Bindings, const VirtualMachineOwner &Machine,
    FaultInjector &Faults);

} // namespace Luna::Detail
