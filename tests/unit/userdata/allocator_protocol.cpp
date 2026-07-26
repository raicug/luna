// Focused coverage of the semantic allocator protocol and of the cleanup its
// milestones warrant.
//
// The protocol is four steps and nothing else: obtain suitably aligned storage,
// construct one object in it, destroy an object known to be constructed, give
// the storage back. A consumer supplies whichever of those steps it owns as an
// ordinary callable, and Luna erases every one of them - together with whatever
// state the callable captured - into one immutable record it retains until the
// last value created through it has finished its cleanup.
//
// What is checked here is that completed milestones decide cleanup exactly:
//
//   * allocation that produced nothing is cleaned up by nothing at all;
//   * construction that refused, or threw, gives the storage back without
//     destroying an object that was never constructed;
//   * ownership refused after construction destroys, then releases, then
//     deallocates, each exactly once;
//   * a published value's final release performs every applicable step exactly
//     once and a second release performs none of them;
//   * the retained protocol is still reachable at the final step, long after
//   the
//     allocator value the consumer held is gone.
//
// Every failure also has to leave the State exactly as it found it: nothing
// published, no owner behind, the stack restored, and the next construction
// succeeding.

// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/lifetime_handle.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/test_hooks.hpp"
#include "state/userdata/allocator.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/ownership.hpp"

#include <cstddef>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
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
using Luna::Detail::OwnershipModel;
using Luna::Detail::ReleaseCause;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "allocator protocol check failed: " << Description << '\n';
}

// One representative native object whose construction is observable, so "was
// constructed" and "was destroyed" are separately checkable rather than
// assumed.
struct Probe final {
  int Value = 0;
  bool IsLive = false;
};

int AllocateCalls = 0;
int ConstructCalls = 0;
int DestroyCalls = 0;
int DeallocateCalls = 0;

void ResetStorageCounters() {
  AllocateCalls = 0;
  ConstructCalls = 0;
  DestroyCalls = 0;
  DeallocateCalls = 0;
}

// What one generated protocol is asked to do. The state lives here and is
// captured by the steps themselves, which is exactly how a consumer's arena
// would travel with its protocol.
struct StoragePolicy final {
  bool AllocationFails = false;
  bool ConstructionRefuses = false;
  bool ConstructionThrows = false;
  int ConstructedValue = 41;
};

[[nodiscard]] ClassAllocator OwnedStorageProtocol(const StoragePolicy *Policy) {
  ClassAllocator::AllocateOperation Allocate =
      [Policy](const StorageRequest &Wanted) -> void * {
    ++AllocateCalls;
    if (Policy != nullptr && Policy->AllocationFails)
      return nullptr;
    return ::operator new(Wanted.ByteCount);
  };
  ClassAllocator::ConstructOperation Construct = [Policy](void *Storage) {
    ++ConstructCalls;
    if (Policy != nullptr && Policy->ConstructionThrows)
      throw std::runtime_error("probe construction reported a failure");
    if (Policy != nullptr && Policy->ConstructionRefuses)
      return AllocatorStepResult::Declined();

    Probe *Built = new (Storage) Probe{};
    Built->Value = Policy != nullptr ? Policy->ConstructedValue : 0;
    Built->IsLive = true;
    return AllocatorStepResult::Done();
  };
  ClassAllocator::DestroyOperation Destroy = [](void *Storage) {
    ++DestroyCalls;
    Probe *Built = static_cast<Probe *>(Storage);

    // A destruction step only ever sees an object Luna knows is constructed.
    Check(Built->IsLive, "destruction only ever runs on a constructed object");
    Built->IsLive = false;
    Built->~Probe();
    return AllocatorStepResult::Done();
  };
  ClassAllocator::DeallocateOperation Deallocate = [](void *Storage,
                                                      const StorageRequest &) {
    ++DeallocateCalls;
    ::operator delete(Storage);
    return AllocatorStepResult::Done();
  };
  return ClassAllocator::FromOperations(
      "Studio.ProbeArena", StorageRequest::ForClass<Probe>(),
      std::move(Allocate), std::move(Construct), std::move(Destroy),
      std::move(Deallocate));
}

