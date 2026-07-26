// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/lifetime_handle.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/test_hooks.hpp"
#include "state/userdata/collection.hpp"
#include "state/userdata/identity.hpp"
#include "state/userdata/ownership.hpp"

#include <cstdint>
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
using Luna::Detail::NativeIdentity;
using Luna::Detail::OwnershipModel;
using Luna::Detail::OwnershipRequest;
using Luna::Detail::ReleaseCause;
using Luna::Detail::StagedStorage;
using Luna::Detail::UserdataHeader;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "userdata collection check failed: " << Description << '\n';
}

// One representative native object, plus the exact number of times Luna ran
// each storage step on it.
struct Probe final {
  int Value = 11;
};

int DestroyCalls = 0;
int DeallocateCalls = 0;
bool DestroyThrows = false;
bool DeallocateThrows = false;

void ResetStorageCounters() {
  DestroyCalls = 0;
  DeallocateCalls = 0;
  DestroyThrows = false;
  DeallocateThrows = false;
}

[[nodiscard]] Probe *AllocateProbe() {
  Probe *Storage = static_cast<Probe *>(::operator new(sizeof(Probe)));
  new (Storage) Probe{};
  return Storage;
}

// The semantic protocol of storage Luna owns. Either of its two steps can be
// told to report a failure, which is how a collection proves it contains one.
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
      throw std::runtime_error("probe destruction failed during collection");
    static_cast<Probe *>(Storage)->~Probe();
    return AllocatorStepResult::Done();
  };
  ClassAllocator::DeallocateOperation Deallocate = [](void *Storage,
                                                      const StorageRequest &) {
    ++DeallocateCalls;
    if (DeallocateThrows) {
      // The storage is still released: only the consumer's report of it
      // throws.
      ::operator delete(Storage);
      throw std::runtime_error("probe deallocation reported a failure");
    }
    ::operator delete(Storage);
    return AllocatorStepResult::Done();
  };
  return ClassAllocator::FromOperations(
      "Studio.CollectedProbeStorage", StorageRequest::ForClass<Probe>(),
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

// One State with one registered class, so every value a test exposes carries a
// complete, real class identity.
class Fixture final {
public:
  Fixture() {
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Probe> Class = Registry.RegisterClass<Probe>(
        "Probe", Luna::StableTypeKey("Studio.CollectedProbe"));
    Check(Class.Commit().IsSuccess(), "the probe class registers");
  }

  [[nodiscard]] Luna::State &StateObject() noexcept { return Owner; }

  [[nodiscard]] Luna::Detail::ReleaseCounters Counters() const noexcept {
    return Hooks::UserdataReleaseCounters(Owner);
  }

private:
  Luna::State Owner;
};

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

[[nodiscard]] Luna::Detail::ClassValueWriteObservation
ExposeOwned(Luna::State &Host, const std::string &Path, void *Storage) {
  return ExposeValue(Host, Path, Storage, OwnershipModel::LuaOwned,
                     Luna::LifetimeHandle::Undeclared(), nullptr,
                     OwnedStorageProtocol());
}

// Drops every script-visible reference to one path, so the only thing left
// holding the value is the weak identity slot the cache keeps.
[[nodiscard]] bool DropReference(Luna::State &Host, const std::string &Path) {
  return Host.Execute(Path + " = nil").IsSuccess();
}

// The collection boundary exists before any value does, and Luau spells it as
// the destructor of Luna's own userdata tag rather than as a field a script
// could read or replace.
void CheckCollectorIsInstalled() {
  Fixture Owner;
  Luna::State &Host = Owner.StateObject();
  Check(Hooks::UserdataCollectorIsInstalled(Host),
        "a ready State has Luna's contained collection boundary installed");
  Check(!Hooks::ClassMetatableIsCreated(Host, "Probe"),
        "registering a class still installs nothing in the virtual machine");

  // Nothing about the boundary is reachable from a script: the metatable is
  // protected, and the collector is not a metatable field at all.
  Check(Host.Execute("Probed = getmetatable(newproxy(true))").IsSuccess(),
        "a script may still use its own proxies");
}

// A collected value ends exactly the way an explicitly released one does: one
// invalidation, one cache eviction, one destruction, one deallocation, one
// metadata release.
void CheckCollectionReleasesThroughTheGate() {
  ResetStorageCounters();
  Hooks::ResetUserdataCollections();

  Fixture Owner;
  Luna::State &Host = Owner.StateObject();
  Probe *Storage = AllocateProbe();

  const auto Published = ExposeOwned(Host, "Owned", Storage);
  Check(Published.Published && Published.Failure == "none",
        "a Lua-owned value publishes through the write path");
  Check(Hooks::OwnedUserdataCount(Host) == 1 &&
            Hooks::PublishedUserdataCount(Host) == 1,
        "the exposed object has exactly one owner");
  Check(Hooks::LiveCachedIdentityCount(Host) == 1,
        "the exposed object has exactly one live cache entry");
  Check(Hooks::ClassMetatableCreationCount(Host, "Probe") == 1,
        "the first exposure creates exactly one class metatable");

  const auto BeforeRelease = Owner.Counters();
  Check(DropReference(Host, "Owned"),
        "the script drops its reference to the value");
  Check(DestroyCalls == 0, "dropping a reference releases nothing by itself");

  Check(Hooks::CollectGarbage(Host), "the collector runs to completion");
  const auto Collected = Hooks::ObserveUserdataCollections();
  Check(Collected.Released == 1,
        "collection routed exactly one value into the release gate");
  Check(Collected.ContainedException == 0,
        "nothing was thrown at the collection boundary");
  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "a collected Lua-owned value is destroyed and deallocated exactly "
        "once");

  const auto AfterRelease = Owner.Counters();
  Check(AfterRelease.Invalidate == BeforeRelease.Invalidate + 1,
        "collection invalidates access exactly once, before anything is "
        "released");
  Check(AfterRelease.CacheRemoval == BeforeRelease.CacheRemoval + 1,
        "collection removes the identity-cache entry exactly once");
  Check(AfterRelease.MetadataRelease == BeforeRelease.MetadataRelease + 1,
        "collection releases the value's metadata exactly once");
  Check(AfterRelease.IncompleteMetadata == 0,
        "every cleanup step ran with the metadata it needs");
  Check(Hooks::OwnedUserdataCount(Host) == 0,
        "a collected value leaves no owner behind");
  Check(Hooks::LiveCachedIdentityCount(Host) == 0,
        "a collected value leaves no live cache entry behind");

  // The class metatable is retained through collection, so a value exposed
  // afterwards carries exactly the same one.
  Probe *Reborn = AllocateProbe();
  Check(ExposeOwned(Host, "Reborn", Reborn).Published,
        "a value exposed after collection publishes");
  Check(Hooks::ClassMetatableCreationCount(Host, "Probe") == 1,
        "collecting every value of a class keeps its one metatable");
  Check(Host.Execute("return 1").IsSuccess(),
        "the State stays usable after a collection");
}

