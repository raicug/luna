#pragma once

// The one ownership and release gate of every exposed userdata value.
//
// Three ownership models exist and they never blur: a borrowed object is owned
// by native code and Luna never deletes it, a Lua-owned object is destroyed
// exactly once by Luna, and a shared object holds exactly one corresponding
// `std::shared_ptr` ownership reference that Luna releases exactly once. Which
// model a value uses decides which cleanup steps apply, and nothing else does.
//
// Every path that can end a value converges here: construction failure,
// publication failure, garbage collection, explicit invalidation of a borrowed
// lifetime handle, a module lifecycle action, and State destruction all call
// the same idempotent gate. The gate walks the release state machine in exactly
// one direction - `Allocated`, `Constructed`, `Published`, `Invalid`, then
// `Destroyed` or `SharedReleased`, then `Released` - so a step is never skipped
// and never repeated, and calling the gate twice on the same value performs no
// second destruction, no second deallocation, and no second shared release.
//
// Cleanup needs metadata, so cleanup keeps it. The canonical type, the class
// symbol, the metatable identity, the dispatch generation, the retained
// lifetime record, and the immutable allocator protocol of the value's storage
// all stay reachable through the final step of release; only after every
// applicable step has run does Luna drop them. The identity-cache entry of a
// value is removed before its payload is released, which is why the cache
// remover is installed here rather than consulted by the cache itself.
//
// Nothing in this gate may throw into its caller: a garbage collector and a
// State destructor both call it, so every consumer-supplied destruction or
// deallocation step is contained here and counted.

// clang-format off
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/lifetime_handle.hpp>
#include <luna/reflection/ids.hpp>

#include "state/transaction/lifecycle.hpp"
#include "state/userdata/allocator.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/identity.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

// Why the release gate ran. Every one of these converges on the same idempotent
// transitions; the cause is diagnostic and accounting information only.
enum class ReleaseCause : std::uint8_t {
  ConstructionFailure,
  PublicationFailure,
  GarbageCollection,
  ExplicitInvalidation,
  LifecycleAction,
  StateDestruction
};

[[nodiscard]] std::string_view ReleaseCauseText(ReleaseCause Cause) noexcept;

// The generation one Luna-owned lifetime record reports right now, in exactly
// the shape the access gate's probe expects. This is the whole invalidation
// mechanism seen from the access side: an invalidated handle has advanced past
// the generation its values were published under, so one comparison rejects
// every later access to every one of them, atomically and without ever touching
// the native object.
[[nodiscard]] std::uint64_t
ObserveLifetimeHandleGeneration(const void *Record) noexcept;

// Exactly how many times each build step has run in one State: how many
// allocations produced storage and how many refused, and how many constructions
// completed and how many refused. Destruction and deallocation are release
// steps, so they are counted with the rest of release rather than here - the
// cleanup of a failed construction is a release, whichever milestone triggered
// it.
struct ConstructionCounters final {
  std::uint64_t Allocate = 0;
  std::uint64_t AllocationFailure = 0;
  std::uint64_t Construct = 0;
  std::uint64_t ConstructionFailure = 0;

  // Exceptions an allocation or a construction step threw and this gate
  // contained instead of letting them escape into a virtual-machine callback.
  std::uint64_t ContainedException = 0;
};

// Exactly how many times each release step has run in one State. Every counter
// is a step count, not a callback count, so an independent model can predict
// all of them from the generated ownership sequence alone.
struct ReleaseCounters final {
  std::uint64_t Invalidate = 0;
  std::uint64_t CacheRemoval = 0;
  std::uint64_t Destroy = 0;
  std::uint64_t SharedRelease = 0;
  std::uint64_t Deallocate = 0;
  std::uint64_t MetadataRelease = 0;

  // Exceptions the gate contained instead of letting them escape, and cleanup
  // steps that ran without the metadata they require. The second one must stay
  // zero: it is the direct observation of "metadata remains valid through
  // cleanup".
  std::uint64_t ContainedException = 0;
  std::uint64_t IncompleteMetadata = 0;
};