// One State with one registered class, so every constructed value carries a
// complete, real class identity.
class Fixture final {
public:
  Fixture() {
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Probe> Class = Registry.RegisterClass<Probe>(
        "Probe", Luna::StableTypeKey("Studio.AllocatedProbe"));
    const Luna::RegistrationResult Published = Class.Commit();
    Check(Published.IsSuccess(), "the probe class registers");
  }

  [[nodiscard]] Luna::State &StateObject() noexcept { return Owner; }

  [[nodiscard]] Luna::Detail::ReleaseCounters Released() const noexcept {
    return Hooks::UserdataReleaseCounters(Owner);
  }

  [[nodiscard]] Luna::Detail::ConstructionCounters Built() const noexcept {
    return Hooks::UserdataConstructionCounters(Owner);
  }

private:
  Luna::State Owner;
};

[[nodiscard]] Luna::Detail::ClassValueWriteObservation ConstructValue(
    Luna::State &Host, const std::string &Path, const ClassAllocator &Allocator,
    OwnershipModel Ownership = OwnershipModel::LuaOwned,
    const Luna::LifetimeHandle &Handle = Luna::LifetimeHandle::Undeclared()) {
  Luna::Detail::ClassValueConstructionRequest Request;
  Request.QualifiedName = "Probe";
  Request.Path = Path;
  Request.Ownership = Ownership;
  Request.Access = ConstAccess::Mutable;
  Request.Allocator = Allocator;
  Request.Handle = Handle;
  return Hooks::ConstructClassValue(Host, Request);
}

// The object one published value carries, named through its own header rather
// than through any address the test kept.
[[nodiscard]] void *ConstructedObjectAt(const Luna::State &Host,
                                        const std::string &Path) {
  const std::optional<Luna::Detail::UserdataHeader> Header =
      Hooks::ObserveClassUserdata(Host, Path);
  return Header ? Header->Payload.Storage : nullptr;
}

void CheckNothingWasPublished(
    Luna::State &Host, const std::string &Path,
    const Luna::Detail::ClassValueWriteObservation &Observed,
    std::string_view Description) {
  Check(!Observed.Published && Observed.PublishedCount == 0, Description);
  Check(Observed.FinalStackDepth == Observed.EntryStackDepth,
        "a refused construction leaves the stack exactly as it found it");
  Check(Hooks::OwnedUserdataCount(Host) == 0,
        "a refused construction leaves no owner behind");
  Check(Hooks::PublishedUserdataCount(Host) == 0,
        "a refused construction publishes nothing");
  Check(Hooks::CachedIdentityCount(Host) == 0,
        "a refused construction records no identity-cache entry");
  Check(!Hooks::ObserveClassUserdata(Host, Path).has_value(),
        "a refused construction installs no value at the path it named");
}

// The public protocol answers what it declares, and nothing about it needs a
// virtual machine to be true.
void CheckDeclaredStepsDecideTheProtocol() {
  const ClassAllocator Undeclared;
  Check(!Undeclared.IsDeclared() && !Undeclared.OwnsStorage() &&
            !Undeclared.DeclaresAllocation() &&
            !Undeclared.DeclaresConstruction() &&
            !Undeclared.DeclaresDestruction(),
        "a default-constructed allocator declares no step at all");
  Check(Undeclared.PolicyIdentity().empty() && !Undeclared.Storage().IsUsable(),
        "an undeclared protocol names no policy and no storage");

  const StoragePolicy Policy;
  const ClassAllocator Owned = OwnedStorageProtocol(&Policy);
  Check(Owned.IsDeclared() && Owned.DeclaresAllocation() &&
            Owned.DeclaresConstruction() && Owned.DeclaresDestruction() &&
            Owned.OwnsStorage(),
        "a complete protocol declares all four semantic steps");
  Check(Owned.PolicyIdentity() == "Studio.ProbeArena",
        "the protocol reflects the policy identity its consumer named");
  Check(Owned.Storage().ByteCount == sizeof(Probe) &&
            Owned.Storage().Alignment == alignof(Probe) &&
            Owned.Storage().IsUsable(),
        "the protocol carries the declared storage of its class");

  const ClassAllocator Copied = Owned;
  Check(Copied.RefersToSame(Owned) && !Copied.RefersToSame(Undeclared),
        "copies of one allocator name exactly the same protocol");

  // Luna's own protocol for a complete class type declares the same four steps
  // without the consumer writing any of them.
  const ClassAllocator Ordinary = ClassAllocator::ForOwnedObject<Probe>();
  Check(Ordinary.DeclaresAllocation() && Ordinary.DeclaresConstruction() &&
            Ordinary.DeclaresDestruction() && Ordinary.OwnsStorage(),
        "the ordinary protocol of a class declares every step");

  // An adopted object is destroyed and never deallocated, which is the whole
  // difference between owning an object and owning its storage.
  const ClassAllocator Adopted = ClassAllocator::ForAdoptedObject<Probe>();
  Check(Adopted.DeclaresDestruction() && !Adopted.OwnsStorage() &&
            !Adopted.DeclaresAllocation(),
        "an adopted object's protocol destroys but never deallocates");
}