// Nothing a consumer's cleanup step throws may reach the virtual machine, and
// every remaining step still runs exactly once.
void CheckCollectionContainsExceptions() {
  ResetStorageCounters();
  Hooks::ResetUserdataCollections();

  Fixture Owner;
  Luna::State &Host = Owner.StateObject();
  Probe *Storage = AllocateProbe();
  Check(ExposeOwned(Host, "Throwing", Storage).Published,
        "the value publishes before its destruction throws");

  DestroyThrows = true;
  DeallocateThrows = true;
  Check(DropReference(Host, "Throwing"),
        "the script drops its reference to the throwing value");

  // The collector is entered from inside ordinary script execution as well as
  // from an explicit collection, and neither one may see a C++ exception.
  Check(Host.Execute("local Held = {}\nfor Index = 1, 4096 do Held[Index] = "
                     "{ Index } end\nreturn #Held")
            .IsSuccess(),
        "an allocating script that may collect still succeeds");
  Check(Hooks::CollectGarbage(Host),
        "an explicit collection over a throwing value still completes");
  DestroyThrows = false;
  DeallocateThrows = false;

  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "a throwing destruction is contained and deallocation still runs "
        "once");
  const auto Counted = Owner.Counters();
  Check(Counted.ContainedException == 2,
        "the gate contains exactly the two exceptions its cleanup steps threw");
  Check(Counted.IncompleteMetadata == 0,
        "a contained cleanup exception still keeps cleanup metadata valid");
  Check(Hooks::OwnedUserdataCount(Host) == 0,
        "a contained cleanup exception still releases the value");
  Check(Hooks::ObserveUserdataCollections().Released == 1,
        "the contained exception did not stop the boundary from releasing the "
        "value");

  // The State is not poisoned by a contained cleanup exception.
  Check(Host.Execute("Recovered = 7\nreturn Recovered").IsSuccess(),
        "the State executes again after a contained cleanup exception");
  Probe *Another = AllocateProbe();
  Check(ExposeOwned(Host, "Recovered", Another).Published,
        "the State exposes another value after a contained cleanup exception");
}