// Why one ownership step was refused. Each value is one deterministic reason,
// so a diagnostic never has to guess.
enum class OwnershipFailure : std::uint8_t {
  None,
  UnknownIdentity,
  StagedLifetime,
  MissingIdentity,
  MissingStorage,
  DuplicateIdentity,
  MissingLifetimeHandle,
  ExpiredLifetimeHandle,
  UnexpectedLifetimeHandle,
  MissingSharedOwnership,
  UnexpectedSharedOwnership,
  UnreleasableStorage,
  BorrowedStorageOwnership,
  OwnershipAlreadyEstablished,
  MissingOwnership,

  // The construction step of one staged object refused to construct it, or
  // threw. Either way no object exists, so the staged storage is given back
  // without destroying anything.
  RefusedConstruction
};

[[nodiscard]] std::string_view
OwnershipFailureText(OwnershipFailure Failure) noexcept;

struct OwnershipOutcome final {
  bool Succeeded = false;
  OwnershipFailure Failure = OwnershipFailure::None;

  [[nodiscard]] static OwnershipOutcome Accept() noexcept;
  [[nodiscard]] static OwnershipOutcome
  Refuse(OwnershipFailure Reason) noexcept;
};

// The storage one value was staged with, before anything was constructed in it,
// together with the semantic protocol that storage came from. Staging retains
// the protocol, which is what keeps a consumer's allocator policy and its state
// reachable until the last value created through it completes cleanup.
struct StagedStorage final {
  void *Storage = nullptr;
  NativeIdentity Identity;

  // The protocol whose declared steps decide this value's cleanup. A value Luna
  // neither created nor releases is staged with an undeclared protocol, and
  // staging attaches the shared borrowed protocol to it.
  ClassAllocator Allocator;
};

// The ownership one constructed value is about to be given. Exactly one of the
// two payload members is meaningful, and which one is decided by the ownership
// model of the header, never by which member happens to be set.
struct OwnershipRequest final {
  LifetimeHandle Handle = LifetimeHandle::Undeclared();
  std::shared_ptr<void> SharedOwnership;
};

// Luna's own record of one exposed value. It outlives every cleanup step of
// that value and is dropped only by the final metadata release.
struct OwnershipRecord final {
  NativeIdentity Identity;
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  LifetimeState Lifetime = LifetimeState::Allocated;

  bool WasConstructed = false;
  bool OwnershipEstablished = false;

  void *Storage = nullptr;

  // The retained semantic protocol of this value's storage. Retaining it here
  // is what makes the destruction and deallocation steps - and whatever state
  // the consumer's operations captured - still reachable at the final release
  // step, long after the allocator value the consumer held is gone.
  ClassAllocator Allocator;

  // The retained lifetime record of a borrowed value and the generation the
  // value was published under. Retaining the record is what makes reading the
  // generation safe even after the consumer's handle object is gone.
  std::shared_ptr<const LifetimeRecord> Handle;
  std::uint64_t HandleGeneration = 0;

  // The one shared ownership reference of a shared value.
  std::shared_ptr<void> SharedOwnership;

  // The cleanup metadata this value needs through its final release.
  StateIdentity Origin;
  TypeId DynamicType;
  TypeId DeclaredViewType;
  SymbolId ClassSymbol;
  MetatableId Metatable;
  std::uint64_t DispatchGeneration = 0;

  // Everything cleanup needs is still reachable: the canonical types, the class
  // symbol, the metatable identity, and every step of the allocator protocol
  // this value's cleanup will actually run.
  [[nodiscard]] bool RetainsCleanupMetadata() const noexcept;

  // A borrowed value's declared lifetime is still exactly the one it was
  // published under. Any other ownership model is always live.
  [[nodiscard]] bool HasLiveHandle() const noexcept;
};