// One complete construction: every milestone in order, every step exactly once,
// and the object the script holds is the object the construction step built.
void CheckCompleteConstructionPublishesOnce() {
  ResetStorageCounters();
  {
    Fixture Owner;
    Luna::State &Host = Owner.StateObject();
    StoragePolicy Policy;
    Policy.ConstructedValue = 23;

    const auto Published =
        ConstructValue(Host, "Owned", OwnedStorageProtocol(&Policy));
    Check(Published.Published && Published.PublishedCount == 1 &&
              Published.Failure == "none",
          "a complete construction publishes exactly one value");
    Check(AllocateCalls == 1 && ConstructCalls == 1,
          "the storage is allocated once and the object constructed once");
    Check(DestroyCalls == 0 && DeallocateCalls == 0,
          "a published value has nothing cleaned up yet");

    const auto Built = Owner.Built();
    Check(Built.Allocate == 1 && Built.Construct == 1 &&
              Built.AllocationFailure == 0 && Built.ConstructionFailure == 0 &&
              Built.ContainedException == 0,
          "the State counts exactly one allocation and one construction");

    void *Object = ConstructedObjectAt(Host, "Owned");
    Check(Object != nullptr, "the published value names the object it carries");
    Check(Object != nullptr && static_cast<Probe *>(Object)->Value == 23,
          "the object the value carries is the one the step constructed");

    const auto Header = Hooks::ObserveClassUserdata(Host, "Owned");
    Check(Header && Header->Lifetime == LifetimeState::Published &&
              Header->Ownership == OwnershipModel::LuaOwned,
          "a constructed value is published and Lua-owned");
    Check(Header && Header->Allocator.IsDeclared(),
          "the value names the immutable allocator record its cleanup uses");

    Luna::Detail::ClassAccessRequest Read;
    Read.QualifiedName = "Probe";
    Read.Path = "Owned";
    Read.ExpectedStorage = Object;
    Check(Hooks::AccessClassUserdata(Host, Read).DeliveredExpectedObject,
          "the constructed object reaches native code through the value");

    // The final release performs every applicable step exactly once, and a
    // second release performs none of them.
    Check(Hooks::ReleaseClassValue(Host, Object, ReleaseCause::LifecycleAction),
          "the constructed value releases once");
    Check(DestroyCalls == 1 && DeallocateCalls == 1,
          "final release destroys once and deallocates once");
    Check(
        !Hooks::ReleaseClassValue(Host, Object, ReleaseCause::LifecycleAction),
        "releasing an already released value does nothing");
    Check(DestroyCalls == 1 && DeallocateCalls == 1,
          "a second release performs no second step");
    Check(Owner.Released().IncompleteMetadata == 0,
          "every cleanup step ran with the metadata it needs");
  }
  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "State destruction performs no second release");
}