// Collection is one more cause of the one idempotent gate, so a value an
// explicit release already ended is released exactly once in total.
void CheckCollectionAfterExplicitReleaseDoesNothing() {
  ResetStorageCounters();
  Hooks::ResetUserdataCollections();

  Fixture Owner;
  Luna::State &Host = Owner.StateObject();
  Probe *Storage = AllocateProbe();
  Check(ExposeOwned(Host, "Explicit", Storage).Published,
        "the value publishes before it is released explicitly");

  Check(Hooks::ReleaseClassValue(Host, Storage,
                                 ReleaseCause::ExplicitInvalidation),
        "an explicit invalidation releases the value once");
  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "the explicit release destroyed and deallocated exactly once");

  Check(DropReference(Host, "Explicit"),
        "the script drops its reference to the released value");
  Check(Hooks::CollectGarbage(Host), "the collector runs to completion");

  const auto Counted = Hooks::ObserveUserdataCollections();
  Check(Counted.Entered >= 1, "the collector was entered for the block");
  Check(Counted.Released == 0 && Counted.AlreadyReleased >= 1,
        "collecting an already released value performs no second release");
  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "no second destruction and no second deallocation ever run");
  Check(Counted.ForeignBlock == 0 && Counted.UnroutedOrigin == 0,
        "every collected block was Luna's and found its own State's gate");
}

// Luna never destroys an object it only borrows, whichever cause ends the
// value - collection included.
void CheckCollectingBorrowedValuesDestroysNothing() {
  ResetStorageCounters();
  Hooks::ResetUserdataCollections();

  Fixture Owner;
  Luna::State &Host = Owner.StateObject();
  Probe Borrowed;
  Luna::LifetimeHandle Handle;

  Check(ExposeValue(Host, "Borrowed", &Borrowed, OwnershipModel::Borrowed,
                    Handle, nullptr, ClassAllocator())
            .Published,
        "a borrowed value with a live handle publishes");

  const auto Before = Owner.Counters();
  Check(DropReference(Host, "Borrowed"),
        "the script drops its reference to the borrowed value");
  Check(Hooks::CollectGarbage(Host), "the collector runs to completion");

  const auto After = Owner.Counters();
  Check(Hooks::ObserveUserdataCollections().Released == 1,
        "collecting a borrowed value routes it into the gate exactly once");
  Check(DestroyCalls == 0 && DeallocateCalls == 0,
        "collecting a borrowed value destroys and deallocates nothing");
  Check(After.Destroy == Before.Destroy &&
            After.Deallocate == Before.Deallocate,
        "no owned release step ran for a borrowed value");
  Check(After.MetadataRelease == Before.MetadataRelease + 1,
        "the borrowed value's metadata is released exactly once");
  Check(Borrowed.Value == 11, "the borrowed object is left exactly as it was");
  Check(Hooks::OwnedUserdataCount(Host) == 0,
        "a collected borrowed value leaves no owner behind");
  Check(Handle.IsValid(),
        "collecting a borrowed value never invalidates the owner's lifetime");
}

// A shared value releases exactly one corresponding shared ownership reference
// when it is collected, and never more than one.
void CheckCollectingSharedValuesReleasesOneReference() {
  ResetStorageCounters();
  Hooks::ResetUserdataCollections();

  Fixture Owner;
  Luna::State &Host = Owner.StateObject();
  std::shared_ptr<Probe> Object = std::make_shared<Probe>();

  Check(ExposeValue(Host, "Shared", Object.get(), OwnershipModel::Shared,
                    Luna::LifetimeHandle::Undeclared(), Object,
                    ClassAllocator())
            .Published,
        "a shared value publishes with its one shared ownership reference");
  Check(Object.use_count() == 2,
        "Luna retains exactly one shared ownership reference");

  const auto Before = Owner.Counters();
  Check(DropReference(Host, "Shared"),
        "the script drops its reference to the shared value");
  Check(Hooks::CollectGarbage(Host), "the collector runs to completion");

  const auto After = Owner.Counters();
  Check(After.SharedRelease == Before.SharedRelease + 1,
        "collection releases exactly one shared ownership reference");
  Check(After.Destroy == Before.Destroy &&
            After.Deallocate == Before.Deallocate,
        "a shared value is never destroyed or deallocated by Luna");
  Check(Object.use_count() == 1,
        "the consumer is left holding exactly its own reference");
}

