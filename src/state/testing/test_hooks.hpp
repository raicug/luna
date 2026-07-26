#pragma once

// clang-format off
#include <luna/binding/value.hpp>

#include "state/dispatch/generation.hpp"
#include "state/freeze/cache.hpp"
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

  // One canonical `|`-separated text per cached entry, in exactly the cached
  // order, so a test can compare cache contents and ordering with an
  // independent model and with the uncached lookups without reading Luna's
  // private storage layout and without any address ever leaving Luna.
  //
  //   LookupDetails: name|category|kind|symbol|reflection index or "-"
  //   OrderedOverloads: name|candidate count|lookup indices
  //   OrderedConversions: canonical type
  //   OrderedCastPaths: source|target|kind
  //   OrderedMetatables: name|type|class symbol
  //   OrderedNamespaces: name|scope symbol
  //   OrderedModules: identity|version|module symbol
  std::vector<std::string> LookupDetails;
  std::vector<std::string> OrderedOverloads;
  std::vector<std::string> OrderedConversions;
  std::vector<std::string> OrderedCastPaths;
  std::vector<std::string> OrderedMetatables;
  std::vector<std::string> OrderedNamespaces;
  std::vector<std::string> OrderedModules;

  // The state-local metatable identity of each cached metatable entry, in the
  // same order as `OrderedMetatables`. It is state-local rather than canonical,
  // so two equivalent States may issue different values for the same class.
  std::vector<std::uint64_t> MetatableIdentities;
};

// One object a test exposes as a value of one registered class.
struct ClassExposureRequest final {
  std::string QualifiedName;
  std::string Path;
  void *Storage = nullptr;
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  ConstAccess Access = ConstAccess::Mutable;

  // The explicit lifetime handle a borrowed value requires, modelled as one
  // generation counter the caller owns. The exposure records the generation it
  // reads here; advancing the counter is exactly what invalidation does, so
  // every later access is expired.
  const std::uint64_t *LifetimeGeneration = nullptr;
};

struct ClassExposureObservation final {
  // Deterministic outcome token: created, reused, conflicting_ownership,
  // incompatible_type, incompatible_access, missing_lifetime_handle, and so on.
  std::string Status;
  bool Created = false;
  bool Reused = false;

  // The state-local nonce the exposure recorded. It is not an address, and a
  // reused value keeps the nonce it was first exposed with.
  std::uint64_t Nonce = 0;
};

// One object a test exposes as a value of one registered class through the
// ordinary conversion write path, together with the ownership statement that
// decides how that value is owned and released.
struct ClassValueExposureRequest final {
  std::string QualifiedName;
  std::string Path;
  void *Storage = nullptr;
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  ConstAccess Access = ConstAccess::Mutable;

  // The explicit lifetime a borrowed value requires, the one shared ownership
  // reference a shared value requires, and the semantic allocator protocol
  // whose declared steps decide this value's cleanup.
  LifetimeHandle Handle = LifetimeHandle::Undeclared();
  std::shared_ptr<void> SharedOwnership;
  ClassAllocator Allocator;
};

// One object a test asks Luna to create rather than to adopt: the semantic
// allocator protocol produces its storage, the construction step builds the
// object in it, and only a completely owned value is published. It is exactly
// the path a constructor or factory candidate takes.
struct ClassValueConstructionRequest final {
  std::string QualifiedName;
  std::string Path;
  OwnershipModel Ownership = OwnershipModel::LuaOwned;
  ConstAccess Access = ConstAccess::Mutable;

  ClassAllocator Allocator;

  // The construction step. When it is absent the protocol's own construction
  // step is used; when it refuses or throws, no object exists and the storage
  // is given back without being destroyed.
  ObjectConstruction Construct;

  LifetimeHandle Handle = LifetimeHandle::Undeclared();
  std::shared_ptr<void> SharedOwnership;
};

// One member access a test takes against the value at a canonical path: the
// registered class, the member name, the path the value lives at, and - for a
// write - the Luna-owned value being written.
struct ClassMemberAccessRequest final {
  std::string QualifiedName;
  std::string Member;
  std::string Path;
  Value Incoming;
};

// What one attempted member access observed. Nothing here is an address: the
// value is the Luna-owned value the member produced, and the failure is the
// deterministic token of the earliest check that refused.
struct ClassMemberAccessObservation final {
  bool Reached = false;
  std::string Failure;
  std::string Receiver;
  std::string Diagnostic;

  // Which half of the member boundary the outcome belongs to:
  // `before_user_code`, where the native object is unchanged, or
  // `after_user_code`, where Luna promises only virtual-machine rollback and
  // exception translation.
  std::string Boundary;