// Luna's own protocol for a complete class type, with no construction step
// supplied by the caller at all: the protocol's own step is the one that runs.
void CheckOrdinaryProtocolConstructsAndReleases() {
  Fixture Owner;
  Luna::State &Host = Owner.StateObject();

  const auto Published =
      ConstructValue(Host, "Ordinary", ClassAllocator::ForOwnedObject<Probe>());
  Check(Published.Published && Published.Failure == "none",
        "the ordinary protocol of a class constructs and publishes a value");

  void *Object = ConstructedObjectAt(Host, "Ordinary");
  Check(Object != nullptr && !static_cast<Probe *>(Object)->IsLive &&
            static_cast<Probe *>(Object)->Value == 0,
        "the ordinary protocol default-constructs the object");
  Check(Hooks::ReleaseClassValue(Host, Object, ReleaseCause::GarbageCollection),
        "the ordinary protocol releases its value once");
  Check(Owner.Released().Destroy == 1 && Owner.Released().Deallocate == 1,
        "the ordinary protocol destroys once and deallocates once");
}

// Allocation that produced nothing: the one failure the protocol answers with
// no cleanup call whatsoever.
void CheckAllocationFailureCleansUpNothing() {
  ResetStorageCounters();
  Fixture Owner;
  Luna::State &Host = Owner.StateObject();
  StoragePolicy Policy;
  Policy.AllocationFails = true;

  const auto Refused =
      ConstructValue(Host, "Unallocated", OwnedStorageProtocol(&Policy));
  CheckNothingWasPublished(Host, "Unallocated", Refused,
                           "a construction whose storage never existed "
                           "publishes nothing");
  Check(AllocateCalls == 1 && ConstructCalls == 0,
        "no object is constructed in storage that does not exist");
  Check(DestroyCalls == 0 && DeallocateCalls == 0,
        "an allocation that produced nothing is cleaned up by nothing");

  const auto Built = Owner.Built();
  Check(Built.AllocationFailure == 1 && Built.Allocate == 0 &&
            Built.Construct == 0,
        "the State counts exactly one refused allocation");
  Check(Owner.Released().MetadataRelease == 0,
        "no release step runs for a value that was never staged");

  // The same path publishes as soon as its storage exists.
  StoragePolicy Working;
  Check(ConstructValue(Host, "Unallocated", OwnedStorageProtocol(&Working))
            .Published,
        "the State constructs a value after a refused allocation");
}

// Construction that refused, and construction that threw: both mean no object
// exists, so both give the storage back without destroying anything.
void CheckConstructionFailureDeallocatesWithoutDestroying() {
  for (int Variant = 0; Variant < 2; ++Variant) {
    ResetStorageCounters();
    Fixture Owner;
    Luna::State &Host = Owner.StateObject();
    StoragePolicy Policy;
    Policy.ConstructionRefuses = Variant == 0;
    Policy.ConstructionThrows = Variant == 1;

    const auto Refused =
        ConstructValue(Host, "Unconstructed", OwnedStorageProtocol(&Policy));
    CheckNothingWasPublished(Host, "Unconstructed", Refused,
                             "a construction that produced no object "
                             "publishes nothing");
    Check(AllocateCalls == 1 && ConstructCalls == 1,
          "the construction step ran exactly once on the storage it was given");
    Check(DestroyCalls == 0,
          "storage nothing was constructed in is never destroyed");
    Check(DeallocateCalls == 1,
          "construction failure gives the storage back exactly once");

    const auto Built = Owner.Built();
    Check(Built.Allocate == 1 && Built.Construct == 0 &&
              Built.ConstructionFailure == 1,
          "the State counts exactly one refused construction");
    Check(Built.ContainedException == (Policy.ConstructionThrows ? 1U : 0U),
          "a construction step that throws is contained and counted once");

    const auto Released = Owner.Released();
    Check(Released.Invalidate == 0,
          "a value that was never accessible needs no invalidation");
    Check(Released.Destroy == 0 && Released.Deallocate == 1 &&
              Released.MetadataRelease == 1,
          "construction failure releases the storage and nothing else");
    Check(Released.IncompleteMetadata == 0,
          "the cleanup of a failed construction kept its metadata");
    Check(Host.Execute("return 1").IsSuccess(),
          "the State stays usable after a refused construction");

    StoragePolicy Working;
    Check(ConstructValue(Host, "Unconstructed", OwnedStorageProtocol(&Working))
              .Published,
          "the State constructs a complete value afterwards");
  }
}

