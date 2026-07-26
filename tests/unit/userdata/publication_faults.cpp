// Focused coverage of what happens when exposing a class value fails partway
// through, and of the exact release accounting each failure produces.
//
// Luna's exposure path has no injectable fault point of its own: the write path
// is not the return writer, so none of the transaction or return-writer fault
// points reach it. What it does have is a fixed set of refusals that fail after
// real work has already been done, which is exactly the situation an injected
// publication fault would model:
//
//   * A refusal that fails during staging, before anything was staged at all.
//   * A refusal that fails during ownership establishment, after the value was
//     staged and recorded as constructed, so the release gate has to undo
//     precisely what the earlier steps established.
//
// Each case asserts the same three things a publication fault must guarantee -
// nothing is published, no owner is left behind, and the stack is restored
// exactly - plus the exact number of times each release step ran, that the
// identity cache recorded nothing, and that the State keeps exposing and
// reading values afterwards.
//
// An ownership statement Luna could never honor is Luna's own mistake rather
// than a consumer's, so its deterministic reason is reported through the
// refusal token the write path returns; the rendered message stays an internal
// error, exactly as every other internal conversion failure does.

// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/lifetime_handle.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/test_hooks.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/ownership.hpp"

#include <iostream>
#include <memory>
#include <new>
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
using Luna::Detail::OwnershipModel;
using Luna::Detail::ReleaseCause;
using Luna::Detail::ReleaseCounters;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "userdata publication check failed: " << Description << '\n';
}

struct Probe final {
  int Value = 5;
};

int DestroyCalls = 0;
int DeallocateCalls = 0;

void ResetStorageCounters() {
  DestroyCalls = 0;
  DeallocateCalls = 0;
}

[[nodiscard]] Probe *AllocateProbe() {
  Probe *Storage = static_cast<Probe *>(::operator new(sizeof(Probe)));
  new (Storage) Probe{};
  return Storage;
}

[[nodiscard]] ClassAllocator::DestroyOperation ProbeDestruction() {
  return [](void *Storage) {
    ++DestroyCalls;
    static_cast<Probe *>(Storage)->~Probe();
    return AllocatorStepResult::Done();
  };
}

[[nodiscard]] ClassAllocator::DeallocateOperation ProbeDeallocation() {
  return [](void *Storage, const StorageRequest &) {
    ++DeallocateCalls;
    ::operator delete(Storage);
    return AllocatorStepResult::Done();
  };
}

// The whole protocol of storage Luna owns: it destroys the object and gives the
// storage back.
[[nodiscard]] ClassAllocator OwnedStorageProtocol() {
  return ClassAllocator::FromOperations(
      "Studio.ProbeStorage", StorageRequest::ForClass<Probe>(),
      ClassAllocator::AllocateOperation(), ClassAllocator::ConstructOperation(),
      ProbeDestruction(), ProbeDeallocation());
}

// Owned storage Luna could never destroy: the deallocation step is declared, so
// staging accepts it, and only ownership establishment can refuse it.
[[nodiscard]] ClassAllocator UndestroyableStorageProtocol() {
  return ClassAllocator::FromOperations(
      "Studio.UndestroyableProbeStorage", StorageRequest::ForClass<Probe>(),
      ClassAllocator::AllocateOperation(), ClassAllocator::ConstructOperation(),
      ClassAllocator::DestroyOperation(), ProbeDeallocation());
}

// One State with one registered class, so every attempted exposure carries a
// complete, real class identity.
class Fixture final {
public:
  Fixture() {
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Probe> Class = Registry.RegisterClass<Probe>(
        "Probe", Luna::StableTypeKey("Studio.PublishedProbe"));
    const Luna::RegistrationResult Published = Class.Commit();
    Check(Published.IsSuccess(), "the probe class registers");
  }

  [[nodiscard]] Luna::State &StateObject() noexcept { return Owner; }