// State destruction refuses every new invocation first, then closes and
// finalizes the machine while every piece of cleanup metadata is still valid,
// and releases each userdata resource exactly once.
void CheckStateDestructionOrdering() {
  ResetStorageCounters();
  Hooks::ResetUserdataCollections();

  {
    Fixture Owner;
    Luna::State &Host = Owner.StateObject();
    for (int Index = 0; Index < 3; ++Index) {
      Probe *Storage = AllocateProbe();
      const std::string Path = "Owned" + std::to_string(Index);
      Check(ExposeOwned(Host, Path, Storage).Published,
            "each Lua-owned value publishes and stays reachable");
    }
    Check(Hooks::OwnedUserdataCount(Host) == 3 &&
              Hooks::PublishedUserdataCount(Host) == 3,
          "the State owns exactly the values it published");
    Check(DestroyCalls == 0,
          "nothing is released while the State is still usable");
  }

  Check(DestroyCalls == 3 && DeallocateCalls == 3,
        "State destruction releases each userdata resource exactly once");

  const auto Observed = Hooks::ObserveLastStateDestruction();
  Check(Observed.Observed, "the destruction recorded its own ordering");
  Check(Observed.RefusedNewInvocations,
        "no new invocation, access, or exposure is accepted once destruction "
        "begins");
  Check(Observed.RetainedCleanupMetadata,
        "every value still owned kept the metadata its cleanup needs");
  Check(Observed.ReleasedDuringClose == 3,
        "the machine finalized every value it held, while that metadata was "
        "still valid");
  Check(Observed.ReleasedAfterClose == 0,
        "nothing was left for the final sweep");
  Check(Observed.IncompleteMetadata == 0,
        "no cleanup step ran without the metadata it requires");
  Check(Hooks::ObserveUserdataCollections().ContainedException == 0,
        "nothing was thrown at the collection boundary during close");
}

// A value the machine never held is still released exactly once, by the same
// gate and with the same metadata, after the machine has closed.
void CheckStateDestructionReleasesValuesTheMachineNeverHeld() {
  ResetStorageCounters();
  Hooks::ResetUserdataCollections();

  {
    Fixture Owner;
    Luna::State &Host = Owner.StateObject();
    Luna::Detail::OwnershipRegistry *Gate = Hooks::UserdataOwnershipOf(Host);
    Check(Gate != nullptr, "the State owns one ownership and release gate");
    if (Gate == nullptr)
      return;

    // One value staged, constructed, owned, and published entirely outside the
    // virtual machine: no block exists, so no finalizer can ever run for it.
    Probe *Storage = AllocateProbe();
    const auto Described = Hooks::DescribeClassUserdata(
        Host, "Probe", OwnershipModel::LuaOwned, ConstAccess::Mutable);
    Check(Described.has_value(), "the registered class describes one header");
    if (!Described)
      return;

    UserdataHeader Header = *Described;
    StagedStorage Staged;
    Staged.Storage = Storage;
    Staged.Identity = IdentityFor(Storage);
    Staged.Allocator = OwnedStorageProtocol();
    Check(Gate->Expose(Header, Staged, OwnershipRequest{}).Succeeded,
          "the value the machine never holds still publishes");
    Check(Hooks::OwnedUserdataCount(Host) == 1,
          "the State owns exactly that value");
  }

  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "a value the machine never held is destroyed and deallocated exactly "
        "once");

  const auto Observed = Hooks::ObserveLastStateDestruction();
  Check(Observed.ReleasedDuringClose == 0,
        "the machine finalized nothing, because it held nothing");
  Check(Observed.ReleasedAfterClose == 1,
        "the final sweep released exactly the value the machine never held");
  Check(Observed.IncompleteMetadata == 0,
        "the final sweep ran with every piece of cleanup metadata still valid");
}

