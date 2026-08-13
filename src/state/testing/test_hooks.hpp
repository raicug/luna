#pragma once

// clang-format off
#include <luna/binding/value.hpp>

#include "state/dispatch/generation.hpp"
#include "state/freeze/cache.hpp"
#include "state/invocation/async/suspended_call.hpp"
#include "state/invocation/delegate/vm_delegate.hpp"
#include "state/module/lifecycle.hpp"
#include "state/module/registry.hpp"
#include "state/reflection/database.hpp"
#include "state/registration/submission.hpp"
#include "state/testing/fault_injector.hpp"
#include "state/testing/fault_point.hpp"
#include "state/testing/test_control.hpp"
#include "state/transaction/capture.hpp"
#include "state/transaction/installation.hpp"
#include "state/transaction/generation_set.hpp"
#include "state/transaction/lifecycle.hpp"
#include "state/transaction/lifecycle_publication.hpp"
#include "state/transaction/lifecycle_staging.hpp"
#include "state/userdata/allocator.hpp"
#include "state/userdata/class_registry.hpp"
#include "state/userdata/collection.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/identity.hpp"
#include "state/userdata/identity_cache.hpp"
#include "state/userdata/lazy_cache.hpp"
#include "state/userdata/member_access.hpp"
#include "state/userdata/member_dispatch.hpp"
#include "state/userdata/ownership.hpp"
#include "state/userdata/value_exposure.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna {
class State;
}

namespace Luna::Detail {

struct NativeInvocationObservation final {
  bool Succeeded = false;
  int ReturnCount = 0;
  std::optional<Value> ReturnedValue;
  std::string ErrorMessage;
  int EntryStackDepth = 0;
  int CompletionStackDepth = 0;
  int FinalStackDepth = 0;
};

struct FreezeCacheObservation final {
  bool Published = false;
  std::uintptr_t Address = 0;
  FreezeCacheKey Key;
  std::size_t Lookups = 0;
  std::size_t Overloads = 0;
  std::size_t Conversions = 0;
  std::size_t CastPaths = 0;
  std::size_t Metatables = 0;
  std::size_t Namespaces = 0;
  std::size_t Modules = 0;
  std::vector<std::string> OrderedLookups;

  std::vector<std::string> LookupDetails;
  std::vector<std::string> OrderedOverloads;
  std::vector<std::string> OrderedConversions;
  std::vector<std::string> OrderedCastPaths;
  std::vector<std::string> OrderedMetatables;
  std::vector<std::string> OrderedNamespaces;
  std::vector<std::string> OrderedModules;

  std::vector<std::uint64_t> MetatableIdentities;
};

struct ClassExposureRequest final {
  std::string QualifiedName;
  std::string Path;
  void *Storage = nullptr;
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  ConstAccess Access = ConstAccess::Mutable;

  const std::uint64_t *LifetimeGeneration = nullptr;
};

struct ClassExposureObservation final {
  std::string Status;
  bool Created = false;
  bool Reused = false;

  std::uint64_t Nonce = 0;
};

struct ClassValueExposureRequest final {
  std::string QualifiedName;
  std::string Path;
  void *Storage = nullptr;
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  ConstAccess Access = ConstAccess::Mutable;

  LifetimeHandle Handle = LifetimeHandle::Undeclared();
  std::shared_ptr<void> SharedOwnership;
  ClassAllocator Allocator;
};

struct ClassValueConstructionRequest final {
  std::string QualifiedName;
  std::string Path;
  OwnershipModel Ownership = OwnershipModel::LuaOwned;
  ConstAccess Access = ConstAccess::Mutable;

  ClassAllocator Allocator;

  ObjectConstruction Construct;

  LifetimeHandle Handle = LifetimeHandle::Undeclared();
  std::shared_ptr<void> SharedOwnership;
};

struct ClassMemberAccessRequest final {
  std::string QualifiedName;
  std::string Member;
  std::string Path;
  Value Incoming;
};

struct ClassMemberAccessObservation final {
  bool Reached = false;
  std::string Failure;
  std::string Receiver;
  std::string Diagnostic;

  std::string Boundary;

  std::optional<Value> Produced;
  bool ServedFromCache = false;
  bool Recorded = false;

  std::size_t Invalidated = 0;
};

struct ClassAccessRequest final {
  std::string QualifiedName;
  std::string Path;

  const void *ExpectedStorage = nullptr;
};

struct ClassAccessObservation final {
  bool ReachedNativeCode = false;
  bool DeliveredExpectedObject = false;
  bool PermitsMutation = false;
  std::string Failure;
  std::string Diagnostic;
};

struct PreparedNativeInvocation final {
  void *VirtualMachine = nullptr;
  int Reference = 0;
  int ReturnCount = 0;