  [[nodiscard]] ReleaseCounters Counters() const noexcept {
    return Hooks::UserdataReleaseCounters(Owner);
  }

private:
  Luna::State Owner;
};

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

[[nodiscard]] bool Contains(const std::string &Text, std::string_view Needle) {
  return Text.find(Needle) != std::string::npos;
}

// Nothing exists at the path a refused exposure named, and nothing about the
// State's own accounting moved.
void CheckNothingWasPublished(
    Luna::State &Host, const std::string &Path,
    const Luna::Detail::ClassValueWriteObservation &Observed,
    std::string_view Description) {
  Check(!Observed.Published && Observed.PublishedCount == 0, Description);
  Check(Observed.FinalStackDepth == Observed.EntryStackDepth,
        "a refused exposure leaves the stack exactly as it found it");
  Check(Hooks::OwnedUserdataCount(Host) == 0,
        "a refused exposure leaves no owner behind");
  Check(Hooks::PublishedUserdataCount(Host) == 0,
        "a refused exposure publishes nothing");
  Check(Hooks::CachedIdentityCount(Host) == 0 &&
            Hooks::LiveCachedIdentityCount(Host) == 0,
        "a refused exposure records no identity-cache entry");
  const auto Installed = Hooks::ObserveClassUserdata(Host, Path);
  Check(!Installed.has_value(),
        "a refused exposure installs no value at the path it named");
}

// A refusal that happens while the value is still only being staged: no record
// exists yet, so no release step may run at all.
void CheckStagingRefusalsReleaseNothing() {
  ResetStorageCounters();
  Fixture Owner;
  Luna::State &Host = Owner.StateObject();
  const auto Before = Owner.Counters();
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Host);

  // Luna never owns the storage of an object it only borrows, and staging is
  // where that is decided - before any ownership record exists.
  Probe Borrowed;
  const Luna::LifetimeHandle Handle;
  const auto Refused =
      ExposeValue(Host, "Borrowed", &Borrowed, OwnershipModel::Borrowed, Handle,
                  nullptr, OwnedStorageProtocol());
  CheckNothingWasPublished(
      Host, "Borrowed", Refused,
      "Luna refuses to own the storage of an object it only borrows");
  Check(Refused.Failure == "internal_failure" &&
            Contains(Refused.Diagnostic, "Internal error while converting"),
        "an ownership statement Luna could never honor is Luna's own mistake");

  const auto After = Owner.Counters();
  Check(After.Invalidate == Before.Invalidate &&
            After.CacheRemoval == Before.CacheRemoval &&
            After.MetadataRelease == Before.MetadataRelease,
        "a refusal during staging runs no release step at all");
  Check(DestroyCalls == 0 && DeallocateCalls == 0,
        "a refusal during staging destroys and deallocates nothing");
  Check(Borrowed.Value == 5, "the borrowed object is left exactly as it was");

  // The same is true of the two lifetime refusals a borrowed exposure can
  // reach: both are decided before the identity cache is even consulted, and
  // both do name their exact reason, because an ended lifetime is the one
  // exposure refusal a consumer can cause.
  Luna::LifetimeHandle Invalidated;
  Invalidated.Invalidate();
  const auto Expired =
      ExposeValue(Host, "Expired", &Borrowed, OwnershipModel::Borrowed,
                  Invalidated, nullptr, ClassAllocator());
  CheckNothingWasPublished(Host, "Expired", Expired,
                           "a borrowed exposure through an invalidated "
                           "lifetime publishes nothing");
  Check(Expired.Failure == "expired_userdata" &&
            Contains(Expired.Diagnostic, "expired_lifetime_handle"),
        "an invalidated lifetime is refused as an expired value");

  const auto Undeclared = ExposeValue(
      Host, "Undeclared", &Borrowed, OwnershipModel::Borrowed,
      Luna::LifetimeHandle::Undeclared(), nullptr, ClassAllocator());
  CheckNothingWasPublished(Host, "Undeclared", Undeclared,
                           "a borrowed exposure without a declared lifetime "
                           "publishes nothing");
  Check(Undeclared.Failure == "expired_userdata" &&
            Contains(Undeclared.Diagnostic, "missing_lifetime_handle"),
        "a missing lifetime is refused as an expired value");

  Check(Owner.Counters().MetadataRelease == Before.MetadataRelease,
        "no lifetime refusal ever reaches a release step");
  Check(Hooks::ObserveRootStackDepth(Host) == EntryDepth,
        "every refused exposure restores the exact root stack depth");

  // The State keeps exposing and reading values afterwards.
  const auto Published =
      ExposeValue(Host, "Borrowed", &Borrowed, OwnershipModel::Borrowed, Handle,
                  nullptr, ClassAllocator());
  Check(Published.Published && Published.Failure == "none",
        "the State exposes a well-formed value after every refusal");
  Check(ReadValue(Host, "Borrowed", &Borrowed).DeliveredExpectedObject,
        "the value the State published after the refusals reaches native code");
  Check(Hooks::ObserveRootStackDepth(Host) == EntryDepth,
        "the recovered exposure and access restore the root stack depth");
}

