// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/lifetime_handle.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/test_hooks.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/identity.hpp"
#include "state/userdata/ownership.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::AllocatorStepResult;
using Luna::ClassAllocator;
using Luna::StorageRequest;
using Luna::Detail::ConstAccess;
using Luna::Detail::LifetimeState;
using Luna::Detail::NativeIdentity;
using Luna::Detail::OwnershipFailure;
using Luna::Detail::OwnershipModel;
using Luna::Detail::OwnershipRegistry;
using Luna::Detail::OwnershipRequest;
using Luna::Detail::ReleaseCause;
using Luna::Detail::StagedStorage;
using Luna::Detail::UserdataHeader;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "userdata ownership check failed: " << Description << '\n';
}

// One representative native object, plus the exact number of times Luna ran
// each storage step on it.
struct Probe final {
  int Value = 7;
};

int DestroyCalls = 0;
int DeallocateCalls = 0;
bool DestroyThrows = false;

void ResetStorageCounters() {
  DestroyCalls = 0;
  DeallocateCalls = 0;
  DestroyThrows = false;
}

[[nodiscard]] Probe *AllocateProbe() {
  return static_cast<Probe *>(::operator new(sizeof(Probe)));
}

// The semantic protocol of storage Luna owns: it constructs the object,
// destroys it exactly once, and gives the storage back. Every step is
// Luau-free, and none of them names anything but the storage it was handed.
[[nodiscard]] ClassAllocator OwnedStorageProtocol() {
  ClassAllocator::AllocateOperation Allocate =
      [](const StorageRequest &Wanted) -> void * {
    return ::operator new(Wanted.ByteCount);
  };
  ClassAllocator::ConstructOperation Construct = [](void *Storage) {
    new (Storage) Probe{};
    return AllocatorStepResult::Done();
  };
  ClassAllocator::DestroyOperation Destroy = [](void *Storage) {
    ++DestroyCalls;
    if (DestroyThrows)
      throw std::runtime_error("probe destruction failed");
    static_cast<Probe *>(Storage)->~Probe();
    return AllocatorStepResult::Done();
  };
  ClassAllocator::DeallocateOperation Deallocate = [](void *Storage,
                                                      const StorageRequest &) {
    ++DeallocateCalls;
    ::operator delete(Storage);
    return AllocatorStepResult::Done();
  };
  return ClassAllocator::FromOperations(
      "Studio.ProbeStorage", StorageRequest::ForClass<Probe>(),
      std::move(Allocate), std::move(Construct), std::move(Destroy),
      std::move(Deallocate));
}

std::uint64_t NextNonce = 0;

[[nodiscard]] NativeIdentity IdentityFor(const void *Address) {
  NativeIdentity Identity;
  Identity.Address = Address;
  Identity.Nonce = ++NextNonce;
  return Identity;
}

// One State with one registered class, so every header a test stages carries a
// complete, real class identity.
class Fixture final {
public:
  Fixture() {
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Probe> Class = Registry.RegisterClass<Probe>(
        "Probe", Luna::StableTypeKey("Studio.Probe"));
    Check(Class.Commit().IsSuccess(), "the probe class registers");
    Gate = Hooks::UserdataOwnershipOf(Owner);
    Check(Gate != nullptr, "the State owns one ownership and release gate");
  }

  [[nodiscard]] Luna::State &StateObject() noexcept { return Owner; }
  [[nodiscard]] OwnershipRegistry &Registry() noexcept { return *Gate; }

  [[nodiscard]] UserdataHeader HeaderFor(OwnershipModel Ownership,
                                         ConstAccess Access) {
    const auto Described =
        Hooks::DescribeClassUserdata(Owner, "Probe", Ownership, Access);
    Check(Described.has_value(), "a registered class describes one header");
    return Described ? *Described : UserdataHeader{};
  }

  [[nodiscard]] Luna::Detail::ReleaseCounters Counters() const noexcept {
    return Hooks::UserdataReleaseCounters(Owner);
  }

private:
  Luna::State Owner;
  OwnershipRegistry *Gate = nullptr;
};

