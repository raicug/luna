#pragma once

// clang-format off
#include <luna/binding/module_registration.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/state/state.hpp>

#include "state/binding/state_handle.hpp"
#include "state/freeze/cache.hpp"
#include "state/invocation/async/suspended_call.hpp"
#include "state/invocation/delegate/vm_delegate.hpp"
#include "state/invocation/parameters/vm_userdata_capture.hpp"
#include "state/vm/enum_item.hpp"
#include "state/module/load.hpp"
#include "state/module/registry.hpp"
#include "state/module/resolution.hpp"
#include "state/reflection/database.hpp"
#include "state/registration/class_plan.hpp"
#include "state/registration/function_plan.hpp"
#include "state/registration/relationship_plan.hpp"
#include "state/registration/scope_plan.hpp"
#include "state/registration/store.hpp"
#include "state/registration/submission.hpp"
#include "state/registration/value_plan.hpp"
#include "state/testing/fault_injector.hpp"
#include "state/tooling/profiling_registry.hpp"
#include "state/transaction/capture.hpp"
#include "state/transaction/generation_set.hpp"
#include "state/transaction/installation.hpp"
#include "state/transaction/lifecycle.hpp"
#include "state/transaction/lifecycle_publication.hpp"
#include "state/transaction/lifecycle_staging.hpp"
#include "state/transaction/transaction.hpp"
#include "state/userdata/access.hpp"
#include "state/userdata/class_registry.hpp"
#include "state/userdata/collection.hpp"
#include "state/userdata/identity.hpp"
#include "state/userdata/identity_cache.hpp"
#include "state/userdata/lazy_cache.hpp"
#include "state/userdata/member_access.hpp"
#include "state/userdata/member_dispatch.hpp"
#include "state/userdata/ownership.hpp"
#include "state/userdata/value_exposure.hpp"
#include "state/vm/owner.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna {

class State::Impl final {
public:
  Impl();

  ~Impl();

  [[nodiscard]] bool IsReady() const noexcept;

  [[nodiscard]] bool IsOwnerThread() const noexcept {
    return Lifecycle.IsOwnerThread();
  }

  [[nodiscard]] RegistrationResult
  RegisterErased(std::string_view GlobalName,
                 ErasedCallableDescriptor &&Descriptor);
  [[nodiscard]] ExecutionResult Execute(std::string_view Source);
  [[nodiscard]] ReflectionSnapshot CaptureReflection() const;
  [[nodiscard]] RegistrationResult Freeze();

  [[nodiscard]] RegistrationResult
  RegisterBuilderPlan(const Detail::BuilderPlan &Plan,
                      const std::optional<ErrorDiagnostic> &StagedFailure);

  [[nodiscard]] RegistrationResult
  ProvideModuleDefinition(ModuleManifest Manifest,
                          Detail::ModuleRegistration Registration);

  [[nodiscard]] RegistrationResult
  RegisterModuleGraph(ModuleManifest Manifest,
                      Detail::ModuleRegistration Registration);

  [[nodiscard]] const Detail::ModuleRegistry &LoadedModules() const noexcept {
    return Modules;
  }

  [[nodiscard]] std::size_t AvailableModuleCount() const noexcept {
    return Definitions.Count();
  }

  [[nodiscard]] const Detail::NamespaceOwnershipTable &
  NamespaceOwnerships() const noexcept {
    return Namespaces;
  }

  [[nodiscard]] const Detail::ClassRegistry &
  RegisteredClasses() const noexcept {
    return Classes;
  }

  [[nodiscard]] Detail::NativeIdentitySource &NativeIdentities() noexcept {
    return NativeIdentityNonces;
  }

  [[nodiscard]] Detail::UserdataIdentityCache &NativeIdentityCache() noexcept {
    return Identities;
  }

  [[nodiscard]] const Detail::UserdataIdentityCache &
  NativeIdentityCache() const noexcept {
    return Identities;
  }