// Ownership refused after construction succeeded: the object is destroyed, then
// what ownership took is released, then the storage is deallocated - each step
// exactly once, and in that order.
void CheckOwnershipFailureDestroysBeforeDeallocating() {
  ResetStorageCounters();
  Fixture Owner;
  Luna::State &Host = Owner.StateObject();
  Luna::Detail::OwnershipRegistry *Gate = Hooks::UserdataOwnershipOf(Host);
  Check(Gate != nullptr, "the State owns one release gate");
  if (Gate == nullptr)
    return;

  StoragePolicy Policy;
  Policy.ConstructedValue = 13;
  const ClassAllocator Allocator = OwnedStorageProtocol(&Policy);

  // Exactly the steps a constructor candidate takes: allocate, stage,
  // construct.
  const Luna::Detail::StorageAllocationOutcome Allocated =
      Gate->Allocate(Allocator);
  Check(Allocated.Succeeded() && AllocateCalls == 1,
        "the allocation step produced storage exactly once");

  const auto Described = Hooks::DescribeClassUserdata(
      Host, "Probe", OwnershipModel::LuaOwned, ConstAccess::Mutable);
  Check(Described.has_value(), "the class describes the header of its values");
  if (!Described)
    return;

  Luna::Detail::UserdataHeader Header = *Described;
  Luna::Detail::StagedStorage Staged;
  Staged.Storage = Allocated.Storage;
  Staged.Identity.Address = Allocated.Storage;
  Staged.Identity.Nonce = 1;
  Staged.Allocator = Allocator;
  Check(Gate->Stage(Header, Staged).Succeeded,
        "the allocated storage stages before anything is constructed in it");
  Check(Header.Allocator.IsDeclared(),
        "the staged value names the immutable allocator record");

  Check(Gate->Construct(Header, Luna::Detail::ObjectConstruction()).Succeeded,
        "the protocol's own construction step constructs the object once");
  Check(ConstructCalls == 1 && DestroyCalls == 0 && DeallocateCalls == 0,
        "a constructed object has nothing cleaned up yet");

  // A Lua-owned object is never given a lifetime handle, so this statement is
  // one Luna refuses - after construction has already happened.
  Luna::Detail::OwnershipRequest Request;
  Request.Handle = Luna::LifetimeHandle();
  const auto Refused = Gate->Establish(Header, Request);
  Check(!Refused.Succeeded &&
            Refused.Failure ==
                Luna::Detail::OwnershipFailure::UnexpectedLifetimeHandle,
        "an ownership statement that does not match its model is refused");

  Check(Gate->Release(Header, ReleaseCause::PublicationFailure),
        "the refused value releases once");
  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "a constructed object is destroyed once and its storage given back "
        "once");
  Check(Header.Lifetime == LifetimeState::Released,
        "the refused value ends released");
  Check(Hooks::OwnedUserdataCount(Host) == 0,
        "a refused ownership leaves no owner behind");

  const auto Released = Owner.Released();
  Check(Released.Invalidate == 1,
        "ownership failure invalidates access before releasing anything");
  Check(Released.CacheRemoval == 1,
        "ownership failure removes the identity-cache entry once");
  Check(Released.Destroy == 1 && Released.Deallocate == 1 &&
            Released.MetadataRelease == 1,
        "every applicable release step runs exactly once");
  Check(Released.ContainedException == 0 && Released.IncompleteMetadata == 0,
        "the cleanup of a refused ownership kept its metadata and threw "
        "nothing");
}

// Storage the protocol allocated and staging then refused: no record ever
// described it, so it is given straight back - and nothing was constructed in
// it, so nothing is destroyed.
void CheckRefusedStagingGivesTheStorageBack() {
  ResetStorageCounters();
  Fixture Owner;
  Luna::State &Host = Owner.StateObject();
  StoragePolicy Policy;

  // Luna never owns the storage of an object it only borrows, and a protocol
  // that would deallocate it says exactly that.
  const Luna::LifetimeHandle Lifetime;
  const auto Refused =
      ConstructValue(Host, "Borrowed", OwnedStorageProtocol(&Policy),
                     OwnershipModel::Borrowed, Lifetime);
  CheckNothingWasPublished(Host, "Borrowed", Refused,
                           "a borrowed value Luna would have to deallocate "
                           "publishes nothing");
  Check(AllocateCalls == 1 && ConstructCalls == 0,
        "nothing is constructed in storage staging refused");
  Check(DestroyCalls == 0 && DeallocateCalls == 1,
        "storage that was never staged is given straight back, exactly once");
  Check(Owner.Released().MetadataRelease == 0,
        "no record was ever created, so no metadata is released");
}

