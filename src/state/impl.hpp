#pragma once

// clang-format off
#include <luna/binding/module_registration.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/state/state.hpp>

#include "state/binding/state_handle.hpp"
#include "state/freeze/cache.hpp"
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
#include "state/transaction/capture.hpp"
#include "state/transaction/generation_set.hpp"
#include "state/transaction/installation.hpp"
#include "state/transaction/lifecycle.hpp"
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

  // Destruction is explicitly ordered: no new invocation is accepted, then the
  // virtual machine closes and finalizes while every piece of cleanup metadata
  // is still valid, then whatever the machine never held is released, and only
  // then does the collection route die.
  ~Impl();

  [[nodiscard]] bool IsReady() const noexcept;

  // The construction thread remains the owner for the implementation's whole
  // lifetime. Because moves transfer this implementation, they preserve this
  // affinity instead of adopting the thread that performs the move.
  [[nodiscard]] bool IsOwnerThread() const noexcept {
    return Lifecycle.IsOwnerThread();
  }

  [[nodiscard]] RegistrationResult
  RegisterErased(std::string_view GlobalName,
                 ErasedCallableDescriptor &&Descriptor);
  [[nodiscard]] ExecutionResult Execute(std::string_view Source);
  [[nodiscard]] ReflectionSnapshot CaptureReflection() const;
  [[nodiscard]] RegistrationResult Freeze();

  // Submits one complete builder plan as a single outermost transaction: every
  // staged namespace, constant, and enumeration becomes visible together or
  // none does. A namespace that is already committed and still Luna-owned is
  // reopened without contributing a second symbol; every other occupant of its
  // path is a collision.
  [[nodiscard]] RegistrationResult
  RegisterBuilderPlan(const Detail::BuilderPlan &Plan,
                      const std::optional<ErrorDiagnostic> &StagedFailure);

  // Records one available module definition. Availability is Luna-side metadata
  // only: no callback runs, no virtual-machine path is touched, and nothing is
  // published, so a later load can still execute the whole resolved graph
  // inside one outermost transaction.
  [[nodiscard]] RegistrationResult
  ProvideModuleDefinition(ModuleManifest Manifest,
                          Detail::ModuleRegistration Registration);

  // Loads the module graph rooted at one requested manifest as one outermost
  // transaction: dependency callbacks and the requested callback run
  // dependency-first in canonical order, and the selected graph, its symbols,
  // records, and dispatch targets publish atomically or not at all.
  [[nodiscard]] RegistrationResult
  RegisterModuleGraph(ModuleManifest Manifest,
                      Detail::ModuleRegistration Registration);

  // The loaded module graph and the available definitions, for private
  // observation. Public consumers observe modules through reflection.
  [[nodiscard]] const Detail::ModuleRegistry &LoadedModules() const noexcept {
    return Modules;
  }

  [[nodiscard]] std::size_t AvailableModuleCount() const noexcept {
    return Definitions.Count();
  }

  // The private Luna ownership identity of every committed namespace table.
  [[nodiscard]] const Detail::NamespaceOwnershipTable &
  NamespaceOwnerships() const noexcept {
    return Namespaces;
  }

  // The registered classes of this logical State, each with its one canonical
  // type, class symbol, and cached metatable identity. Public consumers observe
  // classes only through reflection.
  [[nodiscard]] const Detail::ClassRegistry &
  RegisteredClasses() const noexcept {
    return Classes;
  }

  // The state-local nonce source every native identity of this State is paired
  // with, so recycled storage can never impersonate a released object.
  [[nodiscard]] Detail::NativeIdentitySource &NativeIdentities() noexcept {
    return NativeIdentityNonces;
  }

  // The per-State native identity cache. It decides whether a re-exposed object
  // is handed back as the value it already has or refused, and it is emptied of
  // an entry before that entry's payload is released.
  [[nodiscard]] Detail::UserdataIdentityCache &NativeIdentityCache() noexcept {
    return Identities;
  }

  [[nodiscard]] const Detail::UserdataIdentityCache &
  NativeIdentityCache() const noexcept {
    return Identities;
  }

  // The access context every validated native access resolves through,
  // refreshed against this implementation's live identity, class registry, and
  // identity cache. Its address is stable across every move of its State, which
  // is what lets one Luna-private virtual-machine slot name it.
  //
  // A destroying State withdraws the identity every access validates against
  // before its machine closes, so no new access can start while cleanup runs.
  [[nodiscard]] Detail::UserdataAccessContext &UserdataAccess() noexcept {
    AccessContext.Origin =
        Destroying ? Detail::StateIdentity{} : Lifecycle.Identity();
    AccessContext.Classes = &Classes;
    AccessContext.Cache = &Identities;
    AccessContext.Lazy = &LazyValues;

    // What a member access reached through the virtual machine needs beyond the
    // gate: the immutable type generation it captures at entry, the fault
    // context every callback-stack restoration is recorded through, the
    // recorder that keeps the last dispatch observable, and the generation a
    // cached value belongs to. Callable dispatch already resolves through
    // stable slots, but a cached member value is still keyed by the lifecycle
    // generation until the dynamic lifecycle milestone retires values itself.
    AccessContext.Types = &Bindings.Types();
    AccessContext.Faults = &Faults;
    AccessContext.Dispatch = &MemberDispatch;
    AccessContext.DispatchGeneration = Lifecycle.Generation();
    return AccessContext;
  }

  // What the most recent member access taken through the virtual machine did.
  // Only Luna can observe it; a consumer sees the diagnostic it produced.
  [[nodiscard]] Detail::MemberDispatchRecorder &
  MemberDispatchObservations() noexcept {
    return MemberDispatch;
  }

  [[nodiscard]] const Detail::MemberDispatchRecorder &
  MemberDispatchObservations() const noexcept {
    return MemberDispatch;
  }

  // The lazy value cache of this State. A lazy property records only a value
  // its getter already produced, for one exposed object, under one dispatch
  // generation; retiring or releasing that object drops its entries first.
  [[nodiscard]] Detail::LazyPropertyCache &LazyMemberValues() noexcept {
    return LazyValues;
  }

  [[nodiscard]] const Detail::LazyPropertyCache &
  LazyMemberValues() const noexcept {
    return LazyValues;
  }

  // The exposure context the write path resolves through, refreshed against
  // this implementation's live identity, class registry, identity cache, nonce
  // source, and release gate. Its address is stable across every move of its
  // State, exactly like the access context it complements.
  //
  // A destroying State withdraws the identity every exposure is published under
  // before its machine closes, so no new value can come into existence while
  // cleanup runs.
  [[nodiscard]] Detail::UserdataExposureContext &UserdataExposure() noexcept {
    ExposureContext.Origin =
        Destroying ? Detail::StateIdentity{} : Lifecycle.Identity();
    ExposureContext.Classes = &Classes;
    ExposureContext.Cache = &Identities;
    ExposureContext.Nonces = &NativeIdentityNonces;
    ExposureContext.Ownership = &Userdata;
    ExposureContext.Access = &UserdataAccess();

    // The generation one exposed value is published under stays the lifecycle
    // generation of its publication; callable dispatch generations are keyed
    // separately by the dispatch table.
    ExposureContext.DispatchGeneration = Lifecycle.Generation();
    return ExposureContext;
  }

  // The one ownership and release gate of this State. Every exposure,
  // invalidation, collection, lifecycle action, and destruction of a value goes
  // through it, so no value is ever released twice and none is ever missed.
  [[nodiscard]] Detail::OwnershipRegistry &UserdataOwnership() noexcept {
    return Userdata;
  }

  [[nodiscard]] const Detail::OwnershipRegistry &
  UserdataOwnership() const noexcept {
    return Userdata;
  }

  // The shared liveness token a builder captures. It dies with this
  // implementation, so a builder can detect the destruction of its owner
  // instead of dereferencing it.
  [[nodiscard]] const std::shared_ptr<Detail::StateHandleToken> &
  HandleToken() const noexcept {
    return Handle;
  }

  // A new owner object holds this implementation. The token records which
  // object that is, together with the identity and epoch of that ownership.
  void AdoptOwner(State &Object) noexcept;

  [[nodiscard]] bool IsFrozen() const noexcept { return Lifecycle.IsFrozen(); }

  [[nodiscard]] const std::shared_ptr<const Detail::FreezeCacheStorage> &
  FrozenCache() const noexcept {
    return FrozenCaches;
  }

  // The immutable generation set every ordinary query observes. Publication
  // replaces the whole set at once; nothing else mutates it.
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

  // The published access context must be refreshed after the generation moves.
  // Lazy values remain owned but become unreachable by generation mismatch;
  // the identity cache continues to describe the still-live userdata itself.
  void AdvanceLifecycleGeneration() noexcept {
    Lifecycle.AdvanceGeneration();
    AccessContext.DispatchGeneration = Lifecycle.Generation();
  }