// One ownership statement carries exactly the payload its model needs, and the
// write path chooses which payload that is from the model alone. A stray
// lifetime handle on an owning model is therefore neither honored nor a second
// owner: it is simply not part of that model's statement.
void CheckOwnershipPayloadIsChosenByModel() {
  ResetStorageCounters();
  {
    Fixture Owner;
    Luna::State &Host = Owner.StateObject();
    Probe *Storage = AllocateProbe();

    Luna::LifetimeHandle Stray;
    const auto Published =
        ExposeValue(Host, "Owned", Storage, OwnershipModel::LuaOwned, Stray,
                    nullptr, OwnedStorageProtocol());
    Check(Published.Published && Published.PublishedCount == 1 &&
              Published.Failure == "none",
          "a Lua-owned exposure publishes exactly one value");
    Check(Hooks::OwnedUserdataCount(Host) == 1 &&
              Hooks::PublishedUserdataCount(Host) == 1,
          "a stray lifetime handle creates no second owner");

    const auto Header = Hooks::ObserveClassUserdata(Host, "Owned");
    Check(Header && Header->Ownership == OwnershipModel::LuaOwned &&
              !Header->Handle.IsDeclared(),
          "an owning model records no borrowed lifetime at all");

    // Invalidating the stray handle changes nothing, because the value never
    // declared it.
    Stray.Invalidate();
    Check(ReadValue(Host, "Owned", Storage).DeliveredExpectedObject,
          "a Lua-owned value is unaffected by a handle it never declared");
    Check(
        Hooks::ReleaseClassValue(Host, Storage, ReleaseCause::LifecycleAction),
        "the Lua-owned value releases once");
    Check(DestroyCalls == 1 && DeallocateCalls == 1,
          "the Lua-owned value is destroyed once and deallocated once");
  }
  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "State destruction performs no second release");
}