  [[nodiscard]] bool IsValid() const noexcept {
    return VirtualMachine != nullptr && Reference > 0;
  }
};

class StateTestHooks final {
public:
  static void ResetLifecycle() noexcept;
  static void FailNextCreations(std::size_t Count = 1) noexcept;
  [[nodiscard]] static StateLifecycleCounters Lifecycle() noexcept;

  [[nodiscard]] static std::optional<int>
  ObserveRootStackDepth(const State &Owner) noexcept;
  [[nodiscard]] static bool SetRootStackDepth(State &Owner, int Depth) noexcept;
  [[nodiscard]] static std::size_t BindingCount(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  PendingBindingCount(const State &Owner) noexcept;
  [[nodiscard]] static bool
  BindingIsCommitted(const State &Owner, std::string_view GlobalName) noexcept;
  [[nodiscard]] static std::optional<std::uintptr_t>
  BindingRecordAddress(const State &Owner,
                       std::string_view GlobalName) noexcept;
  [[nodiscard]] static std::optional<std::uintptr_t>
  InstalledBindingRecordAddress(const State &Owner,
                                std::string_view GlobalName) noexcept;

  [[nodiscard]] static std::optional<std::uint64_t>
  DispatchSlotOf(const State &Owner, std::string_view GlobalName) noexcept;
  [[nodiscard]] static std::optional<std::uint64_t>
  InstalledDispatchSlotOf(const State &Owner,
                          std::string_view GlobalName) noexcept;
  [[nodiscard]] static bool
  DispatchSlotIsAvailable(const State &Owner,
                          std::string_view GlobalName) noexcept;
  [[nodiscard]] static std::size_t
  IssuedDispatchSlotCount(const State &Owner) noexcept;
  [[nodiscard]] static std::uint64_t
  DispatchGenerationOf(const State &Owner) noexcept;

  [[nodiscard]] static std::size_t
  DispatchInvocationRetainerCount(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  SupersededDispatchGenerationCount(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  RetainedDispatchGenerationCount(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  ReclaimDispatchGenerations(State &Owner) noexcept;

  [[nodiscard]] static bool RetireDispatchSlot(State &Owner,
                                               std::string_view GlobalName);
  [[nodiscard]] static bool RetargetDispatchSlot(State &Owner,
                                                 std::string_view SlotName,
                                                 std::string_view TargetName);

  [[nodiscard]] static DispatchRetention
  RetainDispatchGeneration(const State &Owner, DispatchRetainer Retainer);

  [[nodiscard]] static std::size_t
  OverloadCandidateCount(const State &Owner,
                         std::string_view GlobalName) noexcept;
  [[nodiscard]] static std::size_t
  StagedOverloadCandidateCount(const State &Owner,
                               std::string_view GlobalName) noexcept;
  [[nodiscard]] static std::vector<std::string>
  OverloadCandidateSignatures(const State &Owner, std::string_view GlobalName);
  [[nodiscard]] static bool SetIntegerGlobal(State &Owner,
                                             const std::string &GlobalName,
                                             int Value) noexcept;
  [[nodiscard]] static std::optional<int>
  ObserveIntegerGlobal(const State &Owner,
                       const std::string &GlobalName) noexcept;
  [[nodiscard]] static NativeInvocationObservation
  InvokeBinding(State &Owner, std::string_view GlobalName,
                const std::vector<Value> &Arguments);
  [[nodiscard]] static std::optional<PreparedNativeInvocation>
  PrepareBindingInvocation(State &Owner, std::string_view GlobalName,
                           int ReturnCount);
  [[nodiscard]] static bool
  InvokePreparedBinding(const PreparedNativeInvocation &Prepared,
                        std::span<const Value> Arguments);
  static void
  ReleasePreparedBinding(PreparedNativeInvocation &Prepared) noexcept;

  [[nodiscard]] static std::optional<CallbackStackRestorationObservation>
  ObserveLastCallbackStackRestoration(const State &Owner) noexcept;

  [[nodiscard]] static AsyncCallCounters
  AsyncCallCountersOf(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  PendingAsyncCallCount(const State &Owner) noexcept;

  [[nodiscard]] static DelegateCounters
  DelegateCountersOf(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  OutstandingDelegateCount(const State &Owner) noexcept;

  [[nodiscard]] static ReflectionDatabase *
  ReflectionDatabaseOf(State &Owner) noexcept;
  [[nodiscard]] static std::uint64_t
  ReflectionGeneration(const State &Owner) noexcept;

  [[nodiscard]] static std::shared_ptr<const GenerationSet>
  GenerationsOf(const State &Owner);
  [[nodiscard]] static std::optional<StateIdentity>
  LogicalIdentityOf(const State &Owner) noexcept;
  [[nodiscard]] static std::optional<std::uint64_t>
  OwnerEpochOf(const State &Owner) noexcept;
  [[nodiscard]] static std::optional<std::uint64_t>
  LifecycleGenerationOf(const State &Owner) noexcept;
  [[nodiscard]] static bool HasActiveTransaction(const State &Owner) noexcept;

  [[nodiscard]] static std::optional<TransactionCapture>
  CaptureTransactionEntryOf(const State &Owner);

  static bool MarkFrozen(State &Owner) noexcept;
  [[nodiscard]] static bool IsFrozen(const State &Owner) noexcept;
  [[nodiscard]] static FreezeCacheObservation
  ObserveFreezeCache(const State &Owner);

  static bool AdvanceLifecycleGeneration(State &Owner) noexcept;

  [[nodiscard]] static std::size_t
  LoadedModuleCount(const State &Owner) noexcept;
  [[nodiscard]] static bool ModuleIsLoaded(const State &Owner,
                                           std::string_view Identity) noexcept;
  [[nodiscard]] static std::optional<std::string>
  LoadedModuleVersion(const State &Owner, std::string_view Identity);
  [[nodiscard]] static std::size_t
  AvailableModuleCount(const State &Owner) noexcept;

  [[nodiscard]] static std::size_t
  NamespaceOwnershipCount(const State &Owner) noexcept;
  [[nodiscard]] static bool NamespaceIsOwned(const State &Owner,
                                             std::string_view QualifiedName);

  [[nodiscard]] static std::size_t
  RegisteredClassCount(const State &Owner) noexcept;
  [[nodiscard]] static bool ClassIsRegistered(const State &Owner,
                                              std::string_view QualifiedName);
  [[nodiscard]] static std::optional<TypeId>
  ClassTypeOf(const State &Owner, std::string_view QualifiedName);
  [[nodiscard]] static std::optional<std::uint64_t>
  ClassMetatableIdentityOf(const State &Owner, std::string_view QualifiedName);
  [[nodiscard]] static std::size_t
  IssuedMetatableIdentityCount(const State &Owner) noexcept;

  [[nodiscard]] static bool
  ClassMetatableIsCreated(const State &Owner, std::string_view QualifiedName);
  [[nodiscard]] static std::size_t
  ClassMetatableCreationCount(const State &Owner,
                              std::string_view QualifiedName);

  [[nodiscard]] static bool CollectGarbage(State &Owner) noexcept;

  [[nodiscard]] static bool
  UserdataCollectorIsInstalled(const State &Owner) noexcept;

  [[nodiscard]] static UserdataCollectionCounters
  ObserveUserdataCollections() noexcept;
  static void ResetUserdataCollections() noexcept;

  [[nodiscard]] static StateDestructionObservation
  ObserveLastStateDestruction() noexcept;

  [[nodiscard]] static std::optional<UserdataHeader>
  DescribeClassUserdata(const State &Owner, std::string_view QualifiedName,
                        OwnershipModel Ownership, ConstAccess Access);

  [[nodiscard]] static OwnershipRegistry *
  UserdataOwnershipOf(State &Owner) noexcept;

  [[nodiscard]] static ReleaseCounters
  UserdataReleaseCounters(const State &Owner) noexcept;

  [[nodiscard]] static ConstructionCounters
  UserdataConstructionCounters(const State &Owner) noexcept;

  [[nodiscard]] static std::size_t
  OwnedUserdataCount(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  PublishedUserdataCount(const State &Owner) noexcept;

  [[nodiscard]] static std::size_t
  CachedIdentityCount(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  LiveCachedIdentityCount(const State &Owner) noexcept;

  [[nodiscard]] static ClassExposureObservation
  ExposeClassUserdata(State &Owner, const ClassExposureRequest &Request);

  [[nodiscard]] static ClassValueWriteObservation
  ExposeClassValue(State &Owner, const ClassValueExposureRequest &Request);

  [[nodiscard]] static ClassValueWriteObservation
  ConstructClassValue(State &Owner,
                      const ClassValueConstructionRequest &Request);

  [[nodiscard]] static bool ReleaseClassValue(State &Owner, const void *Storage,
                                              ReleaseCause Cause);

  [[nodiscard]] static ClassAccessObservation
  AccessClassUserdata(State &Owner, const ClassAccessRequest &Request);

  [[nodiscard]] static bool RetireClassUserdata(State &Owner,
                                                const void *Storage);

  [[nodiscard]] static bool
  ClassMemberIsRegistered(const State &Owner, std::string_view QualifiedName,
                          std::string_view Member);
  [[nodiscard]] static std::size_t
  ClassMemberCount(const State &Owner, std::string_view QualifiedName);

  [[nodiscard]] static std::size_t
  ClassOperatorCount(const State &Owner, std::string_view QualifiedName);
  [[nodiscard]] static bool
  ClassOperatorIsRegistered(const State &Owner, std::string_view QualifiedName,
                            ClassOperator Selected);
  [[nodiscard]] static std::optional<std::string>
  ClassOperatorSegment(const State &Owner, std::string_view QualifiedName,
                       ClassOperator Selected);

  [[nodiscard]] static std::vector<ClassBaseView>
  ClassBases(const State &Owner, std::string_view QualifiedName);
  [[nodiscard]] static std::vector<ClassCastView>
  ClassCasts(const State &Owner, std::string_view QualifiedName);
  [[nodiscard]] static std::vector<ClassInheritedMemberView>
  ClassInheritedMembers(const State &Owner, std::string_view QualifiedName);

  [[nodiscard]] static ClassMemberAccessObservation
  ReadClassMemberValue(State &Owner, const ClassMemberAccessRequest &Request);

  [[nodiscard]] static ClassMemberAccessObservation
  WriteClassMemberValue(State &Owner, const ClassMemberAccessRequest &Request);

  [[nodiscard]] static std::size_t
  InvalidateClassMemberCache(State &Owner, const std::string &Path);

  [[nodiscard]] static std::optional<MemberDispatchObservation>
  ObserveLastClassMemberDispatch(const State &Owner);
  static void ClearClassMemberDispatch(State &Owner) noexcept;

  [[nodiscard]] static std::size_t
  LazyMemberCacheNodeCount(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  LazyMemberCacheEntryCount(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  LiveLazyMemberCacheEntryCount(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  LazyMemberCacheEntryCountOf(const State &Owner, const void *Storage) noexcept;
  [[nodiscard]] static std::uint64_t
  LazyMemberCacheGenerationOf(const State &Owner, const void *Storage) noexcept;
  [[nodiscard]] static LazyCacheCounters
  LazyMemberCacheCounters(const State &Owner) noexcept;
  static void ResetLazyMemberCacheCounters(State &Owner) noexcept;

  [[nodiscard]] static bool
  ClassUserdataNamesLazyEntries(const State &Owner, const std::string &Path);
  [[nodiscard]] static std::uint64_t
  ClassUserdataLazyGeneration(const State &Owner, const std::string &Path);

  [[nodiscard]] static std::optional<UserdataHeader>
  ObserveClassUserdata(const State &Owner, const std::string &Path);

  [[nodiscard]] static JoinedSubmissionReport
  SubmitJoinedFunctions(State &Owner,
                        std::vector<JoinedFunctionDeclaration> Declarations,
                        bool IgnoreNestedFailures);

  [[nodiscard]] static JoinedSubmissionReport
  PublishJoinedFunctions(State &Owner,
                         std::vector<JoinedFunctionDeclaration> Declarations,
                         bool IgnoreNestedFailures);

  [[nodiscard]] static CallbackBoundaryObservation
  SubmitThroughCallback(State &Owner,
                        std::vector<JoinedFunctionDeclaration> Declarations,
                        std::size_t ThrowAfterSubmissions,
                        bool ThrowStandardException, bool PublishWhenComplete);

  [[nodiscard]] static std::optional<std::string>
  ObserveVmPathValueKind(State &Owner, const std::string &Path) noexcept;

  [[nodiscard]] static PublicationObservation
  ProbeInstallationJournal(State &Owner, const std::vector<std::string> &Paths,
                           const std::vector<InstallationScope> &Overlays,
                           bool RestoreInsteadOfCommit);

  [[nodiscard]] static LifecycleSubject
  DescribeLifecycleSubject(const State &Owner);

  [[nodiscard]] static LifecycleAnalysis
  AnalyzeLifecycleRequest(const State &Owner, const LifecycleRequest &Request);

  [[nodiscard]] static LifecycleAttemptObservation
  PrepareLifecycleAttempt(State &Owner, const LifecycleAttempt &Attempt);

  [[nodiscard]] static LifecycleCommitObservation
  PublishLifecycleAttempt(State &Owner, const LifecycleCommitAttempt &Request);

  static void InjectFault(State &Owner, StateFaultPoint Point,
                          std::size_t Count = 1) noexcept;
  [[nodiscard]] static bool ConsumeFault(State &Owner,
                                         StateFaultPoint Point) noexcept;
  [[nodiscard]] static std::size_t
  PendingFaults(const State &Owner, StateFaultPoint Point) noexcept;
};

} // namespace Luna::Detail