// A lifetime handle is one atomic, idempotent, monotonic statement about one
// borrowed object.
void CheckLifetimeHandleSemantics() {
  Luna::LifetimeHandle Handle;
  Check(Handle.IsDeclared() && Handle.IsValid(),
        "a default-constructed handle declares one live lifetime");

  const std::uint64_t Live = Handle.Generation();
  Check(Live != 0, "a live lifetime has a non-zero generation");

  Luna::LifetimeHandle Copy = Handle;
  Check(Copy.RefersToSame(Handle),
        "a copy of a handle declares exactly the same lifetime");

  Handle.Invalidate();
  Check(!Handle.IsValid() && !Copy.IsValid(),
        "invalidating one handle ends the lifetime every copy declares");
  const std::uint64_t Invalidated = Handle.Generation();
  Check(Invalidated != Live,
        "invalidation advances the generation past the published one");

  Handle.Invalidate();
  Copy.Invalidate();
  Check(Handle.Generation() == Invalidated,
        "invalidating an already invalid handle changes nothing");

  const Luna::LifetimeHandle Undeclared = Luna::LifetimeHandle::Undeclared();
  Check(!Undeclared.IsDeclared() && !Undeclared.IsValid() &&
            Undeclared.Generation() == 0,
        "an undeclared handle declares no lifetime at all");
  Check(!Undeclared.RefersToSame(Handle),
        "an undeclared handle is never the same lifetime as a declared one");
}

