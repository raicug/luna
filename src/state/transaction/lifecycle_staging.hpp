#pragma once

// clang-format off
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/module/module_manifest.hpp>

#include "state/dispatch/generation.hpp"
#include "state/module/lifecycle.hpp"
#include "state/transaction/generation_set.hpp"
#include "state/transaction/installation.hpp"
#include "state/transaction/transaction.hpp"
#include "state/type/type_generation.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

class BindingStore;
class FaultInjector;
class FreezeCacheStorage;
class ModuleRegistry;
class ReflectionStorage;
class VirtualMachineOwner;

enum class LifecycleStagedKind : std::uint8_t {
  ModuleGraph,
  TablePath,
  Type,
  UserdataAction,
  Reflection,
  Cache,
  UnavailableSlot,
  DispatchTarget
};

[[nodiscard]] std::string_view
LifecycleStagedKindText(LifecycleStagedKind Kind) noexcept;

enum class LifecycleUserdataAction : std::uint8_t { Migrate, RemainValid };

[[nodiscard]] std::string_view
LifecycleUserdataActionText(LifecycleUserdataAction Action) noexcept;

struct LifecycleStagedItem final {
  LifecycleStagedKind Kind = LifecycleStagedKind::ModuleGraph;
  std::string Subject;
  std::string Detail;

  [[nodiscard]] std::string Text() const;

  friend bool operator==(const LifecycleStagedItem &Left,
                         const LifecycleStagedItem &Right);
};

[[nodiscard]] std::strong_ordering
CompareStaged(const LifecycleStagedItem &Left,
              const LifecycleStagedItem &Right);

struct LifecycleUserdataPolicy final {
  std::string Subject;
  std::string ClassQualifiedName;

  bool MigrationAvailable = false;
  bool RemainsValid = false;
};

struct LifecyclePlan final {
  LifecycleOperation Operation = LifecycleOperation::Unload;

  std::string Identity;
  ModuleManifest Replacement;

  bool DynamicLifecycleEnabled = false;

  std::vector<std::string> RemovedSubjects;

  std::vector<std::string> RetainedPaths;

  std::vector<std::string> RemovedTypes;

  std::vector<LifecycleUserdataPolicy> LiveUserdata;

  std::vector<LifecycleCacheEntry> InvalidatedCaches;
};

enum class LifecycleStageStatus : std::uint8_t {
  Prepared,
  ValidationFailure,
  CallbackFailure,
  AllocationFailure,
  InstallationFailure,
  MigrationFailure,
  CacheFailure,
  PublicationFailure
};

[[nodiscard]] std::string_view
LifecycleStageStatusText(LifecycleStageStatus Status) noexcept;

struct LifecycleStagingSources final {
  VirtualMachineOwner *Machine = nullptr;
  BindingStore *Bindings = nullptr;
  FaultInjector *Faults = nullptr;
  const ModuleRegistry *Modules = nullptr;

  std::shared_ptr<const FreezeCacheStorage> Caches;
};

struct LifecycleStagingObservation final {
  LifecycleStageStatus Status = LifecycleStageStatus::Prepared;
  std::string Diagnostic;

  std::vector<std::string> Staged;

  bool IsPrepared = false;
  bool IsRolledBack = false;

  bool CompletedStaging = false;

  std::size_t JournalledEntries = 0;
  std::size_t JournalledPaths = 0;
  std::size_t JournalledOverlays = 0;
  std::vector<std::string> JournalledPathNames;
  std::vector<std::string> PriorValueKinds;
  std::vector<std::string> RestorationOrder;
  bool RestoredEveryEntry = false;
  bool RestoredEntryStackDepth = false;
  int EntryStackDepth = 0;

  std::uint64_t PreviousGeneration = 0;
  std::uint64_t PreviousReflectionGeneration = 0;
  std::uint64_t PreviousTypeGeneration = 0;
  std::uint64_t PreviousDispatchGeneration = 0;
  std::size_t PreviousModuleCount = 0;
  std::size_t PreviousSymbolCount = 0;

  std::uint64_t StagedGeneration = 0;
  std::uint64_t StagedReflectionGeneration = 0;
  std::uint64_t StagedTypeGeneration = 0;
  std::uint64_t StagedDispatchGeneration = 0;
  std::size_t StagedModuleCount = 0;
  std::size_t StagedSymbolCount = 0;
  std::size_t StagedUnavailableSlots = 0;
  std::size_t StagedDispatchTargets = 0;
  std::size_t StagedUserdataActions = 0;
  std::size_t StagedCacheEntries = 0;