class OwnershipRegistry final {
public:
  // Removes the identity-cache entry of one value. It runs before the payload
  // of that value is released, never after.
  using CacheRemover = std::function<void(const NativeIdentity &)>;

  OwnershipRegistry() = default;

  OwnershipRegistry(const OwnershipRegistry &) = delete;
  OwnershipRegistry &operator=(const OwnershipRegistry &) = delete;
  OwnershipRegistry(OwnershipRegistry &&) = delete;
  OwnershipRegistry &operator=(OwnershipRegistry &&) = delete;

  // State destruction: every value this State still owns is released exactly
  // once, through exactly the same gate, while all of its cleanup metadata is
  // still valid.
  ~OwnershipRegistry();

  void InstallCacheRemover(CacheRemover Remover);

  // The semantic allocation step of one protocol. Storage that exists is not
  // staged yet: allocation is one milestone and staging is the next, so an
  // allocation that produced nothing needs no cleanup call at all.
  [[nodiscard]] StorageAllocationOutcome
  Allocate(const ClassAllocator &Allocator) noexcept;

  // Storage that was allocated and then never staged, given straight back. It
  // is the only deallocation that happens outside the release gate, because it
  // is the only one whose storage no record ever described.
  void DiscardStorage(const ClassAllocator &Allocator, void *Storage) noexcept;

  // `Allocated`: the storage of one value exists and nothing has been
  // constructed in it yet.
  [[nodiscard]] OwnershipOutcome Stage(UserdataHeader &Header,
                                       const StagedStorage &Staged);

  // `Allocated` to `Constructed`: native construction succeeded, so from here
  // on release destroys a known-constructed object.
  [[nodiscard]] OwnershipOutcome Construct(UserdataHeader &Header);

  // The same step, performed here rather than reported. The supplied
  // construction step - a constructor candidate, a factory, or the protocol's
  // own - runs inside this gate, so everything it throws is contained and a
  // refusal releases the staged storage without destroying an object that was
  // never constructed.
  [[nodiscard]] OwnershipOutcome Construct(UserdataHeader &Header,
                                           const ObjectConstruction &Build);

  // Ownership establishment of one constructed value. A borrowed value requires
  // a declared, still live lifetime handle; a shared value requires exactly one
  // shared ownership reference; a Lua-owned value requires a destruction step
  // and neither of the two.
  [[nodiscard]] OwnershipOutcome Establish(UserdataHeader &Header,
                                           const OwnershipRequest &Request);

  // `Constructed` to `Published`: ownership is established and the value is
  // visible. This is the only state that permits native access.
  [[nodiscard]] OwnershipOutcome Publish(UserdataHeader &Header);

  // One already live native object, taken through every step at once. A failure
  // at any step releases exactly what the earlier steps established, so a
  // refused exposure leaves no record, no second owner, and no half-built
  // value.
  [[nodiscard]] OwnershipOutcome Expose(UserdataHeader &Header,
                                        const StagedStorage &Staged,
                                        const OwnershipRequest &Request);

  // `Published` or `Constructed` to `Invalid`: access is rejected from here on,
  // and nothing has been released yet. Invalidating an already invalid or
  // released value changes nothing.
  bool Invalidate(UserdataHeader &Header, ReleaseCause Cause) noexcept;

  // The idempotent gate. It invalidates access, removes the identity-cache
  // entry, destroys when Luna owns the object, releases the one shared
  // ownership reference when there is one, deallocates storage Luna allocated,
  // and only then releases Luna's metadata. Calling it again does nothing.
  bool Release(UserdataHeader &Header, ReleaseCause Cause) noexcept;

  // The same gate, entered from the virtual machine's collector. Collection is
  // one more cause, not one more path: it invalidates access, walks exactly the
  // same steps, and releases the value exactly once. What is different is that
  // the collector is mid-traversal, so for the duration of this call no release
  // step may re-enter the virtual machine; the identity-cache entry is still
  // evicted, through the Luna-owned half of the installed remover.
  bool ReleaseCollected(UserdataHeader &Header) noexcept;