// A borrowed value requires an explicit handle, is never deleted by Luna, and
// stops being reachable the moment its handle is invalidated.
void CheckBorrowedOwnership() {
  ResetStorageCounters();
  Fixture Owner;
  Probe Borrowed;

  // Without a declared handle the exposure is refused, and nothing is left
  // behind.
  {
    UserdataHeader Header =
        Owner.HeaderFor(OwnershipModel::Borrowed, ConstAccess::Mutable);
    StagedStorage Staged;
    Staged.Storage = &Borrowed;
    Staged.Identity = IdentityFor(&Borrowed);
    const auto Outcome =
        Owner.Registry().Expose(Header, Staged, OwnershipRequest{});
    Check(!Outcome.Succeeded &&
              Outcome.Failure == OwnershipFailure::MissingLifetimeHandle,
          "a borrowed value without an explicit lifetime handle is refused");
    Check(Owner.Registry().RecordCount() == 0,
          "a refused borrowed exposure leaves no owner behind");
    Check(Header.Lifetime == LifetimeState::Released,
          "a refused exposure releases the value it staged");
    Check(DestroyCalls == 0 && DeallocateCalls == 0,
          "a refused borrowed exposure never destroys or deallocates");
  }

  // Luna refuses to own the storage of an object it only borrows.
  {
    UserdataHeader Header =
        Owner.HeaderFor(OwnershipModel::Borrowed, ConstAccess::Mutable);
    StagedStorage Staged;
    Staged.Storage = &Borrowed;
    Staged.Identity = IdentityFor(&Borrowed);
    Staged.Allocator = OwnedStorageProtocol();
    OwnershipRequest Request;
    Request.Handle = Luna::LifetimeHandle();
    const auto Outcome = Owner.Registry().Expose(Header, Staged, Request);
    Check(!Outcome.Succeeded &&
              Outcome.Failure == OwnershipFailure::BorrowedStorageOwnership,
          "Luna never takes ownership of storage it only borrows");
  }

  Luna::LifetimeHandle Handle;
  UserdataHeader Header =
      Owner.HeaderFor(OwnershipModel::Borrowed, ConstAccess::Mutable);
  StagedStorage Staged;
  Staged.Storage = &Borrowed;
  Staged.Identity = IdentityFor(&Borrowed);

  OwnershipRequest Request;
  Request.Handle = Handle;
  const auto Exposed = Owner.Registry().Expose(Header, Staged, Request);
  Check(Exposed.Succeeded, "a borrowed value with a live handle publishes");
  Check(Header.Lifetime == LifetimeState::Published,
        "a published borrowed value is in the published state");
  Check(Header.Handle.IsDeclared() && Header.HasRequiredLifetimeHandle(),
        "a published borrowed value carries its explicit lifetime handle");
  Check(Owner.Registry().LifetimePermitsAccess(Header),
        "a published borrowed value with a live handle permits access");

  // Invalidation is atomic with respect to access: one comparison rejects every
  // later access, and no native code runs.
  Handle.Invalidate();
  Check(!Owner.Registry().LifetimePermitsAccess(Header),
        "an invalidated handle rejects every later access");
  Check(Header.Lifetime == LifetimeState::Published,
        "invalidating a handle releases nothing by itself");
  Check(DestroyCalls == 0 && DeallocateCalls == 0,
        "an invalidated borrowed value is never destroyed by Luna");

  // A handle whose consumer object is gone still rejects access, because Luna
  // retains the record it compares against.
  {
    UserdataHeader Second =
        Owner.HeaderFor(OwnershipModel::Borrowed, ConstAccess::Mutable);
    StagedStorage SecondStaged;
    SecondStaged.Storage = &Borrowed;
    SecondStaged.Identity = IdentityFor(&Borrowed);
    OwnershipRequest SecondRequest;
    {
      Luna::LifetimeHandle Temporary;
      SecondRequest.Handle = Temporary;
      Check(Owner.Registry()
                .Expose(Second, SecondStaged, SecondRequest)
                .Succeeded,
            "a second borrowed value publishes through its own handle");
      Temporary.Invalidate();
    }
    Check(!Owner.Registry().LifetimePermitsAccess(Second),
          "a value of an invalidated lifetime stays unreachable after the "
          "consumer's handle object is gone");
    Check(Owner.Registry().Release(Second, ReleaseCause::GarbageCollection),
          "collecting the second borrowed value releases it once");
  }

  const auto Before = Owner.Counters();
  Check(Owner.Registry().Release(Header, ReleaseCause::ExplicitInvalidation),
        "explicitly releasing a borrowed value releases it once");
  const auto After = Owner.Counters();
  Check(After.Destroy == Before.Destroy &&
            After.Deallocate == Before.Deallocate,
        "releasing a borrowed value destroys and deallocates nothing");
  Check(After.CacheRemoval == Before.CacheRemoval + 1,
        "releasing a value removes its identity-cache entry exactly once");
  Check(After.MetadataRelease == Before.MetadataRelease + 1,
        "releasing a value releases its metadata exactly once");
  Check(Header.Lifetime == LifetimeState::Released,
        "a released borrowed value is in the released state");
  Check(!Owner.Registry().Release(Header, ReleaseCause::GarbageCollection),
        "releasing an already released value does nothing");
  Check(Owner.Counters().MetadataRelease == After.MetadataRelease,
        "a second release performs no second step");
  Check(Borrowed.Value == 7, "a borrowed object is left exactly as it was");
  Check(Owner.Counters().IncompleteMetadata == 0,
        "every cleanup step ran with the metadata it needs");
}