  bool PreviousDispatchRetained = false;
  std::size_t LifecycleJournalRetainers = 0;
};

class PreparedLifecycle final {
public:
  PreparedLifecycle() = default;
  ~PreparedLifecycle() noexcept;

  PreparedLifecycle(const PreparedLifecycle &) = delete;
  PreparedLifecycle &operator=(const PreparedLifecycle &) = delete;
  PreparedLifecycle(PreparedLifecycle &&) = delete;
  PreparedLifecycle &operator=(PreparedLifecycle &&) = delete;

  std::vector<ModuleManifest> ModuleGraph;
  std::shared_ptr<const CommittedSymbolTable> Symbols;
  std::shared_ptr<const TypeGeneration> Types;
  std::shared_ptr<const ReflectionStorage> Reflection;
  std::shared_ptr<const GenerationSet> Candidate;
  std::shared_ptr<const DispatchGeneration> Dispatch;
  std::vector<LifecycleCacheEntry> InvalidatedCaches;
  std::vector<LifecycleStagedItem> Staged;

  std::vector<std::string> RemovedPaths;
  std::vector<std::string> RetainedPaths;
  std::vector<SymbolId> RemovedSymbols;

  std::shared_ptr<const FreezeCacheStorage> PreviousCaches;

  DispatchRetention PreviousDispatch;

  std::unique_ptr<InstallationJournal> Journal;

  [[nodiscard]] bool IsStaged() const noexcept {
    return Candidate != nullptr && Dispatch != nullptr;
  }

  [[nodiscard]] bool IsRolledBack() const noexcept { return RolledBack; }

  [[nodiscard]] bool IsCommitted() const noexcept { return Committed; }

  void Rollback() noexcept;

  void Commit() noexcept;

  [[nodiscard]] const LifecycleStagingObservation &Observed() const noexcept {
    return Observation;
  }

  [[nodiscard]] LifecycleStagingObservation &Observed() noexcept {
    return Observation;
  }

private:
  LifecycleStagingObservation Observation;
  bool RolledBack = false;
  bool Committed = false;
};

using LifecycleStagingCallback =
    std::function<std::optional<ErrorDiagnostic>()>;

[[nodiscard]] LifecycleStageStatus PrepareLifecycle(
    RegistrationTransaction &Transaction, const LifecyclePlan &Plan,
    const LifecycleAnalysis &Analysis, const LifecycleStagingSources &Sources,
    const LifecycleStagingCallback &Callback, PreparedLifecycle &Prepared);

void ObserveLifecycleStaging(const PreparedLifecycle &Prepared,
                             LifecycleStagingObservation &Observed);

struct LifecycleAttempt final {
  LifecyclePlan Plan;

  std::vector<LifecycleBlocker> Blockers;

  bool RunCallback = false;
  bool CallbackFails = false;
  bool CallbackThrows = false;
};

struct LifecycleAttemptObservation final {
  LifecycleStagingObservation Staging;

  bool TransactionPoisoned = false;
  std::string TransactionFailure;

  std::uint64_t GenerationWhileStaged = 0;
  std::uint64_t ReflectionGenerationWhileStaged = 0;
  std::uint64_t DispatchGenerationWhileStaged = 0;
  std::size_t ModuleCountWhileStaged = 0;
  std::size_t SymbolCountWhileStaged = 0;
  std::vector<std::string> PathKindsWhileStaged;
  int StackDepthWhileStaged = 0;

  std::uint64_t GenerationAfter = 0;
  std::uint64_t ReflectionGenerationAfter = 0;
  std::uint64_t DispatchGenerationAfter = 0;
  std::size_t ModuleCountAfter = 0;
  std::size_t SymbolCountAfter = 0;
  std::vector<std::string> PathKindsAfter;
  int StackDepthAfter = 0;

  bool ModuleStillLoaded = false;

  std::size_t SupersededDispatchGenerations = 0;
  std::size_t RetainedDispatchGenerations = 0;
  std::size_t LifecycleJournalRetainersAfter = 0;
};

} // namespace Luna::Detail