  // The value one successful read produced, whether it came from the lazy cache
  // instead of from the declared getter, and whether a successful read recorded
  // its value for later reuse.
  std::optional<Value> Produced;
  bool ServedFromCache = false;
  bool Recorded = false;

  // How many cached values one successful write invalidated.
  std::size_t Invalidated = 0;
};

// One access a test takes against the value at a canonical path.
struct ClassAccessRequest final {
  std::string QualifiedName;
  std::string Path;

  // The object the caller expects native code to receive. The observation
  // reports whether exactly that object arrived, so no address leaves Luna.
  const void *ExpectedStorage = nullptr;
};

struct ClassAccessObservation final {
  bool ReachedNativeCode = false;
  bool DeliveredExpectedObject = false;
  bool PermitsMutation = false;
  std::string Failure;
  std::string Diagnostic;
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

  // The dispatch indirection of one State: the permanent slot one canonical
  // callable path owns, the slot the closure installed at that path actually
  // carries, whether the current generation resolves that slot to a target, how
  // many slots the State has ever issued, and which dispatch generation is
  // current. An installed closure carries only the slot, so these are the only
  // way to observe what a path dispatches through.
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

  // The retention accounting of one State's dispatch indirection: how many
  // invocations currently hold a generation, how many superseded generations
  // are still journaled, how many of those something still retains, and how
  // many a reclamation attempt actually released. A retained generation is
  // never reclaimed, which is what lets a test prove a call in progress keeps
  // its own generation.
  [[nodiscard]] static std::size_t
  DispatchInvocationRetainerCount(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  SupersededDispatchGenerationCount(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  RetainedDispatchGenerationCount(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  ReclaimDispatchGenerations(State &Owner) noexcept;

  // A test publication of one dispatch generation, standing in for the
  // lifecycle operations a later milestone performs. Retiring publishes an
  // immutable unavailable entry for the path's permanent slot; retargeting
  // publishes the callable of `TargetName` as the target of `SlotName`'s slot.
  // Neither touches the virtual machine, so both are safe from inside a running
  // call.
  [[nodiscard]] static bool RetireDispatchSlot(State &Owner,
                                               std::string_view GlobalName);
  [[nodiscard]] static bool RetargetDispatchSlot(State &Owner,
                                                 std::string_view SlotName,
                                                 std::string_view TargetName);

  // One accounted claim on the current generation, taken on behalf of the
  // userdata release gate or the lifecycle journal, and given back by the
  // returned token. It is how a test proves reclamation waits for retainers
  // that are not invocations.
  [[nodiscard]] static DispatchRetention
  RetainDispatchGeneration(const State &Owner, DispatchRetainer Retainer);

  // The overload set one canonical callable path owns: how many candidates it
  // published, how many an open attempt staged, and the canonical order of the
  // committed candidate signatures. Only Luna can observe this; a consumer sees
  // one callable.
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
  [[nodiscard]] static std::optional<CallbackStackRestorationObservation>
  ObserveLastCallbackStackRestoration(const State &Owner) noexcept;

  // Private access to the State's single logical reflection database. Public
  // consumers only ever observe owning snapshots.
  [[nodiscard]] static ReflectionDatabase *
  ReflectionDatabaseOf(State &Owner) noexcept;
  [[nodiscard]] static std::uint64_t
  ReflectionGeneration(const State &Owner) noexcept;

  // Private access to the committed generation set, the logical State identity,
  // the owner-object epoch, and the active outer transaction. None of these are
  // reachable from the public API.
  [[nodiscard]] static std::shared_ptr<const GenerationSet>
  GenerationsOf(const State &Owner);
  [[nodiscard]] static std::optional<StateIdentity>
  LogicalIdentityOf(const State &Owner) noexcept;
  [[nodiscard]] static std::optional<std::uint64_t>
  OwnerEpochOf(const State &Owner) noexcept;
  [[nodiscard]] static std::optional<std::uint64_t>
  LifecycleGenerationOf(const State &Owner) noexcept;
  [[nodiscard]] static bool HasActiveTransaction(const State &Owner) noexcept;

  // The entry capture a transaction would take right now: owner thread,
  // readiness and freeze phase, entry stack depth, identity, epochs, and the
  // committed generation set.
  [[nodiscard]] static std::optional<TransactionCapture>
  CaptureTransactionEntryOf(const State &Owner);

  // Freeze is an explicit later operation. This models the frozen phase so
  // capture and validation can be exercised against it today.
  static bool MarkFrozen(State &Owner) noexcept;
  [[nodiscard]] static bool IsFrozen(const State &Owner) noexcept;
  [[nodiscard]] static FreezeCacheObservation
  ObserveFreezeCache(const State &Owner);

  // Replacing the registered model is a later milestone. This advances the
  // lifecycle generation so handles captured earlier can be exercised against
  // an incompatible generation replacement today.
  static bool AdvanceLifecycleGeneration(State &Owner) noexcept;

  // The load-once module state of one State: how many modules are loaded, which
  // identities and versions are loaded, and how many definitions are available
  // to dependency resolution. None of this is reachable from the public API
  // except through reflection.
  [[nodiscard]] static std::size_t
  LoadedModuleCount(const State &Owner) noexcept;
  [[nodiscard]] static bool ModuleIsLoaded(const State &Owner,
                                           std::string_view Identity) noexcept;
  [[nodiscard]] static std::optional<std::string>
  LoadedModuleVersion(const State &Owner, std::string_view Identity);
  [[nodiscard]] static std::size_t
  AvailableModuleCount(const State &Owner) noexcept;

  // How many namespace tables Luna owns, and whether one exact qualified name
  // is owned as a namespace whose table the virtual machine still holds.
  [[nodiscard]] static std::size_t
  NamespaceOwnershipCount(const State &Owner) noexcept;
  [[nodiscard]] static bool NamespaceIsOwned(const State &Owner,
                                             std::string_view QualifiedName);

  // The registered classes of one State: how many there are, whether one exact
  // qualified name is registered with a complete identity that belongs to this
  // logical State and its current lifecycle generation, the canonical class
  // type, and the state-local metatable identity. None of this is reachable
  // from the public API, and none of it exposes an address.
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

  // Whether the virtual-machine metatable of one registered class exists yet,
  // and how many times it was created. Registration creates nothing, the first
  // exposure creates exactly one table, and every later exposure of any value
  // of that class reuses the retained one.
  [[nodiscard]] static bool
  ClassMetatableIsCreated(const State &Owner, std::string_view QualifiedName);
  [[nodiscard]] static std::size_t
  ClassMetatableCreationCount(const State &Owner,
                              std::string_view QualifiedName);

  // Runs one complete collection in this State's virtual machine. Luau exposes
  // no script-visible collector, so this is how a test proves what survives
  // collection.
  [[nodiscard]] static bool CollectGarbage(State &Owner) noexcept;

  // Whether Luna's contained collection boundary is installed in this State's
  // virtual machine. Luau spells that boundary as the destructor of Luna's own
  // userdata tag, so this is the `__gc` of a typed userdata.
  [[nodiscard]] static bool
  UserdataCollectorIsInstalled(const State &Owner) noexcept;

  // Exactly what the collection boundary did across the process: how often it
  // was entered, how many values it released, how many were already released,
  // how many blocks were not Luna's or had no route, and how many exceptions it
  // contained instead of letting them reach the virtual machine.
  [[nodiscard]] static UserdataCollectionCounters
  ObserveUserdataCollections() noexcept;
  static void ResetUserdataCollections() noexcept;

  // What the destruction of the most recently destroyed State observed about
  // its own ordering. It is readable after that State is gone, which is the
  // only moment the whole ordering is knowable.
  [[nodiscard]] static StateDestructionObservation
  ObserveLastStateDestruction() noexcept;

  // One userdata header this State would write for a value of one registered
  // class. It carries the logical State identity, the dynamic and declared-view
  // types, the class symbol, and the metatable identity, so a test can prove
  // those identities survive a State move without exposing any address.
  [[nodiscard]] static std::optional<UserdataHeader>
  DescribeClassUserdata(const State &Owner, std::string_view QualifiedName,
                        OwnershipModel Ownership, ConstAccess Access);

  // The one ownership and release gate of this State, so a test can drive the
  // whole release state machine - stage, construct, establish, publish,
  // invalidate, release - directly, exactly the way collection, a lifecycle
  // action, and State destruction drive it.
  [[nodiscard]] static OwnershipRegistry *
  UserdataOwnershipOf(State &Owner) noexcept;

  // Exactly how many times each release step has run in this State: invalidate,
  // cache removal, destroy, shared release, deallocate, and metadata release,
  // plus contained exceptions and cleanup steps that ran without their
  // metadata. An independent model predicts every one of these.
  [[nodiscard]] static ReleaseCounters
  UserdataReleaseCounters(const State &Owner) noexcept;

  // Exactly how many allocation and construction steps this State performed and
  // how many it refused, plus the exceptions those steps threw and Luna
  // contained. A milestone model predicts every one of them.
  [[nodiscard]] static ConstructionCounters
  UserdataConstructionCounters(const State &Owner) noexcept;

  // How many exposed values this State still owns, and how many of those are
  // published and therefore reachable from native code.
  [[nodiscard]] static std::size_t
  OwnedUserdataCount(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  PublishedUserdataCount(const State &Owner) noexcept;

  // The native identity cache of one State: how many exposures it recorded, and
  // how many of those are still live. An entry stops being live before the
  // payload it described is released.
  [[nodiscard]] static std::size_t
  CachedIdentityCount(const State &Owner) noexcept;
  [[nodiscard]] static std::size_t
  LiveCachedIdentityCount(const State &Owner) noexcept;

  // Exposes one native object of one registered class at a canonical path,
  // through the identity cache and the lazily created class metatable. It
  // performs no ownership transition beyond publication: the release gate owns
  // destruction, shared release, and deallocation.
  [[nodiscard]] static ClassExposureObservation
  ExposeClassUserdata(State &Owner, const ClassExposureRequest &Request);

  // Exposes one native object as a value of one registered class through
  // exactly the conversion write path a returned value takes: the identity
  // cache decides first, the release gate establishes ownership, the lazily
  // created class metatable is associated, and only a completely owned value is
  // published.
  [[nodiscard]] static ClassValueWriteObservation
  ExposeClassValue(State &Owner, const ClassValueExposureRequest &Request);

  // Creates one object through the semantic allocator protocol and publishes it
  // as a value of one registered class, through exactly the same write path an
  // adopted object takes. Every milestone belongs to Luna here - allocation,
  // construction, ownership, cache, metatable, publication - so a failure at
  // any of them performs exactly the cleanup the completed milestones warrant.
  [[nodiscard]] static ClassValueWriteObservation
  ConstructClassValue(State &Owner,
                      const ClassValueConstructionRequest &Request);

  // Releases the value one exposed object owns through the one idempotent
  // release gate, for the cause a test names. It is what a collection, an
  // explicit invalidation, or a lifecycle action performs.
  [[nodiscard]] static bool ReleaseClassValue(State &Owner, const void *Storage,
                                              ReleaseCause Cause);

  // Reads the value one canonical path holds as a handle of one registered
  // class, through exactly the conversion path an ordinary argument takes.
  [[nodiscard]] static ClassAccessObservation
  AccessClassUserdata(State &Owner, const ClassAccessRequest &Request);

  // Retires one exposed value: access is invalidated and the cache entry is
  // evicted, which is what must happen before any payload release.
  [[nodiscard]] static bool RetireClassUserdata(State &Owner,
                                                const void *Storage);

  // Whether one registered class declares one member name, and the canonical
  // policies that member published. Only Luna can observe these; a consumer
  // sees them through reflection.
  [[nodiscard]] static bool
  ClassMemberIsRegistered(const State &Owner, std::string_view QualifiedName,
                          std::string_view Member);
  [[nodiscard]] static std::size_t
  ClassMemberCount(const State &Owner, std::string_view QualifiedName);

  // The published operators of one registered class, and whether one exact
  // operator is declared. A consumer sees the operator through the class
  // metatable; only Luna sees the candidate behind it.
  [[nodiscard]] static std::size_t
  ClassOperatorCount(const State &Owner, std::string_view QualifiedName);
  [[nodiscard]] static bool
  ClassOperatorIsRegistered(const State &Owner, std::string_view QualifiedName,
                            ClassOperator Selected);
  [[nodiscard]] static std::optional<std::string>
  ClassOperatorSegment(const State &Owner, std::string_view QualifiedName,
                       ClassOperator Selected);

  // Canonical enumeration of the class surface one registered class relates to:
  // its accessible bases, the safe downcasts into it, and the members it
  // inherits rather than declares. An inherited view retains the declaring
  // class and the original member's own symbol identity.
  [[nodiscard]] static std::vector<ClassBaseView>
  ClassBases(const State &Owner, std::string_view QualifiedName);
  [[nodiscard]] static std::vector<ClassCastView>
  ClassCasts(const State &Owner, std::string_view QualifiedName);
  [[nodiscard]] static std::vector<ClassInheritedMemberView>
  ClassInheritedMembers(const State &Owner, std::string_view QualifiedName);

  // Reads one member of the value one canonical path holds, through exactly the
  // one member gate: the receiver passes the whole deterministic access order
  // first, then the direction, then the declared getter, and only a successful
  // lazy getter records its value.
  [[nodiscard]] static ClassMemberAccessObservation
  ReadClassMemberValue(State &Owner, const ClassMemberAccessRequest &Request);

  // Writes one member of the value one canonical path holds, through the same
  // gate. A successful write invalidates every cached value of that object.
  [[nodiscard]] static ClassMemberAccessObservation
  WriteClassMemberValue(State &Owner, const ClassMemberAccessRequest &Request);

  // Drops every cached value of the exposed object one canonical path holds. It
  // is what an explicit invalidation performs.
  [[nodiscard]] static std::size_t
  InvalidateClassMemberCache(State &Owner, const std::string &Path);

  // Exactly what the last member access taken through the real virtual machine
  // did: which step decided it, which half of the side-effect boundary that
  // step belongs to, the diagnostic it produced, how many values it published,
  // and the two stack depths that prove the callback checkpoint was restored.
  [[nodiscard]] static std::optional<MemberDispatchObservation>
  ObserveLastClassMemberDispatch(const State &Owner);
  static void ClearClassMemberDispatch(State &Owner) noexcept;

  // The lazy value cache of one State: how many objects have entries, how many
  // entries exist at all, how many of those still match the current dispatch
  // generation, how many one exposed object holds, and which generation that
  // object's entries were produced under. The difference between the total and
  // the live count is exactly what a generation change invalidated by mismatch.
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

  // Whether the header of the value one canonical path holds still names lazy
  // entries, and the generation its cache slot records. This is the header slot
  // itself, so a test can prove the entries live outside the virtual-machine
  // block while the slot names them.
  [[nodiscard]] static bool
  ClassUserdataNamesLazyEntries(const State &Owner, const std::string &Path);
  [[nodiscard]] static std::uint64_t
  ClassUserdataLazyGeneration(const State &Owner, const std::string &Path);

  // The header of the value one canonical path holds.
  [[nodiscard]] static std::optional<UserdataHeader>
  ObserveClassUserdata(const State &Owner, const std::string &Path);

  // Submits several function declarations as nested submissions of one
  // outermost transaction, the way a builder or module callback will. The group
  // validates and prepares, then stops before publication.
  [[nodiscard]] static JoinedSubmissionReport
  SubmitJoinedFunctions(State &Owner,
                        std::vector<JoinedFunctionDeclaration> Declarations,
                        bool IgnoreNestedFailures);

  // The same group, taken all the way through protected installation, the
  // internal consistency check, and atomic publication. It publishes every
  // declaration or restores every journalled effect.
  [[nodiscard]] static JoinedSubmissionReport
  PublishJoinedFunctions(State &Owner,
                         std::vector<JoinedFunctionDeclaration> Declarations,
                         bool IgnoreNestedFailures);

  // The same group submitted behind the private callback boundary. The callback
  // throws after `ThrowAfterSubmissions` declarations joined the outer
  // transaction; a count past the end never throws. Nothing thrown crosses the
  // boundary, and the observation also records what every ordinary query saw
  // while the attempt was still in flight.
  [[nodiscard]] static CallbackBoundaryObservation
  SubmitThroughCallback(State &Owner,
                        std::vector<JoinedFunctionDeclaration> Declarations,
                        std::size_t ThrowAfterSubmissions,
                        bool ThrowStandardException, bool PublishWhenComplete);

  // The exact value kind one canonical virtual-machine path holds right now, so
  // a journalled overwrite of a foreign value is observable.
  [[nodiscard]] static std::optional<std::string>
  ObserveVmPathValueKind(State &Owner, const std::string &Path) noexcept;

  // Drives the installation journal directly: it captures the exact prior value
  // of every requested path, overwrites each one, journals one overlay per
  // requested category, and then either restores everything in reverse order or
  // keeps it. Categories whose committed stores arrive with a later milestone
  // are exercised through this hook today.
  [[nodiscard]] static PublicationObservation
  ProbeInstallationJournal(State &Owner, const std::vector<std::string> &Paths,
                           const std::vector<InstallationScope> &Overlays,
                           bool RestoreInsteadOfCommit);

  static void InjectFault(State &Owner, StateFaultPoint Point,
                          std::size_t Count = 1) noexcept;
  [[nodiscard]] static bool ConsumeFault(State &Owner,
                                         StateFaultPoint Point) noexcept;
  [[nodiscard]] static std::size_t
  PendingFaults(const State &Owner, StateFaultPoint Point) noexcept;
};

} // namespace Luna::Detail