// A Lua-owned value is destroyed exactly once and its storage is deallocated
// exactly once, whichever cause ends it.
void CheckLuaOwnedOwnership() {
  ResetStorageCounters();
  {
    Fixture Owner;
    Probe *Storage = AllocateProbe();
    UserdataHeader Header =
        Owner.HeaderFor(OwnershipModel::LuaOwned, ConstAccess::Mutable);

    StagedStorage Staged;
    Staged.Storage = Storage;
    Staged.Identity = IdentityFor(Storage);
    Staged.Allocator = OwnedStorageProtocol();

    Check(Owner.Registry().Stage(Header, Staged).Succeeded,
          "staged storage of a Lua-owned value is recorded");
    Check(Header.Lifetime == LifetimeState::Allocated,
          "staged storage is only allocated, never published");
    Check(Header.Allocator.IsDeclared(),
          "a staged value names the record that owns its storage steps");
    Check(!Owner.Registry().LifetimePermitsAccess(Header),
          "a staged value is not reachable from native code");

    new (Storage) Probe{};
    Check(Owner.Registry().Construct(Header).Succeeded,
          "construction moves the value to constructed");
    Check(Owner.Registry().Establish(Header, OwnershipRequest{}).Succeeded,
          "a Lua-owned value needs neither a handle nor a shared reference");
    Check(!Owner.Registry().LifetimePermitsAccess(Header),
          "a constructed but unpublished value is not reachable");
    Check(Owner.Registry().Publish(Header).Succeeded,
          "publication moves the value to published");
    Check(Owner.Registry().LifetimePermitsAccess(Header),
          "a published Lua-owned value permits access");

    // A Lua-owned value never accepts a lifetime handle: its lifetime is
    // Luna's.
    {
      Probe *Second = AllocateProbe();
      new (Second) Probe{};
      UserdataHeader Other =
          Owner.HeaderFor(OwnershipModel::LuaOwned, ConstAccess::Mutable);
      StagedStorage OtherStaged;
      OtherStaged.Storage = Second;
      OtherStaged.Identity = IdentityFor(Second);
      OtherStaged.Allocator = OwnedStorageProtocol();
      OwnershipRequest OtherRequest;
      OtherRequest.Handle = Luna::LifetimeHandle();
      const auto Outcome =
          Owner.Registry().Expose(Other, OtherStaged, OtherRequest);
      Check(!Outcome.Succeeded &&
                Outcome.Failure == OwnershipFailure::UnexpectedLifetimeHandle,
            "a Lua-owned value refuses a borrowed lifetime handle");
      Check(DestroyCalls == 1 && DeallocateCalls == 1,
            "a Lua-owned value that fails publication is destroyed and "
            "deallocated exactly once, in that order");
    }

    ResetStorageCounters();
    Check(Owner.Registry().Release(Header, ReleaseCause::GarbageCollection),
          "collection releases the value once");
    Check(DestroyCalls == 1 && DeallocateCalls == 1,
          "a Lua-owned value is destroyed and deallocated exactly once");
    Check(!Owner.Registry().Release(Header, ReleaseCause::StateDestruction),
          "a second release performs nothing");
    Check(DestroyCalls == 1 && DeallocateCalls == 1,
          "the release gate is idempotent");
    Check(Owner.Counters().IncompleteMetadata == 0,
          "cleanup kept its type, allocator, metatable, and dispatch metadata");
  }

  // Construction failure deallocates the storage without destroying anything.
  ResetStorageCounters();
  {
    Fixture Owner;
    Probe *Storage = AllocateProbe();
    UserdataHeader Header =
        Owner.HeaderFor(OwnershipModel::LuaOwned, ConstAccess::Mutable);
    StagedStorage Staged;
    Staged.Storage = Storage;
    Staged.Identity = IdentityFor(Storage);
    Staged.Allocator = OwnedStorageProtocol();
    Check(Owner.Registry().Stage(Header, Staged).Succeeded,
          "storage is staged before construction is attempted");
    Check(Owner.Registry().Release(Header, ReleaseCause::ConstructionFailure),
          "a construction failure releases the staged storage");
    Check(DestroyCalls == 0 && DeallocateCalls == 1,
          "construction failure deallocates without destroying");
    Check(Owner.Counters().Invalidate == 0,
          "a value that was never accessible needs no invalidation");
  }

  // State destruction releases every remaining value exactly once, while its
  // cleanup metadata is still valid.
  ResetStorageCounters();
  {
    Fixture Owner;
    for (int Index = 0; Index < 3; ++Index) {
      Probe *Storage = AllocateProbe();
      new (Storage) Probe{};
      UserdataHeader Header =
          Owner.HeaderFor(OwnershipModel::LuaOwned, ConstAccess::Mutable);
      StagedStorage Staged;
      Staged.Storage = Storage;
      Staged.Identity = IdentityFor(Storage);
      Staged.Allocator = OwnedStorageProtocol();
      Check(
          Owner.Registry().Expose(Header, Staged, OwnershipRequest{}).Succeeded,
          "each Lua-owned value publishes");
    }
    Check(Owner.Registry().RecordCount() == 3 &&
              Owner.Registry().PublishedCount() == 3,
          "the State owns exactly the published values");
    Check(Hooks::OwnedUserdataCount(Owner.StateObject()) == 3,
          "the State observes exactly its owned values");
  }
  Check(DestroyCalls == 3 && DeallocateCalls == 3,
        "State destruction releases each userdata resource exactly once");
}