  [[nodiscard]] Detail::UserdataAccessContext &UserdataAccess() noexcept {
    AccessContext.Origin =
        Destroying ? Detail::StateIdentity{} : Lifecycle.Identity();
    AccessContext.Classes = &Classes;
    AccessContext.Cache = &Identities;
    AccessContext.Lazy = &LazyValues;

    AccessContext.Types = &Bindings.Types();
    AccessContext.Faults = &Faults;
    AccessContext.Dispatch = &MemberDispatch;
    AccessContext.DispatchGeneration = Lifecycle.Generation();
    return AccessContext;
  }

  [[nodiscard]] Detail::MemberDispatchRecorder &
  MemberDispatchObservations() noexcept {
    return MemberDispatch;
  }

  [[nodiscard]] const Detail::MemberDispatchRecorder &
  MemberDispatchObservations() const noexcept {
    return MemberDispatch;
  }

  [[nodiscard]] Detail::LazyPropertyCache &LazyMemberValues() noexcept {
    return LazyValues;
  }

  [[nodiscard]] const Detail::LazyPropertyCache &
  LazyMemberValues() const noexcept {
    return LazyValues;
  }

  [[nodiscard]] Detail::UserdataExposureContext &UserdataExposure() noexcept {
    ExposureContext.Origin =
        Destroying ? Detail::StateIdentity{} : Lifecycle.Identity();
    ExposureContext.Classes = &Classes;
    ExposureContext.Cache = &Identities;
    ExposureContext.Nonces = &NativeIdentityNonces;
    ExposureContext.Ownership = &Userdata;
    ExposureContext.Access = &UserdataAccess();

    ExposureContext.DispatchGeneration = Lifecycle.Generation();
    return ExposureContext;
  }

  [[nodiscard]] Detail::OwnershipRegistry &UserdataOwnership() noexcept {
    return Userdata;
  }

  [[nodiscard]] const Detail::OwnershipRegistry &
  UserdataOwnership() const noexcept {
    return Userdata;
  }

  [[nodiscard]] const std::shared_ptr<Detail::StateHandleToken> &
  HandleToken() const noexcept {
    return Handle;
  }

  void AdoptOwner(State &Object) noexcept;

  [[nodiscard]] bool IsFrozen() const noexcept { return Lifecycle.IsFrozen(); }

  [[nodiscard]] const std::shared_ptr<const Detail::FreezeCacheStorage> &
  FrozenCache() const noexcept {
    return FrozenCaches;
  }

  [[nodiscard]] std::shared_ptr<const Detail::GenerationSet>
  CurrentGenerations() const noexcept {
    return Generations;
  }

  [[nodiscard]] Detail::StateIdentity LogicalIdentity() const noexcept {
    return Lifecycle.Identity();
  }

  [[nodiscard]] std::uint64_t OwnerEpoch() const noexcept {
    return Lifecycle.OwnerEpoch();
  }

  [[nodiscard]] std::uint64_t LifecycleGeneration() const noexcept {
    return Lifecycle.Generation();
  }

  void AdvanceOwnerEpoch() noexcept;

  void AdvanceLifecycleGeneration() noexcept {
    Lifecycle.AdvanceGeneration();
    AccessContext.DispatchGeneration = Lifecycle.Generation();
    static_cast<void>(Delegates.InvalidateEverything());
    static_cast<void>(UserdataCaptures.InvalidateEverything());
  }

  [[nodiscard]] Detail::VmDelegateRegistry &SubscribedHandlers() noexcept {
    return Delegates;
  }

  [[nodiscard]] const Detail::VmDelegateRegistry &
  SubscribedHandlers() const noexcept {
    return Delegates;
  }

  [[nodiscard]] Detail::VmUserdataCaptureRegistry &
  CapturedUserdataValues() noexcept {
    return UserdataCaptures;
  }

  [[nodiscard]] const Detail::VmUserdataCaptureRegistry &
  CapturedUserdataValues() const noexcept {
    return UserdataCaptures;
  }

  [[nodiscard]] RegistrationResult InstallProfilingHook(ProfilingHook Hook);
  [[nodiscard]] RegistrationResult ClearProfilingHook();

  [[nodiscard]] Detail::ProfilingRegistry &Profiling() noexcept {
    return ProfilingHooks;
  }

private:
  friend class Detail::StateTestHooks;

  [[nodiscard]] Detail::TransactionCapture CaptureTransactionEntry() const;