// A refusal that happens after staging and construction succeeded: the release
// gate has to undo exactly what those steps established, and exactly once.
void CheckEstablishmentRefusalsReleaseExactlyWhatWasStaged() {
  // A Lua-owned exposure Luna could never destroy is refused during
  // establishment, and the storage it already took is still deallocated exactly
  // once.
  ResetStorageCounters();
  {
    Fixture Owner;
    Luna::State &Host = Owner.StateObject();
    const auto Before = Owner.Counters();
    Probe *Storage = AllocateProbe();

    const auto Refused =
        ExposeValue(Host, "Undestroyable", Storage, OwnershipModel::LuaOwned,
                    Luna::LifetimeHandle::Undeclared(), nullptr,
                    UndestroyableStorageProtocol());
    CheckNothingWasPublished(Host, "Undestroyable", Refused,
                             "a value Luna could not destroy exactly once "
                             "publishes nothing");
    Check(Refused.Failure == "internal_failure",
          "storage without its destruction step is Luna's own mistake");

    const auto After = Owner.Counters();
    Check(After.Invalidate == Before.Invalidate + 1,
          "a publication failure invalidates access before releasing anything");
    Check(After.CacheRemoval == Before.CacheRemoval + 1,
          "a publication failure removes the identity-cache entry once");
    Check(After.Deallocate == Before.Deallocate + 1 && DeallocateCalls == 1,
          "the storage Luna already took is deallocated exactly once");
    Check(DestroyCalls == 0,
          "no destruction step ran, because there was none to run");
    Check(After.MetadataRelease == Before.MetadataRelease + 1,
          "the refused value's metadata is released exactly once");
    Check(Host.Execute("return 1").IsSuccess(),
          "the State stays usable after a refused publication");

    // The same path publishes as soon as the ownership statement is complete,
    // because the refusal left no owner, no record, and no cache entry behind.
    Probe *Second = AllocateProbe();
    const auto Published = ExposeValue(
        Host, "Undestroyable", Second, OwnershipModel::LuaOwned,
        Luna::LifetimeHandle::Undeclared(), nullptr, OwnedStorageProtocol());
    Check(Published.Published && Published.Failure == "none",
          "the State publishes a complete value after a refused publication");
    Check(Hooks::OwnedUserdataCount(Host) == 1,
          "exactly the complete value is owned");
    Check(ReadValue(Host, "Undestroyable", Second).DeliveredExpectedObject,
          "the value published after the refusal reaches native code");
  }
  Check(DestroyCalls == 1 && DeallocateCalls == 2,
        "State destruction releases only the value it still owns");

  // A shared exposure without its one shared ownership reference is refused
  // during establishment; Luna never destroys or deallocates a shared object,
  // so the only steps that run are its own.
  ResetStorageCounters();
  {
    Fixture Owner;
    Luna::State &Host = Owner.StateObject();
    const auto Before = Owner.Counters();
    const std::shared_ptr<Probe> Object = std::make_shared<Probe>();

    const auto Refused = ExposeValue(
        Host, "Shared", Object.get(), OwnershipModel::Shared,
        Luna::LifetimeHandle::Undeclared(), nullptr, ClassAllocator());
    CheckNothingWasPublished(Host, "Shared", Refused,
                             "a shared value without its ownership reference "
                             "publishes nothing");

    const auto After = Owner.Counters();
    Check(After.SharedRelease == Before.SharedRelease,
          "no shared ownership reference is released, because none was taken");
    Check(After.Destroy == Before.Destroy &&
              After.Deallocate == Before.Deallocate,
          "a shared value is never destroyed or deallocated by Luna");
    Check(After.MetadataRelease == Before.MetadataRelease + 1,
          "the refused shared value's metadata is released exactly once");
    Check(Object.use_count() == 1,
          "the consumer is left holding exactly its own reference");

    // With exactly one shared ownership reference the same object publishes.
    const auto Published = ExposeValue(
        Host, "Shared", Object.get(), OwnershipModel::Shared,
        Luna::LifetimeHandle::Undeclared(), Object, ClassAllocator());
    Check(Published.Published && Published.Failure == "none",
          "the State publishes the shared value once its ownership is stated");
    Check(Object.use_count() == 2,
          "Luna retains exactly one shared ownership reference");
    Check(Hooks::ReleaseClassValue(Host, Object.get(),
                                   ReleaseCause::LifecycleAction),
          "the published shared value releases once");
    Check(Object.use_count() == 1,
          "releasing the shared value leaves the consumer's own reference");
  }
  Check(DestroyCalls == 0 && DeallocateCalls == 0,
        "no refused or published shared value was ever destroyed by Luna");
}

} // namespace

int RunUserdataPublicationFaultTests() {
  FailureCount = 0;
  CheckStagingRefusalsReleaseNothing();
  CheckOwnershipPayloadIsChosenByModel();
  CheckEstablishmentRefusalsReleaseExactlyWhatWasStaged();
  return FailureCount == 0 ? 0 : 1;
}