// A shared value holds and releases exactly one corresponding shared ownership
// reference per stored object.
void CheckSharedOwnership() {
  ResetStorageCounters();
  Fixture Owner;
  std::shared_ptr<Probe> Object = std::make_shared<Probe>();
  Check(Object.use_count() == 1, "the consumer holds the only reference");

  UserdataHeader Header =
      Owner.HeaderFor(OwnershipModel::Shared, ConstAccess::Mutable);
  StagedStorage Staged;
  Staged.Storage = Object.get();
  Staged.Identity = IdentityFor(Object.get());

  // Without a shared reference a shared value is refused.
  {
    UserdataHeader Refused =
        Owner.HeaderFor(OwnershipModel::Shared, ConstAccess::Mutable);
    StagedStorage RefusedStaged;
    RefusedStaged.Storage = Object.get();
    RefusedStaged.Identity = IdentityFor(Object.get());
    const auto Outcome =
        Owner.Registry().Expose(Refused, RefusedStaged, OwnershipRequest{});
    Check(!Outcome.Succeeded &&
              Outcome.Failure == OwnershipFailure::MissingSharedOwnership,
          "a shared value requires its one shared ownership reference");
    Check(Object.use_count() == 1,
          "a refused shared exposure retains no reference");
  }

  OwnershipRequest Request;
  Request.SharedOwnership = Object;
  Check(Owner.Registry().Expose(Header, Staged, Request).Succeeded,
        "a shared value publishes with its shared ownership reference");
  Check(Object.use_count() == 3,
        "Luna retains exactly one reference beyond the request's own copy");

  const auto Published = Owner.Counters();
  Check(Owner.Registry().Release(Header, ReleaseCause::LifecycleAction),
        "a lifecycle action releases the shared value once");
  const auto Released = Owner.Counters();
  Check(Released.SharedRelease == Published.SharedRelease + 1,
        "exactly one shared ownership reference is released");
  Check(Released.Destroy == Published.Destroy,
        "a shared value is never destroyed by Luna");
  Check(Released.Deallocate == Published.Deallocate,
        "a shared value's storage is never deallocated by Luna");
  Check(Object.use_count() == 2,
        "the request copy is all that is left of Luna's reference");

  Request.SharedOwnership.reset();
  Check(Object.use_count() == 1,
        "the consumer is left holding exactly its own reference");
  Check(!Owner.Registry().Release(Header, ReleaseCause::StateDestruction),
        "releasing a released shared value performs no second release");
  Check(Owner.Counters().SharedRelease == Released.SharedRelease,
        "no second shared ownership reference is ever released");
}

// Nothing a consumer's destruction step throws may escape the gate, and the
// remaining steps still run exactly once.
void CheckContainedCleanupExceptions() {
  ResetStorageCounters();
  Fixture Owner;
  Probe *Storage = AllocateProbe();
  new (Storage) Probe{};

  UserdataHeader Header =
      Owner.HeaderFor(OwnershipModel::LuaOwned, ConstAccess::Mutable);
  StagedStorage Staged;
  Staged.Storage = Storage;
  Staged.Identity = IdentityFor(Storage);
  Staged.Allocator = OwnedStorageProtocol();
  Check(Owner.Registry().Expose(Header, Staged, OwnershipRequest{}).Succeeded,
        "the value publishes before its destruction throws");

  DestroyThrows = true;
  Check(Owner.Registry().Release(Header, ReleaseCause::GarbageCollection),
        "a throwing destruction step still completes the release");
  DestroyThrows = false;
  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "a thrown destruction is contained and deallocation still runs once");
  Check(Owner.Counters().ContainedException == 1,
        "the gate contains exactly the one exception it caught");
  Check(Header.Lifetime == LifetimeState::Released,
        "a contained cleanup exception still reaches the released state");
  Check(Owner.Registry().RecordCount() == 0,
        "a contained cleanup exception still releases the metadata");
}