  [[nodiscard]] RegistrationResult
  ReportSubmissionFailure(const Detail::ActiveTransactionScope &Scope,
                          ErrorDiagnostic Diagnostic);

  [[nodiscard]] RegistrationResult
  SubmitFunctionDeclaration(const Detail::ActiveTransactionScope &Scope,
                            std::string_view GlobalName,
                            ErasedCallableDescriptor &&Descriptor,
                            Detail::PreparedSubmission &Prepared);

  [[nodiscard]] RegistrationResult
  SubmitNamespaceDeclaration(const Detail::ActiveTransactionScope &Scope,
                             const Detail::StagedNamespace &Declaration);

  [[nodiscard]] RegistrationResult
  SubmitScopedFunctionDeclaration(const Detail::ActiveTransactionScope &Scope,
                                  const Detail::StagedFunction &Declaration);

  [[nodiscard]] RegistrationResult
  SubmitConstantDeclaration(const Detail::ActiveTransactionScope &Scope,
                            const Detail::StagedConstant &Declaration);

  [[nodiscard]] RegistrationResult
  SubmitEnumerationDeclaration(const Detail::ActiveTransactionScope &Scope,
                               const Detail::StagedEnumeration &Declaration);

  [[nodiscard]] RegistrationResult
  SubmitClassDeclaration(const Detail::ActiveTransactionScope &Scope,
                         const Detail::StagedClass &Declaration,
                         const Detail::RelationshipCandidate &Relationships);

  [[nodiscard]] Detail::RelationshipCandidate
  BuildRelationshipCandidate(const Detail::BuilderPlan &Plan) const;

  void
  RecordPublishedRelationships(const Detail::RegistrationTransaction &Active);

  [[nodiscard]] RegistrationResult
  SubmitConstructionDeclaration(const Detail::ActiveTransactionScope &Scope,
                                const Detail::StagedClass &Class,
                                const SymbolId &ClassSymbol,
                                const Detail::StagedConstruction &Declaration);

  [[nodiscard]] RegistrationResult
  SubmitMemberDeclaration(const Detail::ActiveTransactionScope &Scope,
                          const Detail::StagedClass &Class,
                          const SymbolId &ClassSymbol,
                          const Detail::StagedMethod &Declaration,
                          const Detail::RelationshipCandidate &Relationships);

  [[nodiscard]] RegistrationResult
  SubmitClassConstantDeclaration(const Detail::ActiveTransactionScope &Scope,
                                 const Detail::StagedClass &Class,
                                 const SymbolId &ClassSymbol,
                                 const Detail::StagedConstant &Declaration);

  [[nodiscard]] RegistrationResult
  SubmitAccessorDeclaration(const Detail::ActiveTransactionScope &Scope,
                            const Detail::StagedClass &Class,
                            const SymbolId &ClassSymbol,
                            const Detail::StagedMember &Declaration,
                            const Detail::RelationshipCandidate &Relationships);

  [[nodiscard]] RegistrationResult
  SubmitOperatorDeclaration(const Detail::ActiveTransactionScope &Scope,
                            const Detail::StagedClass &Class,
                            const SymbolId &ClassSymbol,
                            const Detail::StagedOperator &Declaration);

  [[nodiscard]] Detail::ParentScopeResolution
  ResolveParentScope(const Detail::RegistrationTransaction &Active,
                     std::string_view ParentName) const;

  [[nodiscard]] RegistrationResult
  SubmitPlanDeclarations(const Detail::ActiveTransactionScope &Scope,
                         const Detail::BuilderPlan &Plan,
                         const std::optional<ErrorDiagnostic> &StagedFailure);

  [[nodiscard]] RegistrationResult
  SubmitModuleLoad(const Detail::ActiveTransactionScope &Scope,
                   const Detail::StagedModule &Request);

  [[nodiscard]] RegistrationResult
  SubmitModuleCallback(const Detail::ActiveTransactionScope &Scope,
                       const ModuleManifest &Manifest,
                       const Detail::ModuleRegistration &Registration,
                       std::string_view ParentQualifiedName,
                       const Detail::ModuleResolution &Resolution);