private:
  friend class Detail::StateTestHooks;

  // Phase one of every transaction: owner thread, readiness and freeze phase,
  // entry stack depth, logical identity with its epochs, and the committed
  // generation set, all read exactly once.
  [[nodiscard]] Detail::TransactionCapture CaptureTransactionEntry() const;

  // Records one failure on the active transaction and returns it to the caller.
  // An outer submission rolls its own transaction back; a nested submission
  // poisons the outer one instead, so an ignored nested result still prevents
  // publication.
  [[nodiscard]] RegistrationResult
  ReportSubmissionFailure(const Detail::ActiveTransactionScope &Scope,
                          ErrorDiagnostic Diagnostic);

  // Validates and prepares one function declaration inside the active
  // transaction. It never installs and never publishes.
  [[nodiscard]] RegistrationResult
  SubmitFunctionDeclaration(const Detail::ActiveTransactionScope &Scope,
                            std::string_view GlobalName,
                            ErasedCallableDescriptor &&Descriptor,
                            Detail::PreparedSubmission &Prepared);

  // Validates and prepares one namespace declaration inside the active
  // transaction. A namespace that is already committed and still Luna-owned is
  // reopened, which contributes no declaration and installs nothing.
  [[nodiscard]] RegistrationResult
  SubmitNamespaceDeclaration(const Detail::ActiveTransactionScope &Scope,
                             const Detail::StagedNamespace &Declaration);

  // Validates and prepares one scoped function declaration inside the active
  // transaction. It routes through exactly the canonical descriptor builder,
  // validation, preparation, and staged callable target a root-scope
  // `Register`/`RegisterFunction` request uses; only the qualified name, the
  // parent scope, and the general precedence profile differ.
  [[nodiscard]] RegistrationResult
  SubmitScopedFunctionDeclaration(const Detail::ActiveTransactionScope &Scope,
                                  const Detail::StagedFunction &Declaration);

  // Validates and prepares one constant declaration inside the active
  // transaction. The value is staged, never installed here: the canonical
  // writer runs during protected installation.
  [[nodiscard]] RegistrationResult
  SubmitConstantDeclaration(const Detail::ActiveTransactionScope &Scope,
                            const Detail::StagedConstant &Declaration);

  // Validates and prepares one enumeration declaration inside the active
  // transaction: the enumeration scope with its canonical type, and one
  // reflection record per canonical enumerator and per alias.
  [[nodiscard]] RegistrationResult
  SubmitEnumerationDeclaration(const Detail::ActiveTransactionScope &Scope,
                               const Detail::StagedEnumeration &Declaration);

  // Validates and prepares one class declaration inside the active transaction:
  // the class symbol with its one canonical class type, and the cached
  // metatable identity that type owns in this logical State. Nothing is
  // installed and nothing is registered as a class until the outermost
  // transaction publishes.
  [[nodiscard]] RegistrationResult
  SubmitClassDeclaration(const Detail::ActiveTransactionScope &Scope,
                         const Detail::StagedClass &Declaration,
                         const Detail::RelationshipCandidate &Relationships);

  // The candidate class relationship graph of one attempt: every committed
  // class with its published edges, plus every class and edge the plan
  // declares. It is assembled before one class of the plan is submitted, so the
  // outcome never depends on the order the classes were declared in.
  [[nodiscard]] Detail::RelationshipCandidate
  BuildRelationshipCandidate(const Detail::BuilderPlan &Plan) const;

  // Records the relationships of one published attempt. It runs after every
  // class of the attempt is registered, because an edge needs the metatable
  // identity of both of its classes.
  void
  RecordPublishedRelationships(const Detail::RegistrationTransaction &Active);

  // Validates and prepares one construction candidate of a class inside the
  // active transaction. It is submitted exactly like an ordinary callable
  // candidate - same category, same canonical overload grouping, same
  // validation precedence, same protected closure preparation - parented at the
  // class scope the same transaction just planned.
  [[nodiscard]] RegistrationResult
  SubmitConstructionDeclaration(const Detail::ActiveTransactionScope &Scope,
                                const Detail::StagedClass &Class,
                                const SymbolId &ClassSymbol,
                                const Detail::StagedConstruction &Declaration);

  // Validates and prepares one member candidate of a class inside the active
  // transaction: one instance method or one static method. Both are submitted
  // exactly like an ordinary callable candidate, parented at the class scope
  // the same transaction just planned; the receiver of an instance method
  // travels in its canonical signature, so it is what tells the two apart
  // everywhere.
  [[nodiscard]] RegistrationResult
  SubmitMemberDeclaration(const Detail::ActiveTransactionScope &Scope,
                          const Detail::StagedClass &Class,
                          const SymbolId &ClassSymbol,
                          const Detail::StagedMethod &Declaration,
                          const Detail::RelationshipCandidate &Relationships);

  // Validates and prepares one typed accessor of a class inside the active
  // transaction: one property or one field. It is one reflected symbol with two
  // generated descriptors rather than a callable candidate per direction, so it
  // installs no virtual-machine value of its own; the deterministic member
  // collision order decides whether its name is available at all.
  [[nodiscard]] RegistrationResult
  SubmitAccessorDeclaration(const Detail::ActiveTransactionScope &Scope,
                            const Detail::StagedClass &Class,
                            const SymbolId &ClassSymbol,
                            const Detail::StagedMember &Declaration,
                            const Detail::RelationshipCandidate &Relationships);

  // Validates and prepares one operator of a class inside the active
  // transaction. It is submitted exactly like an instance method candidate, so
  // an operator resolves through the same receiver validation and the same
  // overload rules; its qualified name is the Luna-owned segment the operator
  // names, which is why no consumer declaration can ever occupy it.
  [[nodiscard]] RegistrationResult
  SubmitOperatorDeclaration(const Detail::ActiveTransactionScope &Scope,
                            const Detail::StagedClass &Class,
                            const SymbolId &ClassSymbol,
                            const Detail::StagedOperator &Declaration);

  // The parent scope of one declaration, resolved against the committed and
  // pending symbols of the active transaction plus Luna's private namespace
  // ownership. A root-scope declaration always resolves to the root scope.
  [[nodiscard]] Detail::ParentScopeResolution
  ResolveParentScope(const Detail::RegistrationTransaction &Active,
                     std::string_view ParentName) const;

  // Submits every staged declaration of one builder plan into the active
  // transaction: scopes first, then enumerations, then constants, then module
  // loads. A module callback's own plan is submitted the same way, so a module
  // requested inside a builder joins exactly one outermost transaction.
  [[nodiscard]] RegistrationResult
  SubmitPlanDeclarations(const Detail::ActiveTransactionScope &Scope,
                         const Detail::BuilderPlan &Plan,
                         const std::optional<ErrorDiagnostic> &StagedFailure);

  // Resolves one requested module graph and submits every not-yet-loaded module
  // of it, dependency-first in canonical order, into the active transaction.
  [[nodiscard]] RegistrationResult
  SubmitModuleLoad(const Detail::ActiveTransactionScope &Scope,
                   const Detail::StagedModule &Request);

  // Runs one module's registration callback behind the private callback
  // boundary, submits everything it staged into the active transaction, and
  // plans the module's own declaration. Nothing the callback throws crosses
  // this boundary: the attempt is poisoned and reports one deterministic
  // failure.
  [[nodiscard]] RegistrationResult
  SubmitModuleCallback(const Detail::ActiveTransactionScope &Scope,
                       const ModuleManifest &Manifest,
                       const Detail::ModuleRegistration &Registration,
                       std::string_view ParentQualifiedName,
                       const Detail::ModuleResolution &Resolution);

  // The module definitions available to resolution right now: everything this
  // State already owns plus everything the open attempt provided.
  [[nodiscard]] Detail::ModuleCatalog CandidateModuleCatalog() const;

  [[nodiscard]] const Detail::ModuleDefinition *
  FindModuleDefinition(std::string_view Identity,
                       const SemanticVersion &Version) const;

  // The manifest one still unpublished module load of the open attempt planned,
  // or null when the attempt has not planned that identity.
  [[nodiscard]] const ModuleManifest *
  FindPendingModule(std::string_view Identity) const noexcept;

  // Publication and rollback of the module half of one attempt. Loaded modules
  // and newly available definitions become visible only in the publication step
  // of the outermost transaction.
  void PublishPendingModules();
  void DiscardPendingModules() noexcept;

  // Abandons one outermost attempt that failed before installation: every
  // staged resource and every pending module effect is discarded, so the next
  // attempt starts from the exact committed model. A nested submission abandons
  // nothing: its outer transaction owns that decision.
  void AbandonAttempt(const Detail::ActiveTransactionScope &Scope) noexcept;

  // The first deterministic failure of one planned member of a scope Luna is
  // declaring in the same attempt, such as an enumerator of a new enumeration.
  [[nodiscard]] std::optional<ErrorDiagnostic>
  CheckPlannedMember(const Detail::ActiveTransactionScope &Scope,
                     const Detail::DescriptorPlanEntry &Member);

  // Prepares the replacement immutable stores of the attempt so far. Nothing is
  // installed and nothing is published.
  [[nodiscard]] RegistrationResult
  PrepareSubmittedDeclarations(const Detail::ActiveTransactionScope &Scope,
                               const std::string &Subject);

  // Records the private Luna ownership identity of every namespace table the
  // published plan installed. It runs only after installation and the internal
  // consistency check accepted the attempt.
  void RecordPublishedNamespaces(const Detail::RegistrationTransaction &Active);

  // Records every class the published plan declared, together with the cached
  // metatable identity it owns in this logical State. It runs only after
  // installation and the internal consistency check accepted the attempt, so a
  // rolled-back attempt registers no class and issues no metatable identity.
  void RecordPublishedClasses(const Detail::RegistrationTransaction &Active);

  // Phases four and five of the outermost transaction: protected installation
  // behind the undo journal, the internal consistency check, and atomic
  // publication of the complete virtual machine and generation set. Any failure
  // restores every journalled path in reverse order, discards every overlay,
  // returns the root stack to its captured entry depth, and leaves the
  // committed model exactly as it was.
  [[nodiscard]] RegistrationResult
  CompleteOutermostTransaction(const Detail::ActiveTransactionScope &Scope,
                               Detail::PublicationObservation *Observed);

  // Discards every staged, still uncommitted resource of the plan in reverse
  // canonical order. Used for resources the journal never reached.
  void DiscardStagedResources(
      const Detail::RegistrationTransaction &Active) noexcept;

  // Submits several function declarations as nested submissions of one
  // outermost transaction, the way a builder or module callback will. When
  // `PublishWhenComplete` is false the group stops before publication and
  // discards every staged resource; when it is true the group installs and
  // publishes as one atomic unit or restores everything.
  [[nodiscard]] Detail::JoinedSubmissionReport SubmitJoinedFunctionDeclarations(
      std::vector<Detail::JoinedFunctionDeclaration> Declarations,
      bool IgnoreNestedFailures, bool PublishWhenComplete);

  // The same group, submitted behind the private callback boundary. The
  // callback throws after `ThrowAfterSubmissions` declarations have joined the
  // outer transaction; a count past the end never throws. Nothing thrown may
  // cross the boundary: the attempt is poisoned, every staged resource is
  // discarded, and one deterministic internal failure is reported instead.
  //
  // While the attempt is still in flight the observation records what every
  // ordinary query sees, including an owning public reflection snapshot taken
  // on this thread and on another one.
  [[nodiscard]] Detail::CallbackBoundaryObservation
  SubmitJoinedFunctionsThroughCallback(
      std::vector<Detail::JoinedFunctionDeclaration> Declarations,
      std::size_t ThrowAfterSubmissions, bool ThrowStandardException,
      bool PublishWhenComplete);

  // Destruction is reverse declaration order: the VM closes before records die.
  // Reflection generations and the committed generation set are declared first
  // because they are immutable, hold no VM resource, and may still be shared by
  // snapshots after this State dies.
  Detail::ReflectionDatabase Reflection;
  std::shared_ptr<const Detail::GenerationSet> Generations =
      Detail::GenerationSet::Initial();

  // Published only by a successful freeze. The cache owns immutable values and
  // stable identities for exactly the generation set named by its key.
  std::shared_ptr<const Detail::FreezeCacheStorage> FrozenCaches;

  // Logical State identity, owner thread, lifecycle phase, owner-object epoch,
  // and lifecycle generation.
  Detail::StateLifecycle Lifecycle;

  // Set as the very first step of destruction, before anything is closed or
  // released. From here on the State is not ready, every userdata access and
  // exposure refuses, and no new invocation can start.
  bool Destroying = false;

  // The one active outer transaction. Nested builder, module, and registration
  // callbacks join it instead of committing independently.
  Detail::RegistrationTransaction *ActiveTransaction = nullptr;

  // Private stores publication and rollback operate on.
  Detail::BindingStore Bindings;

  // Which namespace tables Luna owns, and with which identity. It is written
  // only by publication, so a rolled-back attempt leaves no ownership behind.
  Detail::NamespaceOwnershipTable Namespaces;

  // The registered classes of this State and the state-local nonce source their
  // exposed values pair their native identity with. The class registry is
  // written only by publication, so a rolled-back attempt registers no class.
  Detail::ClassRegistry Classes;
  Detail::NativeIdentitySource NativeIdentityNonces;

  // The native identity cache of this State and the access context that names
  // it. Both are declared before the virtual machine so they outlive it: the
  // machine finalizes first, and every cache entry stays observable while the
  // values it described are released.
  Detail::UserdataIdentityCache Identities;
  Detail::UserdataAccessContext AccessContext;
  Detail::UserdataExposureContext ExposureContext;

  // The lazy value cache of this State. It is declared before the virtual
  // machine so it outlives it: the machine finalizes first, and every entry is
  // dropped before the payload it was produced from is released.
  Detail::LazyPropertyCache LazyValues;

  // What the last member access taken through the virtual machine did. It is
  // declared before the machine so a dispatch can still record while the
  // machine is finalizing.
  Detail::MemberDispatchRecorder MemberDispatch;

  // The ownership and release gate of every value this State exposed. It is
  // declared before the virtual machine so it is destroyed after it: the
  // machine finalizes first, and every remaining value is then released exactly
  // once while its type, allocator, metatable, and dispatch metadata are still
  // valid.
  Detail::OwnershipRegistry Userdata;

  // The module definitions available to resolution and the load-once registry
  // of modules this State has loaded. Both are written only by publication, so
  // a rolled-back load leaves the exact pre-load module state behind.
  Detail::ModuleDefinitionLibrary Definitions;
  Detail::ModuleRegistry Modules;

  // The module half of the open attempt: definitions it provided and modules it
  // loaded. Publication moves them into the committed stores; rollback discards
  // them.
  std::vector<Detail::ModuleDefinition> PendingDefinitions;
  std::vector<ModuleManifest> PendingModules;

  // The shared liveness token every builder handle captures.
  std::shared_ptr<Detail::StateHandleToken> Handle;

  Detail::FaultInjector Faults;
  Detail::VirtualMachineOwner VirtualMachine;
};

} // namespace Luna