// The identity-cache entry of a value is removed before its payload is
// released, and the value is never reachable after that.
void CheckCacheEntriesAreRemovedBeforeRelease() {
  ResetStorageCounters();
  Fixture Owner;
  Probe *Storage = AllocateProbe();
  new (Storage) Probe{};

  int RemovedEntries = 0;
  bool RemovedBeforeDestruction = true;
  Owner.Registry().InstallCacheRemover(
      [&RemovedEntries, &RemovedBeforeDestruction](const NativeIdentity &) {
        ++RemovedEntries;
        if (DestroyCalls != 0 || DeallocateCalls != 0)
          RemovedBeforeDestruction = false;
      });

  UserdataHeader Header =
      Owner.HeaderFor(OwnershipModel::LuaOwned, ConstAccess::Mutable);
  StagedStorage Staged;
  Staged.Storage = Storage;
  Staged.Identity = IdentityFor(Storage);
  Staged.Allocator = OwnedStorageProtocol();
  Check(Owner.Registry().Expose(Header, Staged, OwnershipRequest{}).Succeeded,
        "the value publishes");
  Check(RemovedEntries == 0, "publishing removes no cache entry");

  Check(Owner.Registry().Release(Header, ReleaseCause::GarbageCollection),
        "collection releases the value");
  Check(RemovedEntries == 1,
        "the identity-cache entry is removed exactly once");
  Check(RemovedBeforeDestruction,
        "the identity-cache entry is removed before the payload is released");
  Check(!Owner.Registry().LifetimePermitsAccess(Header),
        "a released value is never reachable again");
}

// One exposure through exactly the conversion write path a returned object
// takes.
[[nodiscard]] Luna::Detail::ClassValueWriteObservation
ExposeValue(Luna::State &Host, const std::string &Path, void *Storage,
            OwnershipModel Ownership, const Luna::LifetimeHandle &Handle,
            std::shared_ptr<void> Shared, const ClassAllocator &Allocator) {
  Luna::Detail::ClassValueExposureRequest Request;
  Request.QualifiedName = "Probe";
  Request.Path = Path;
  Request.Storage = Storage;
  Request.Ownership = Ownership;
  Request.Access = ConstAccess::Mutable;
  Request.Handle = Handle;
  Request.SharedOwnership = std::move(Shared);
  Request.Allocator = Allocator;
  return Hooks::ExposeClassValue(Host, Request);
}

[[nodiscard]] Luna::Detail::ClassAccessObservation
ReadValue(Luna::State &Host, const std::string &Path, const void *Expected) {
  Luna::Detail::ClassAccessRequest Request;
  Request.QualifiedName = "Probe";
  Request.Path = Path;
  Request.ExpectedStorage = Expected;
  return Hooks::AccessClassUserdata(Host, Request);
}