  // Whether a release step running right now may call into the virtual machine.
  // It is false while a collection finalizer or this registry's own destruction
  // is in progress, which is exactly when the machine is not re-entrant.
  [[nodiscard]] bool PermitsVirtualMachineAccess() const noexcept {
    return !IsFinalizing && !IsShuttingDown;
  }

  // The same gate, entered by the native object instead of by its value's
  // header. A lifecycle action and an explicit invalidation know the object
  // they are ending, not the virtual-machine block that carries it, and the
  // cache-removal step of the gate invalidates that block before anything is
  // released anyway. Calling it again does nothing.
  bool ReleaseByStorage(const void *Address, ReleaseCause Cause) noexcept;

  // Every still live value, released exactly once each.
  std::size_t ReleaseAll(ReleaseCause Cause) noexcept;

  // The lifetime step of the access order: the value is published, its record
  // agrees, and a borrowed value's declared lifetime is still the one it was
  // published under. An invalidated handle fails here, before any native
  // pointer exists.
  [[nodiscard]] bool
  LifetimePermitsAccess(const UserdataHeader &Header) const noexcept;

  [[nodiscard]] const OwnershipRecord *
  Find(const NativeIdentity &Identity) const noexcept;

  // Every value this registry still owns, ordered by the state-local nonce of
  // its identity. The nonce is monotonic and never recycled, so the order is
  // canonical and never depends on an allocation address. A lifecycle analysis
  // reads this to learn which live values one operation would reach.
  [[nodiscard]] std::vector<const OwnershipRecord *> OwnedValues() const;

  // How many values are still owned, and how many of those are published.
  [[nodiscard]] std::size_t RecordCount() const noexcept {
    return Records.size();
  }
  [[nodiscard]] std::size_t PublishedCount() const noexcept;

  // Every value this registry still owns retains the type, allocator,
  // metatable, and dispatch metadata its cleanup needs. State destruction asks
  // this while its virtual machine is still open, which is the moment the
  // answer matters.
  [[nodiscard]] bool RetainsCleanupMetadata() const noexcept;

  [[nodiscard]] ReleaseCounters Counters() const noexcept { return Counted; }

  // Exactly how many allocation and construction steps this State performed and
  // how many it refused. An independent milestone model predicts every one of
  // them from the generated construction sequence alone.
  [[nodiscard]] ConstructionCounters ConstructionCounts() const noexcept {
    return Built;
  }

private:
  [[nodiscard]] OwnershipRecord *
  FindForUpdate(const NativeIdentity &Identity) noexcept;

  // The whole gate, expressed once against the record. The header is optional
  // because State destruction releases records whose virtual-machine block no
  // longer exists.
  bool ReleaseRecord(OwnershipRecord &Record, ReleaseCause Cause,
                     UserdataHeader *Header) noexcept;
  void InvalidateRecord(OwnershipRecord &Record,
                        UserdataHeader *Header) noexcept;
  void RemoveCacheEntry(const OwnershipRecord &Record) noexcept;
  void Drop(const NativeIdentity &Identity) noexcept;

  std::vector<std::unique_ptr<OwnershipRecord>> Records;
  CacheRemover RemoveFromCache;
  ReleaseCounters Counted;
  ConstructionCounters Built;

  // Set while this registry itself is being destroyed. The cache-removal step
  // still runs and is still counted, but the installed remover is not called,
  // because the cache it would touch may already be gone. Every earlier cause,
  // including an explicit release of everything during State destruction, calls
  // it normally.
  bool IsShuttingDown = false;

  // Set while a collection finalizer is releasing one value. The installed
  // remover is still called - the identity-cache entry must go before the
  // payload - but it is told not to touch the virtual machine, because the
  // collector holding this call is mid-traversal.
  bool IsFinalizing = false;
};

} // namespace Luna::Detail
