#pragma once

// clang-format off
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include "state/module/lifecycle.hpp"
#include "state/transaction/generation_set.hpp"
#include "state/transaction/lifecycle_staging.hpp"
#include "state/transaction/transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

class BindingStore;
class FaultInjector;
class FreezeCacheStorage;
class LazyPropertyCache;
class ModuleRegistry;
class ReflectionDatabase;
class UserdataIdentityCache;
class VirtualMachineOwner;

enum class LifecyclePublishStatus : std::uint8_t {
  Published,

  NothingStaged,

  UnsupportedDynamicMode,

  UnusableTransaction,

  ModuleFailure,

  ReflectionFailure,

  DispatchFailure,

  GenerationFailure
};

[[nodiscard]] std::string_view
LifecyclePublishStatusText(LifecyclePublishStatus Status) noexcept;

struct LifecyclePublicationTargets final {
  VirtualMachineOwner *Machine = nullptr;
  BindingStore *Bindings = nullptr;
  FaultInjector *Faults = nullptr;
  ModuleRegistry *Modules = nullptr;
  ReflectionDatabase *Reflection = nullptr;
  LazyPropertyCache *LazyValues = nullptr;
  UserdataIdentityCache *Identities = nullptr;

  std::shared_ptr<const GenerationSet> *Generations = nullptr;
  std::shared_ptr<const FreezeCacheStorage> *Caches = nullptr;

  [[nodiscard]] bool IsComplete() const noexcept {
    return Machine != nullptr && Bindings != nullptr && Faults != nullptr &&
           Modules != nullptr && Reflection != nullptr &&
           Generations != nullptr;
  }
};

struct LifecyclePublicationObservation final {
  LifecyclePublishStatus Status = LifecyclePublishStatus::NothingStaged;
  std::string Diagnostic;

  bool IsPublished = false;
  bool JournalCommitted = false;

  std::uint64_t PublishedGeneration = 0;
  std::uint64_t PublishedReflectionGeneration = 0;
  std::uint64_t PublishedTypeGeneration = 0;
  std::uint64_t PublishedDispatchGeneration = 0;
  std::size_t PublishedModuleCount = 0;
  std::size_t PublishedSymbolCount = 0;

  std::vector<std::string> InvalidatedCaches;

  bool InvalidatedCachesBeforePublication = false;
  bool DroppedFrozenCaches = false;
  std::size_t DroppedLazyEntries = 0;
  std::size_t DroppedIdentityEntries = 0;
  std::size_t RetainedIdentityEntries = 0;

  std::vector<std::string> ClearedPaths;
  std::vector<std::string> UnavailableSlots;
  std::vector<std::string> RetainedSlots;

  std::uint64_t SupersededDispatchGeneration = 0;
  bool PreviousDispatchRetained = false;
  std::size_t LifecycleJournalRetainers = 0;
};

[[nodiscard]] LifecyclePublishStatus
PublishLifecycle(RegistrationTransaction &Transaction,
                 const LifecyclePlan &Plan, PreparedLifecycle &Prepared,
                 const LifecyclePublicationTargets &Targets,
                 LifecyclePublicationObservation &Observed);

struct LifecycleCommitAttempt final {
  LifecycleAttempt Staged;

  bool PublishWithoutDynamicLifecycle = false;

  bool PublishWithoutStaging = false;

  bool RetainInvocationGeneration = false;

  std::string SourceBeforePublication;
  std::string SourceAfterPublication;

  std::vector<std::string> ProbedPaths;
};

struct LifecycleCommitObservation final {
  LifecycleStagingObservation Staging;
  LifecyclePublicationObservation Publication;

  std::uint64_t GenerationBefore = 0;
  std::uint64_t ReflectionGenerationBefore = 0;
  std::uint64_t DispatchGenerationBefore = 0;
  std::uint64_t LifecycleGenerationBefore = 0;
  std::size_t ModuleCountBefore = 0;
  std::size_t SymbolCountBefore = 0;
  std::size_t OwnershipRecordsBefore = 0;
  std::size_t NamespaceOwnershipsBefore = 0;
  std::vector<std::string> ReflectionIdentitiesBefore;
  std::vector<std::string> ProbedPathKindsBefore;
  int StackDepthBefore = 0;

  std::uint64_t GenerationAfter = 0;
  std::uint64_t ReflectionGenerationAfter = 0;
  std::uint64_t DispatchGenerationAfter = 0;
  std::uint64_t LifecycleGenerationAfter = 0;
  std::size_t ModuleCountAfter = 0;
  std::size_t SymbolCountAfter = 0;
  std::size_t OwnershipRecordsAfter = 0;
  std::size_t NamespaceOwnershipsAfter = 0;
  std::vector<std::string> ReflectionIdentitiesAfter;
  std::vector<std::string> ProbedPathKindsAfter;
  int StackDepthAfter = 0;

  bool ModuleStillLoaded = false;
  std::string LoadedVersionAfter;

  bool SourceBeforeSucceeded = false;
  std::string SourceBeforeDiagnostic;
  bool SourceAfterSucceeded = false;
  std::string SourceAfterDiagnostic;

  bool TransactionCommitted = false;
  bool TransactionPoisoned = false;
  std::string TransactionFailure;

  std::string RetainedProbe;
  std::uint64_t RetainedGenerationNumber = 0;
  bool RetainedGenerationResolvesOldTarget = false;

  std::size_t SupersededDispatchGenerations = 0;
  std::size_t RetainedDispatchGenerations = 0;
  std::size_t LifecycleJournalRetainersAfter = 0;
  std::size_t ReclaimedAfterRelease = 0;
};

} // namespace Luna::Detail