// The write half of a borrowed value: the explicit handle is required before
// anything exists, one object is one value with one owner, and invalidation
// stops every later access without releasing anything.
void CheckBorrowedExposureThroughTheWritePath() {
  ResetStorageCounters();
  Fixture Owner;
  Luna::State &Host = Owner.StateObject();
  Probe Borrowed;

  const auto Refused = ExposeValue(
      Host, "Sample", &Borrowed, OwnershipModel::Borrowed,
      Luna::LifetimeHandle::Undeclared(), nullptr, ClassAllocator());
  Check(!Refused.Published && Refused.PublishedCount == 0 &&
            Refused.Failure == "expired_userdata",
        "a borrowed value without an explicit lifetime handle publishes "
        "nothing");
  Check(Refused.FinalStackDepth == Refused.EntryStackDepth,
        "a refused exposure leaves the stack exactly as it found it");
  Check(Hooks::OwnedUserdataCount(Host) == 0,
        "a refused exposure leaves no owner behind");

  Luna::LifetimeHandle Handle;
  const auto Published =
      ExposeValue(Host, "Sample", &Borrowed, OwnershipModel::Borrowed, Handle,
                  nullptr, ClassAllocator());
  Check(Published.Published && Published.PublishedCount == 1 &&
            Published.Failure == "none",
        "a borrowed value with a live handle publishes exactly one value");
  Check(Hooks::OwnedUserdataCount(Host) == 1 &&
            Hooks::PublishedUserdataCount(Host) == 1,
        "the exposed object has exactly one owner");

  const auto Written = Hooks::ObserveClassUserdata(Host, "Sample");
  Check(Written && Written->Lifetime == LifetimeState::Published &&
            Written->Ownership == OwnershipModel::Borrowed &&
            Written->Handle.IsDeclared(),
        "the published value carries its ownership model and explicit "
        "lifetime");

  const auto Reached = ReadValue(Host, "Sample", &Borrowed);
  Check(Reached.ReachedNativeCode && Reached.DeliveredExpectedObject,
        "a published borrowed value delivers exactly its object to native "
        "code");

  // A conflicting re-exposure of the same object is refused by the cache before
  // ownership is established, so no second owner of one object can exist.
  const auto Conflicting = ExposeValue(
      Host, "Other", &Borrowed, OwnershipModel::LuaOwned,
      Luna::LifetimeHandle::Undeclared(), nullptr, OwnedStorageProtocol());
  Check(!Conflicting.Published &&
            Conflicting.Failure == "conflicting_ownership",
        "re-exposing one object under a conflicting ownership model is "
        "refused");
  Check(Hooks::OwnedUserdataCount(Host) == 1,
        "a conflicting re-exposure creates no second owner");
  Check(DestroyCalls == 0 && DeallocateCalls == 0,
        "a refused re-exposure releases nothing");

  const auto Reused =
      ExposeValue(Host, "Sample", &Borrowed, OwnershipModel::Borrowed, Handle,
                  nullptr, ClassAllocator());
  Check(Reused.Published && Reused.PublishedCount == 1,
        "an identical re-exposure hands back the value that already exists");
  Check(Hooks::OwnedUserdataCount(Host) == 1,
        "a reused exposure establishes no second ownership record");

  Handle.Invalidate();
  const auto Expired = ReadValue(Host, "Sample", &Borrowed);
  Check(!Expired.ReachedNativeCode && Expired.Failure == "expired_userdata",
        "an invalidated handle rejects every later access before native code");
  Check(Hooks::OwnedUserdataCount(Host) == 1,
        "invalidating a handle releases nothing by itself");

  Check(Hooks::ReleaseClassValue(Host, &Borrowed,
                                 ReleaseCause::ExplicitInvalidation),
        "explicitly releasing the exposed borrowed value releases it once");
  Check(DestroyCalls == 0 && DeallocateCalls == 0,
        "Luna never destroys or deallocates an object it only borrows");
  Check(Hooks::OwnedUserdataCount(Host) == 0,
        "a released value leaves no owner behind");
  Check(!Hooks::ReleaseClassValue(Host, &Borrowed,
                                  ReleaseCause::GarbageCollection),
        "releasing the same object again performs nothing");
  const auto Released = ReadValue(Host, "Sample", &Borrowed);
  Check(!Released.ReachedNativeCode,
        "a released value never reaches native code again");
  Check(Borrowed.Value == 7, "the borrowed object is left exactly as it was");
  Check(Hooks::UserdataReleaseCounters(Host).IncompleteMetadata == 0,
        "every cleanup step ran with the metadata it needs");
}