// The protocol outlives the consumer's own allocator value: Luna retains the
// immutable record until the last value created through it completes cleanup.
void CheckRetainedProtocolOutlivesItsAllocatorValue() {
  ResetStorageCounters();
  Fixture Owner;
  Luna::State &Host = Owner.StateObject();
  auto Policy = std::make_unique<StoragePolicy>();
  Policy->ConstructedValue = 7;

  {
    // The consumer's allocator value is gone before the value is ever released,
    // and its steps still run.
    const ClassAllocator Temporary = OwnedStorageProtocol(Policy.get());
    Check(ConstructValue(Host, "Retained", Temporary).Published,
          "the value constructs through the consumer's own protocol");
  }

  void *Object = ConstructedObjectAt(Host, "Retained");
  Check(Object != nullptr && static_cast<Probe *>(Object)->Value == 7,
        "the constructed object is the one the protocol built");
  Check(Hooks::ReleaseClassValue(Host, Object, ReleaseCause::StateDestruction),
        "the value releases once through the retained protocol");
  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "the retained protocol's steps still ran exactly once each");
  Check(Owner.Released().IncompleteMetadata == 0,
        "no cleanup step ran without the protocol it required");
}

// A value Luna neither created nor releases still names a protocol - the one
// that declares no step - so its cleanup decisions are readable and empty.
void CheckBorrowedValuesNameTheEmptyProtocol() {
  ResetStorageCounters();
  Fixture Owner;
  Luna::State &Host = Owner.StateObject();
  Probe Borrowed;
  Borrowed.Value = 5;
  Borrowed.IsLive = true;
  const Luna::LifetimeHandle Lifetime;

  Luna::Detail::ClassValueExposureRequest Request;
  Request.QualifiedName = "Probe";
  Request.Path = "Borrowed";
  Request.Storage = &Borrowed;
  Request.Ownership = OwnershipModel::Borrowed;
  Request.Access = ConstAccess::Mutable;
  Request.Handle = Lifetime;
  Check(Hooks::ExposeClassValue(Host, Request).Published,
        "a borrowed value publishes with no protocol of its own");

  const auto Header = Hooks::ObserveClassUserdata(Host, "Borrowed");
  Check(Header && Header->Allocator.IsDeclared(),
        "even a borrowed value names the protocol it was created under");
  Check(
      Hooks::ReleaseClassValue(Host, &Borrowed, ReleaseCause::LifecycleAction),
      "the borrowed value releases once");
  Check(DestroyCalls == 0 && DeallocateCalls == 0,
        "an empty protocol destroys nothing and deallocates nothing");
  Check(Borrowed.Value == 5 && Borrowed.IsLive,
        "the borrowed object is left exactly as it was");
  Check(Owner.Released().Destroy == 0 && Owner.Released().Deallocate == 0 &&
            Owner.Released().MetadataRelease == 1,
        "only the metadata of a borrowed value is ever released");
}

} // namespace

int RunUserdataAllocatorProtocolTests() {
  FailureCount = 0;
  CheckDeclaredStepsDecideTheProtocol();
  CheckCompleteConstructionPublishesOnce();
  CheckOrdinaryProtocolConstructsAndReleases();
  CheckAllocationFailureCleansUpNothing();
  CheckConstructionFailureDeallocatesWithoutDestroying();
  CheckOwnershipFailureDestroysBeforeDeallocating();
  CheckRefusedStagingGivesTheStorageBack();
  CheckRetainedProtocolOutlivesItsAllocatorValue();
  CheckBorrowedValuesNameTheEmptyProtocol();
  return FailureCount == 0 ? 0 : 1;
}
