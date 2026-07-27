#pragma once

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

enum class InstallationScope {
  VirtualMachinePath,
  Binding,
  Type,
  Reflection,
  Dispatch,
  Module,
  Metatable,
  IdentityCache,
  LookupCache,

  Userdata
};

[[nodiscard]] std::string_view
InstallationScopeText(InstallationScope Scope) noexcept;

struct JournalEntry final {
  InstallationScope Scope = InstallationScope::VirtualMachinePath;

  std::string Path;

  SavedVmValue Prior;

  bool IsInstalled = false;

  bool IsRestored = false;

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

  ~InstallationJournal() noexcept;

  [[nodiscard]] bool JournalVirtualMachinePath(std::string Path);

  void MarkInstalled() noexcept;

  void JournalStagedBinding(std::string Path, BindingRecord *Record);

  void JournalOverlay(InstallationScope Scope, std::string Key);

  void Undo() noexcept;

  void Commit() noexcept;

  [[nodiscard]] bool IsCommitted() const noexcept { return CommittedFlag; }
  [[nodiscard]] bool IsUndone() const noexcept { return UndoneFlag; }

  [[nodiscard]] std::size_t Size() const noexcept { return Entries.size(); }
  [[nodiscard]] std::span<const JournalEntry> Journalled() const noexcept {
    return Entries;
  }
  [[nodiscard]] std::size_t CountOf(InstallationScope Scope) const noexcept;

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

  std::string Path;
  std::size_t Installed = 0;

  [[nodiscard]] bool IsInstalled() const noexcept {
    return Status == InstallationStatus::Installed;
  }
};

[[nodiscard]] InstallationOutcome
InstallPlannedDeclarations(const RegistrationTransaction &Transaction,
                           const TypeGeneration &Types, BindingStore &Bindings,
                           VirtualMachineOwner &Machine, FaultInjector &Faults,
                           InstallationJournal &Journal);

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

struct PublicationObservation final {
  bool IsPublished = false;
  std::uint64_t PublishedGeneration = 0;
  std::size_t PublishedSymbols = 0;
  bool ReflectionAdvanced = false;
  std::uint64_t PublishedReflectionGeneration = 0;

  PreparationStatus Preparation = PreparationStatus::Prepared;
  InstallationStatus Installation = InstallationStatus::Installed;
  ConsistencyStatus Consistency = ConsistencyStatus::Consistent;

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

void ObserveJournal(const InstallationJournal &Journal,
                    PublicationObservation &Observed);

[[nodiscard]] ConsistencyStatus CheckPublicationConsistency(
    const RegistrationTransaction &Transaction,
    const PreparedGenerations &Prepared, const ReflectionDatabase &Reflection,
    const BindingStore &Bindings, const VirtualMachineOwner &Machine,
    FaultInjector &Faults);

} // namespace Luna::Detail