// The write half of a Lua-owned and of a shared value: exactly one destruction
// and one deallocation for the first, exactly one shared ownership reference
// released for the second.
void CheckOwnedAndSharedExposureThroughTheWritePath() {
  ResetStorageCounters();
  {
    Fixture Owner;
    Luna::State &Host = Owner.StateObject();
    Probe *Storage = AllocateProbe();
    new (Storage) Probe{};

    const auto Published = ExposeValue(
        Host, "Owned", Storage, OwnershipModel::LuaOwned,
        Luna::LifetimeHandle::Undeclared(), nullptr, OwnedStorageProtocol());
    Check(Published.Published && Published.Failure == "none",
          "a Lua-owned value publishes through the write path");
    const auto Reached = ReadValue(Host, "Owned", Storage);
    Check(Reached.ReachedNativeCode && Reached.DeliveredExpectedObject,
          "a published Lua-owned value reaches native code");

    Check(Hooks::ReleaseClassValue(Host, Storage,
                                   ReleaseCause::GarbageCollection),
          "collection releases the Lua-owned value once");
    Check(DestroyCalls == 1 && DeallocateCalls == 1,
          "a Lua-owned value is destroyed and deallocated exactly once");
    Check(!Hooks::ReleaseClassValue(Host, Storage,
                                    ReleaseCause::StateDestruction),
          "the release gate stays idempotent through the write path");
    Check(DestroyCalls == 1 && DeallocateCalls == 1,
          "no second destruction and no second deallocation ever run");
  }

  ResetStorageCounters();
  {
    Fixture Owner;
    Luna::State &Host = Owner.StateObject();
    std::shared_ptr<Probe> Object = std::make_shared<Probe>();

    const auto Published = ExposeValue(
        Host, "Shared", Object.get(), OwnershipModel::Shared,
        Luna::LifetimeHandle::Undeclared(), Object, ClassAllocator());
    Check(Published.Published && Published.Failure == "none",
          "a shared value publishes with its one shared ownership reference");
    Check(Object.use_count() == 2,
          "Luna retains exactly one shared ownership reference");

    const auto Before = Hooks::UserdataReleaseCounters(Host);
    Check(Hooks::ReleaseClassValue(Host, Object.get(),
                                   ReleaseCause::LifecycleAction),
          "a lifecycle action releases the shared value once");
    const auto After = Hooks::UserdataReleaseCounters(Host);
    Check(After.SharedRelease == Before.SharedRelease + 1,
          "exactly one shared ownership reference is released");
    Check(After.Destroy == Before.Destroy &&
              After.Deallocate == Before.Deallocate,
          "a shared object is never destroyed or deallocated by Luna");
    Check(Object.use_count() == 1,
          "the consumer is left holding exactly its own reference");
    Check(DestroyCalls == 0 && DeallocateCalls == 0,
          "no storage step ever runs for a shared object");
  }

  // A value the write path published and nothing released is still released
  // exactly once by State destruction.
  ResetStorageCounters();
  {
    Fixture Owner;
    Luna::State &Host = Owner.StateObject();
    Probe *Storage = AllocateProbe();
    new (Storage) Probe{};
    const auto Published = ExposeValue(
        Host, "Owned", Storage, OwnershipModel::LuaOwned,
        Luna::LifetimeHandle::Undeclared(), nullptr, OwnedStorageProtocol());
    Check(Published.Published, "the Lua-owned value publishes");
  }
  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "State destruction releases a published value exactly once");
}

} // namespace

int RunUserdataOwnershipTransitionTests() {
  FailureCount = 0;
  CheckLifetimeHandleSemantics();
  CheckBorrowedOwnership();
  CheckLuaOwnedOwnership();
  CheckSharedOwnership();
  CheckContainedCleanupExceptions();
  CheckCacheEntriesAreRemovedBeforeRelease();
  CheckBorrowedExposureThroughTheWritePath();
  CheckOwnedAndSharedExposureThroughTheWritePath();
  return FailureCount == 0 ? 0 : 1;
}