// A cleanup step that throws during State destruction is contained too: the
// remaining steps still run, and the destructor completes.
void CheckStateDestructionContainsExceptions() {
  ResetStorageCounters();
  Hooks::ResetUserdataCollections();

  {
    Fixture Owner;
    Luna::State &Host = Owner.StateObject();
    Probe *Storage = AllocateProbe();
    Check(ExposeOwned(Host, "Owned", Storage).Published,
          "the value publishes before destruction throws");
    DestroyThrows = true;
  }
  DestroyThrows = false;

  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "a throwing destruction during close is contained and deallocation "
        "still runs once");
  const auto Observed = Hooks::ObserveLastStateDestruction();
  Check(Observed.ReleasedDuringClose == 1,
        "the throwing value was still released exactly once");
  Check(Observed.IncompleteMetadata == 0,
        "a contained exception during close keeps cleanup metadata valid");
  Check(Hooks::ObserveUserdataCollections().ContainedException == 0,
        "the exception was contained by the gate, never by the boundary above "
        "it");
}

// A collected value of one State never reaches another State's gate, and a
// State that is gone is never reached at all.
void CheckCollectionIsIsolatedByState() {
  ResetStorageCounters();
  Hooks::ResetUserdataCollections();

  Fixture First;
  Probe *Held = AllocateProbe();
  Check(ExposeOwned(First.StateObject(), "Owned", Held).Published,
        "the first State exposes one Lua-owned value");

  {
    Fixture Second;
    Probe *Other = AllocateProbe();
    Check(ExposeOwned(Second.StateObject(), "Owned", Other).Published,
          "the second State exposes one Lua-owned value of its own");
  }
  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "destroying one State releases only its own value");
  Check(Hooks::OwnedUserdataCount(First.StateObject()) == 1,
        "the surviving State still owns its value");

  Check(DropReference(First.StateObject(), "Owned"),
        "the surviving State drops its reference");
  Check(Hooks::CollectGarbage(First.StateObject()),
        "the surviving State collects to completion");
  Check(DestroyCalls == 2 && DeallocateCalls == 2,
        "the surviving State's value is released exactly once, by its own "
        "gate");
  Check(Hooks::ObserveUserdataCollections().UnroutedOrigin == 0,
        "every collected value found the gate of the State that exposed it");
}

// The collection route is keyed by the logical State identity a move preserves,
// so a value exposed before a move is still collected by exactly the gate that
// owns it.
void CheckCollectionSurvivesStateMoves() {
  ResetStorageCounters();
  Hooks::ResetUserdataCollections();

  Luna::State First;
  {
    Luna::BindingRegistry Registry = First.Bindings();
    Luna::ClassBuilder<Probe> Class = Registry.RegisterClass<Probe>(
        "Probe", Luna::StableTypeKey("Studio.CollectedProbe"));
    Check(Class.Commit().IsSuccess(),
          "the probe class registers before the move");
  }

  Probe *Storage = AllocateProbe();
  Check(ExposeOwned(First, "Owned", Storage).Published,
        "the value publishes before the move");

  Luna::State Moved = std::move(First);
  Check(Hooks::OwnedUserdataCount(Moved) == 1,
        "the moved State still owns the value");
  Check(Hooks::UserdataCollectorIsInstalled(Moved),
        "the moved State still has the collection boundary installed");

  Check(DropReference(Moved, "Owned"),
        "the moved State drops its reference to the value");
  Check(Hooks::CollectGarbage(Moved), "the moved State collects to completion");
  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "a value exposed before a move is released exactly once after it");
  const auto Counted = Hooks::ObserveUserdataCollections();
  Check(Counted.Released == 1 && Counted.UnroutedOrigin == 0,
        "the value found exactly the gate of the State that exposed it");
}

} // namespace

int RunUserdataCollectionAndDestructionTests() {
  FailureCount = 0;
  CheckCollectorIsInstalled();
  CheckCollectionReleasesThroughTheGate();
  CheckCollectionContainsExceptions();
  CheckCollectionAfterExplicitReleaseDoesNothing();
  CheckCollectingBorrowedValuesDestroysNothing();
  CheckCollectingSharedValuesReleasesOneReference();
  CheckStateDestructionOrdering();
  CheckStateDestructionReleasesValuesTheMachineNeverHeld();
  CheckStateDestructionContainsExceptions();
  CheckCollectionIsIsolatedByState();
  CheckCollectionSurvivesStateMoves();
  Hooks::ResetUserdataCollections();
  return FailureCount == 0 ? 0 : 1;
}