  [[nodiscard]] Detail::ModuleCatalog CandidateModuleCatalog() const;

  [[nodiscard]] const Detail::ModuleDefinition *
  FindModuleDefinition(std::string_view Identity,
                       const SemanticVersion &Version) const;

  [[nodiscard]] const ModuleManifest *
  FindPendingModule(std::string_view Identity) const noexcept;

  void PublishPendingModules();
  void DiscardPendingModules() noexcept;

  void AbandonAttempt(const Detail::ActiveTransactionScope &Scope) noexcept;

  [[nodiscard]] std::optional<ErrorDiagnostic>
  CheckPlannedMember(const Detail::ActiveTransactionScope &Scope,
                     const Detail::DescriptorPlanEntry &Member);

  [[nodiscard]] RegistrationResult
  PrepareSubmittedDeclarations(const Detail::ActiveTransactionScope &Scope,
                               const std::string &Subject);

  void RecordPublishedNamespaces(const Detail::RegistrationTransaction &Active);

  void RecordPublishedClasses(const Detail::RegistrationTransaction &Active);

  [[nodiscard]] RegistrationResult
  CompleteOutermostTransaction(const Detail::ActiveTransactionScope &Scope,
                               Detail::PublicationObservation *Observed);

  void DiscardStagedResources(
      const Detail::RegistrationTransaction &Active) noexcept;

  [[nodiscard]] Detail::JoinedSubmissionReport SubmitJoinedFunctionDeclarations(
      std::vector<Detail::JoinedFunctionDeclaration> Declarations,
      bool IgnoreNestedFailures, bool PublishWhenComplete);

  [[nodiscard]] Detail::CallbackBoundaryObservation
  SubmitJoinedFunctionsThroughCallback(
      std::vector<Detail::JoinedFunctionDeclaration> Declarations,
      std::size_t ThrowAfterSubmissions, bool ThrowStandardException,
      bool PublishWhenComplete);

  [[nodiscard]] Detail::LifecycleSubject DescribeLifecycleSubject() const;

  [[nodiscard]] Detail::LifecycleAnalysis
  AnalyzeLifecycleRequest(const Detail::LifecycleRequest &Request) const;

  [[nodiscard]] Detail::LifecycleAttemptObservation
  PrepareLifecycleAttempt(const Detail::LifecycleAttempt &Attempt);

  [[nodiscard]] Detail::LifecycleCommitObservation
  PublishLifecycleAttempt(const Detail::LifecycleCommitAttempt &Request);

  Detail::ReflectionDatabase Reflection;
  std::shared_ptr<const Detail::GenerationSet> Generations =
      Detail::GenerationSet::Initial();

  std::shared_ptr<const Detail::FreezeCacheStorage> FrozenCaches;

  Detail::StateLifecycle Lifecycle;

  bool Destroying = false;

  Detail::RegistrationTransaction *ActiveTransaction = nullptr;

  Detail::BindingStore Bindings;

  Detail::NamespaceOwnershipTable Namespaces;

  Detail::ClassRegistry Classes;
  Detail::NativeIdentitySource NativeIdentityNonces;

  Detail::UserdataIdentityCache Identities;
  Detail::UserdataAccessContext AccessContext;
  Detail::UserdataExposureContext ExposureContext;

  Detail::LazyPropertyCache LazyValues;

  Detail::MemberDispatchRecorder MemberDispatch;

  Detail::OwnershipRegistry Userdata;

  Detail::ModuleDefinitionLibrary Definitions;
  Detail::ModuleRegistry Modules;

  std::vector<Detail::ModuleDefinition> PendingDefinitions;
  std::vector<ModuleManifest> PendingModules;

  std::shared_ptr<Detail::StateHandleToken> Handle;

  Detail::FaultInjector Faults;
  Detail::AsyncCallRegistry AsyncCalls;
  Detail::VmDelegateRegistry Delegates;
  Detail::VmUserdataCaptureRegistry UserdataCaptures;
  Detail::EnumItemRegistry EnumItems;
  Detail::ProfilingRegistry ProfilingHooks;
  Detail::VirtualMachineOwner VirtualMachine;
};

} // namespace Luna
