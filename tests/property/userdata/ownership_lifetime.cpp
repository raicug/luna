// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/lifetime_handle.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/test_hooks.hpp"
#include "state/userdata/collection.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/identity.hpp"
#include "state/userdata/ownership.hpp"

#include <rapidcheck.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
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

class ByteCursor final {
public:
  explicit ByteCursor(const std::vector<std::uint8_t> &Bytes) noexcept
      : BytesValue(&Bytes) {}

  [[nodiscard]] std::uint8_t Next() noexcept {
    const std::size_t Index = IndexValue++;
    if (BytesValue->empty())
      return static_cast<std::uint8_t>(Index * 37U + 11U);
    return (*BytesValue)[Index % BytesValue->size()];
  }

  [[nodiscard]] std::size_t Pick(std::size_t Count) noexcept {
    return Count == 0 ? 0 : static_cast<std::size_t>(Next()) % Count;
  }

private:
  const std::vector<std::uint8_t> *BytesValue;
  std::size_t IndexValue = 0;
};

struct Probe final {
  int Value = 23;
};

std::size_t DestroyCalls = 0;
std::size_t DeallocateCalls = 0;

struct StoragePolicy final {
  bool DestructionThrows = false;
};

void ResetStorageCounters() {
  DestroyCalls = 0;
  DeallocateCalls = 0;
}

[[nodiscard]] Probe *AllocateLiveProbe() {
  Probe *Storage = static_cast<Probe *>(::operator new(sizeof(Probe)));
  new (Storage) Probe{};
  return Storage;
}

[[nodiscard]] ClassAllocator::DestroyOperation
ProbeDestruction(StoragePolicy *Policy) {
  return [Policy](void *Storage) {
    ++DestroyCalls;
    static_cast<Probe *>(Storage)->~Probe();
    if (Policy != nullptr && Policy->DestructionThrows)
      throw std::runtime_error("probe destruction reported a failure");
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

[[nodiscard]] ClassAllocator OwnedStorageProtocol(StoragePolicy *Policy) {
  return ClassAllocator::FromOperations(
      "Studio.OwnedProbeStorage", StorageRequest::ForClass<Probe>(),
      ClassAllocator::AllocateOperation(), ClassAllocator::ConstructOperation(),
      ProbeDestruction(Policy), ProbeDeallocation());
}

[[nodiscard]] ClassAllocator DestroyOnlyProtocol(StoragePolicy *Policy) {
  return ClassAllocator::FromOperations(
      "Studio.AdoptedProbeStorage", StorageRequest::ForClass<Probe>(),
      ClassAllocator::AllocateOperation(), ClassAllocator::ConstructOperation(),
      ProbeDestruction(Policy), ClassAllocator::DeallocateOperation());
}

std::uint64_t NextNonce = 0;

[[nodiscard]] NativeIdentity IdentityFor(const void *Address) {
  NativeIdentity Identity;
  Identity.Address = Address;
  Identity.Nonce = ++NextNonce;
  return Identity;
}

struct ModelCounters final {
  std::uint64_t Invalidate = 0;
  std::uint64_t CacheRemoval = 0;
  std::uint64_t Destroy = 0;
  std::uint64_t SharedRelease = 0;
  std::uint64_t Deallocate = 0;
  std::uint64_t MetadataRelease = 0;
};

struct ModelRecord final {
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  bool OwnsStorage = false;
  bool HasDestructionStep = false;
  bool WasConstructed = false;
  bool HoldsSharedReference = false;

  bool IsRecorded = false;
  LifetimeState Lifetime = LifetimeState::Allocated;

  LifetimeState HeaderLifetime = LifetimeState::Allocated;

  bool HandleIsLive = true;
};

[[nodiscard]] bool ModelRelease(ModelRecord &Record, ModelCounters &Counted,
                                bool UpdatesHeader) {
  if (!Record.IsRecorded || Record.Lifetime == LifetimeState::Released)
    return false;

  if (Record.Lifetime == LifetimeState::Constructed ||
      Record.Lifetime == LifetimeState::Published) {
    ++Counted.Invalidate;
    Record.Lifetime = LifetimeState::Invalid;
    if (UpdatesHeader)
      Record.HeaderLifetime = LifetimeState::Invalid;
  }

  ++Counted.CacheRemoval;

  if (Record.Ownership == OwnershipModel::LuaOwned && Record.WasConstructed) {
    ++Counted.Destroy;
    Record.WasConstructed = false;
  }

  if (Record.HoldsSharedReference) {
    ++Counted.SharedRelease;
    Record.HoldsSharedReference = false;
  }

  if (Record.OwnsStorage)
    ++Counted.Deallocate;

  ++Counted.MetadataRelease;
  Record.Lifetime = LifetimeState::Released;
  Record.IsRecorded = false;
  if (UpdatesHeader)
    Record.HeaderLifetime = LifetimeState::Released;
  return true;
}

[[nodiscard]] bool ModelInvalidate(ModelRecord &Record,
                                   ModelCounters &Counted) {
  if (!Record.IsRecorded)
    return false;
  if (Record.Lifetime != LifetimeState::Constructed &&
      Record.Lifetime != LifetimeState::Published)
    return false;

  ++Counted.Invalidate;
  Record.Lifetime = LifetimeState::Invalid;
  Record.HeaderLifetime = LifetimeState::Invalid;
  return true;
}

[[nodiscard]] bool ModelPermitsAccess(const ModelRecord &Record) noexcept {
  if (!Record.IsRecorded || Record.Lifetime != LifetimeState::Published)
    return false;
  if (Record.HeaderLifetime != LifetimeState::Published)
    return false;
  return Record.Ownership != OwnershipModel::Borrowed || Record.HandleIsLive;
}

enum class OwnershipFlaw {
  None,
  MissingHandle,
  ExpiredHandle,
  UnexpectedHandle,
  MissingShared,
  UnexpectedShared,
  BorrowedStorage
};

enum class EntryMode { Exposed, Stepped, SkippedEstablishment, StagedOnly };

enum class Ending {
  Nothing,
  Invalidate,
  InvalidateHandle,
  Release,
  ReleaseTwice,
  ReleaseByStorage,
  Collected
};

struct GeneratedValue final {
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  OwnershipFlaw Flaw = OwnershipFlaw::None;
  EntryMode Entry = EntryMode::Exposed;
  Ending End = Ending::Nothing;
  ReleaseCause Cause = ReleaseCause::GarbageCollection;

  bool OwnsStorage = false;
  bool DestructionThrows = false;
};

[[nodiscard]] OwnershipModel GeneratedOwnership(std::size_t Choice) noexcept {
  switch (Choice % 3) {
  case 0:
    return OwnershipModel::Borrowed;
  case 1:
    return OwnershipModel::LuaOwned;
  default:
    break;
  }
  return OwnershipModel::Shared;
}

[[nodiscard]] ReleaseCause GeneratedCause(std::size_t Choice) noexcept {
  switch (Choice % 4) {
  case 0:
    return ReleaseCause::GarbageCollection;
  case 1:
    return ReleaseCause::ExplicitInvalidation;
  case 2:
    return ReleaseCause::LifecycleAction;
  default:
    break;
  }
  return ReleaseCause::StateDestruction;
}

[[nodiscard]] OwnershipFlaw GeneratedFlaw(OwnershipModel Ownership,
                                          std::size_t Choice) noexcept {
  switch (Ownership) {
  case OwnershipModel::Borrowed:
    switch (Choice % 8) {
    case 4:
      return OwnershipFlaw::MissingHandle;
    case 5:
      return OwnershipFlaw::ExpiredHandle;
    case 6:
      return OwnershipFlaw::UnexpectedShared;
    case 7:
      return OwnershipFlaw::BorrowedStorage;
    default:
      break;
    }
    return OwnershipFlaw::None;
  case OwnershipModel::LuaOwned:
    switch (Choice % 8) {
    case 6:
      return OwnershipFlaw::UnexpectedHandle;
    case 7:
      return OwnershipFlaw::UnexpectedShared;
    default:
      break;
    }
    return OwnershipFlaw::None;
  case OwnershipModel::Shared:
    switch (Choice % 8) {
    case 6:
      return OwnershipFlaw::MissingShared;
    case 7:
      return OwnershipFlaw::UnexpectedHandle;
    default:
      break;
    }
    return OwnershipFlaw::None;
  }
  return OwnershipFlaw::None;
}

[[nodiscard]] GeneratedValue GenerateValue(ByteCursor &Cursor) {
  GeneratedValue Value;
  Value.Ownership = GeneratedOwnership(Cursor.Pick(3));
  Value.Flaw = GeneratedFlaw(Value.Ownership, Cursor.Pick(8));

  switch (Cursor.Pick(6)) {
  case 0:
    Value.Entry = EntryMode::Stepped;
    break;
  case 1:
    Value.Entry = EntryMode::SkippedEstablishment;
    break;
  case 2:
    Value.Entry = EntryMode::StagedOnly;
    break;
  default:
    Value.Entry = EntryMode::Exposed;
    break;
  }

  switch (Cursor.Pick(8)) {
  case 0:
    Value.End = Ending::Nothing;
    break;
  case 1:
    Value.End = Ending::Invalidate;
    break;
  case 2:
    Value.End = Ending::InvalidateHandle;
    break;
  case 3:
  case 4:
    Value.End = Ending::Release;
    break;
  case 5:
    Value.End = Ending::ReleaseTwice;
    break;
  case 6:
    Value.End = Ending::ReleaseByStorage;
    break;
  default:
    Value.End = Ending::Collected;
    break;
  }

  if (Value.End == Ending::InvalidateHandle &&
      Value.Ownership != OwnershipModel::Borrowed)
    Value.End = Ending::Invalidate;

  Value.Cause = GeneratedCause(Cursor.Pick(4));

  Value.OwnsStorage = Value.Ownership == OwnershipModel::LuaOwned
                          ? Cursor.Pick(4) != 0
                          : Value.Flaw == OwnershipFlaw::BorrowedStorage;
  Value.DestructionThrows =
      Value.Ownership == OwnershipModel::LuaOwned && Cursor.Pick(5) == 0;
  return Value;
}

[[nodiscard]] OwnershipFailure
ModelEstablishmentFailure(const GeneratedValue &Value) noexcept {
  const bool HasHandle = Value.Flaw == OwnershipFlaw::ExpiredHandle ||
                         Value.Flaw == OwnershipFlaw::UnexpectedHandle ||
                         (Value.Ownership == OwnershipModel::Borrowed &&
                          Value.Flaw != OwnershipFlaw::MissingHandle);
  const bool HasShared = Value.Flaw == OwnershipFlaw::UnexpectedShared ||
                         (Value.Ownership == OwnershipModel::Shared &&
                          Value.Flaw != OwnershipFlaw::MissingShared);

  switch (Value.Ownership) {
  case OwnershipModel::Borrowed:
    if (!HasHandle)
      return OwnershipFailure::MissingLifetimeHandle;
    if (Value.Flaw == OwnershipFlaw::ExpiredHandle)
      return OwnershipFailure::ExpiredLifetimeHandle;
    if (HasShared)
      return OwnershipFailure::UnexpectedSharedOwnership;
    break;
  case OwnershipModel::LuaOwned:
    if (HasHandle)
      return OwnershipFailure::UnexpectedLifetimeHandle;
    if (HasShared)
      return OwnershipFailure::UnexpectedSharedOwnership;
    break;
  case OwnershipModel::Shared:
    if (HasHandle)
      return OwnershipFailure::UnexpectedLifetimeHandle;
    if (!HasShared)
      return OwnershipFailure::MissingSharedOwnership;
    break;
  }
  return OwnershipFailure::None;
}

class Fixture final {
public:
  Fixture() {
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Probe> Class = Registry.RegisterClass<Probe>(
        "Probe", Luna::StableTypeKey("Studio.OwnedProbe"));
    Registered = Class.Commit().IsSuccess();
    Gate = Hooks::UserdataOwnershipOf(Owner);
  }

  [[nodiscard]] bool IsUsable() const noexcept {
    return Registered && Gate != nullptr && Owner.IsReady();
  }

  [[nodiscard]] Luna::State &StateObject() noexcept { return Owner; }
  [[nodiscard]] OwnershipRegistry &Gateway() noexcept { return *Gate; }

  [[nodiscard]] std::optional<UserdataHeader>
  HeaderFor(OwnershipModel Ownership) {
    return Hooks::DescribeClassUserdata(Owner, "Probe", Ownership,
                                        ConstAccess::Mutable);
  }

private:
  Luna::State Owner;
  bool Registered = false;
  OwnershipRegistry *Gate = nullptr;
};

[[nodiscard]] std::string_view OwnershipTag(OwnershipModel Ownership) noexcept {
  switch (Ownership) {
  case OwnershipModel::Borrowed:
    return "borrowed";
  case OwnershipModel::LuaOwned:
    return "lua-owned";
  case OwnershipModel::Shared:
    break;
  }
  return "shared";
}

} // namespace

namespace {

void VerifyGateDrivenSequence(ByteCursor &Cursor) {
  ResetStorageCounters();
  Hooks::ResetUserdataCollections();

  const std::size_t Count = 1 + Cursor.Pick(4);
  std::vector<GeneratedValue> Generated;
  Generated.reserve(Count);
  for (std::size_t Index = 0; Index < Count; ++Index)
    Generated.push_back(GenerateValue(Cursor));

  std::vector<std::unique_ptr<Probe>> BorrowedObjects(Count);
  std::vector<std::shared_ptr<Probe>> SharedObjects(Count);
  std::vector<Probe *> UnownedStorage(Count, nullptr);
  std::vector<Luna::LifetimeHandle> Handles(Count);
  std::vector<std::shared_ptr<Probe>> Spares(Count);

  std::vector<std::unique_ptr<StoragePolicy>> Policies(Count);
  for (std::size_t Index = 0; Index < Count; ++Index) {
    Policies[Index] = std::make_unique<StoragePolicy>();
    Policies[Index]->DestructionThrows = Generated[Index].DestructionThrows;
  }

  std::vector<ModelRecord> Model(Count);
  ModelCounters Expected;
  std::uint64_t ExpectedContained = 0;
  std::size_t RemainingBeforeDestruction = 0;
  std::size_t PublishedEver = 0;

  {
    Fixture Owner;
    RC_ASSERT(Owner.IsUsable());
    OwnershipRegistry &Gate = Owner.Gateway();

    for (std::size_t Index = 0; Index < Count; ++Index) {
      const GeneratedValue &Value = Generated[Index];
      ModelRecord &Record = Model[Index];
      Record.Ownership = Value.Ownership;

      void *Storage = nullptr;
      switch (Value.Ownership) {
      case OwnershipModel::Borrowed:
        BorrowedObjects[Index] = std::make_unique<Probe>();
        Storage = BorrowedObjects[Index].get();
        break;
      case OwnershipModel::LuaOwned: {
        Probe *Allocated = AllocateLiveProbe();
        Storage = Allocated;
        if (!Value.OwnsStorage)
          UnownedStorage[Index] = Allocated;
        break;
      }
      case OwnershipModel::Shared:
        SharedObjects[Index] = std::make_shared<Probe>();
        Storage = SharedObjects[Index].get();
        break;
      }

      ClassAllocator Allocator;
      if (Value.Ownership == OwnershipModel::LuaOwned)
        Allocator = Value.OwnsStorage
                        ? OwnedStorageProtocol(Policies[Index].get())
                        : DestroyOnlyProtocol(Policies[Index].get());
      else if (Value.Flaw == OwnershipFlaw::BorrowedStorage)
        Allocator = OwnedStorageProtocol(Policies[Index].get());
      Record.OwnsStorage = Allocator.OwnsStorage();
      Record.HasDestructionStep = Allocator.DeclaresDestruction();

      const auto Described = Owner.HeaderFor(Value.Ownership);
      RC_ASSERT(Described.has_value());
      UserdataHeader Header = *Described;
      RC_ASSERT(Header.HasCanonicalLayout());
      RC_ASSERT(Header.IdentifiesClass());
      RC_ASSERT(Header.Lifetime == LifetimeState::Allocated);

      StagedStorage Staged;
      Staged.Storage = Storage;
      Staged.Identity = IdentityFor(Storage);
      Staged.Allocator = Allocator;

      if (Value.Flaw == OwnershipFlaw::ExpiredHandle)
        Handles[Index].Invalidate();

      const bool StageRefused = Value.Ownership == OwnershipModel::Borrowed &&
                                Allocator.OwnsStorage();
      const OwnershipFailure Establishment = ModelEstablishmentFailure(Value);
      const bool Throws = Value.DestructionThrows;

      const auto ApplyRelease = [&](bool UpdatesHeader) {
        const std::uint64_t Before = Expected.Destroy;
        const bool Released = ModelRelease(Record, Expected, UpdatesHeader);
        if (Throws && Expected.Destroy != Before)
          ++ExpectedContained;
        return Released;
      };

      {
        OwnershipRequest Request;
        const bool WantsHandle =
            Value.Flaw == OwnershipFlaw::UnexpectedHandle ||
            (Value.Ownership == OwnershipModel::Borrowed &&
             Value.Flaw != OwnershipFlaw::MissingHandle);
        if (WantsHandle)
          Request.Handle = Handles[Index];

        const bool WantsShared =
            Value.Flaw == OwnershipFlaw::UnexpectedShared ||
            (Value.Ownership == OwnershipModel::Shared &&
             Value.Flaw != OwnershipFlaw::MissingShared);
        if (WantsShared) {
          if (Value.Ownership == OwnershipModel::Shared) {
            Request.SharedOwnership = SharedObjects[Index];
          } else {
            Spares[Index] = std::make_shared<Probe>();
            Request.SharedOwnership = Spares[Index];
          }
        }

        switch (Value.Entry) {
        case EntryMode::Exposed: {
          const auto Outcome = Gate.Expose(Header, Staged, Request);
          if (StageRefused) {
            RC_ASSERT(!Outcome.Succeeded);
            RC_ASSERT(Outcome.Failure ==
                      OwnershipFailure::BorrowedStorageOwnership);
          } else if (Establishment != OwnershipFailure::None) {
            RC_ASSERT(!Outcome.Succeeded);
            RC_ASSERT(Outcome.Failure == Establishment);

            Record.IsRecorded = true;
            Record.WasConstructed = true;
            Record.Lifetime = LifetimeState::Constructed;
            Record.HeaderLifetime = LifetimeState::Constructed;
            RC_ASSERT(ApplyRelease(true));
          } else {
            RC_ASSERT(Outcome.Succeeded);
            Record.IsRecorded = true;
            Record.WasConstructed = true;
            Record.Lifetime = LifetimeState::Published;
            Record.HeaderLifetime = LifetimeState::Published;
            Record.HoldsSharedReference =
                Value.Ownership == OwnershipModel::Shared;
            ++PublishedEver;
          }
          break;
        }

        case EntryMode::Stepped:
        case EntryMode::SkippedEstablishment:
        case EntryMode::StagedOnly: {
          const auto Staging = Gate.Stage(Header, Staged);
          if (StageRefused) {
            RC_ASSERT(!Staging.Succeeded);
            RC_ASSERT(Staging.Failure ==
                      OwnershipFailure::BorrowedStorageOwnership);
            break;
          }

          RC_ASSERT(Staging.Succeeded);
          Record.IsRecorded = true;
          Record.Lifetime = LifetimeState::Allocated;
          Record.HeaderLifetime = LifetimeState::Allocated;
          RC_ASSERT(Header.Allocator.IsDeclared());

          if (Value.Entry == EntryMode::StagedOnly) {
            RC_ASSERT(Gate.Release(Header, ReleaseCause::ConstructionFailure));
            const std::uint64_t Invalidations = Expected.Invalidate;
            RC_ASSERT(ApplyRelease(true));
            RC_ASSERT(Expected.Invalidate == Invalidations);
            break;
          }

          RC_ASSERT(Gate.Construct(Header).Succeeded);
          Record.WasConstructed = true;
          Record.Lifetime = LifetimeState::Constructed;
          Record.HeaderLifetime = LifetimeState::Constructed;

          if (Value.Entry == EntryMode::SkippedEstablishment) {
            const auto Refused = Gate.Publish(Header);
            RC_ASSERT(!Refused.Succeeded);
            RC_ASSERT(Refused.Failure == OwnershipFailure::MissingOwnership);
            RC_ASSERT(Gate.Release(Header, ReleaseCause::PublicationFailure));
            RC_ASSERT(ApplyRelease(true));
            break;
          }

          const auto Established = Gate.Establish(Header, Request);
          if (Establishment != OwnershipFailure::None) {
            RC_ASSERT(!Established.Succeeded);
            RC_ASSERT(Established.Failure == Establishment);
            RC_ASSERT(Gate.Release(Header, ReleaseCause::PublicationFailure));
            RC_ASSERT(ApplyRelease(true));
            break;
          }

          RC_ASSERT(Established.Succeeded);
          Record.HoldsSharedReference =
              Value.Ownership == OwnershipModel::Shared;

          RC_ASSERT(Gate.Establish(Header, Request).Failure ==
                    OwnershipFailure::OwnershipAlreadyEstablished);

          RC_ASSERT(Gate.Publish(Header).Succeeded);
          Record.Lifetime = LifetimeState::Published;
          Record.HeaderLifetime = LifetimeState::Published;
          ++PublishedEver;
          break;
        }
        }
      }

      if (Value.Ownership == OwnershipModel::Shared) {
        RC_ASSERT(SharedObjects[Index].use_count() ==
                  (Record.HoldsSharedReference ? 2 : 1));
      }

      RC_ASSERT(Header.Lifetime == Record.HeaderLifetime);
      RC_ASSERT(Gate.LifetimePermitsAccess(Header) ==
                ModelPermitsAccess(Record));

      if (Record.Lifetime != LifetimeState::Published) {
        const ModelCounters Before = Expected;
        RC_ASSERT(!Gate.Release(Header, Value.Cause));
        RC_ASSERT(Expected.MetadataRelease == Before.MetadataRelease);
        continue;
      }

      switch (Value.End) {
      case Ending::Nothing:
        break;

      case Ending::Invalidate:
        RC_ASSERT(Gate.Invalidate(Header, Value.Cause));
        RC_ASSERT(ModelInvalidate(Record, Expected));
        RC_ASSERT(!Gate.Invalidate(Header, Value.Cause));
        break;

      case Ending::InvalidateHandle:
        Handles[Index].Invalidate();
        Record.HandleIsLive = false;
        break;

      case Ending::Release:
        RC_ASSERT(Gate.Release(Header, Value.Cause));
        RC_ASSERT(ApplyRelease(true));
        break;

      case Ending::ReleaseTwice: {
        RC_ASSERT(Gate.Release(Header, Value.Cause));
        RC_ASSERT(ApplyRelease(true));
        const ModelCounters Before = Expected;
        RC_ASSERT(!Gate.Release(Header, Value.Cause));
        RC_ASSERT(Expected.Destroy == Before.Destroy);
        RC_ASSERT(Expected.Deallocate == Before.Deallocate);
        RC_ASSERT(Expected.SharedRelease == Before.SharedRelease);
        break;
      }

      case Ending::ReleaseByStorage:
        RC_ASSERT(Gate.ReleaseByStorage(Storage, Value.Cause));
        RC_ASSERT(ApplyRelease(false));
        RC_ASSERT(!Gate.ReleaseByStorage(Storage, Value.Cause));
        break;

      case Ending::Collected:
        RC_ASSERT(Gate.ReleaseCollected(Header));
        RC_ASSERT(ApplyRelease(true));
        RC_ASSERT(!Gate.ReleaseCollected(Header));
        break;
      }

      RC_ASSERT(Header.Lifetime == Record.HeaderLifetime);
      RC_ASSERT(Gate.LifetimePermitsAccess(Header) ==
                ModelPermitsAccess(Record));
      if (Value.Ownership == OwnershipModel::Shared) {
        RC_ASSERT(SharedObjects[Index].use_count() ==
                  (Record.HoldsSharedReference ? 2 : 1));
      }
    }

    std::size_t ExpectedRecords = 0;
    std::size_t ExpectedPublished = 0;
    for (const ModelRecord &Record : Model) {
      if (!Record.IsRecorded)
        continue;
      ++ExpectedRecords;
      if (Record.Lifetime == LifetimeState::Published)
        ++ExpectedPublished;
    }
    RemainingBeforeDestruction = ExpectedRecords;

    if (PublishedEver == 0)
      RC_TAG("gate: every exposure refused");
    else if (ExpectedRecords == 0)
      RC_TAG("gate: everything released before destruction");
    else
      RC_TAG("gate: values left for State destruction");

    RC_ASSERT(Gate.RecordCount() == ExpectedRecords);
    RC_ASSERT(Gate.PublishedCount() == ExpectedPublished);
    RC_ASSERT(Hooks::OwnedUserdataCount(Owner.StateObject()) ==
              ExpectedRecords);
    RC_ASSERT(Hooks::PublishedUserdataCount(Owner.StateObject()) ==
              ExpectedPublished);

    const auto Counted = Hooks::UserdataReleaseCounters(Owner.StateObject());
    RC_ASSERT(Counted.Invalidate == Expected.Invalidate);
    RC_ASSERT(Counted.CacheRemoval == Expected.CacheRemoval);
    RC_ASSERT(Counted.Destroy == Expected.Destroy);
    RC_ASSERT(Counted.SharedRelease == Expected.SharedRelease);
    RC_ASSERT(Counted.Deallocate == Expected.Deallocate);
    RC_ASSERT(Counted.MetadataRelease == Expected.MetadataRelease);

    RC_ASSERT(Counted.ContainedException == ExpectedContained);
    RC_ASSERT(Counted.IncompleteMetadata == 0);
    RC_ASSERT(Gate.RetainsCleanupMetadata());

    RC_ASSERT(DestroyCalls == Expected.Destroy);
    RC_ASSERT(DeallocateCalls == Expected.Deallocate);
  }

  for (ModelRecord &Record : Model)
    static_cast<void>(ModelRelease(Record, Expected, false));
  RC_ASSERT(DestroyCalls == Expected.Destroy);
  RC_ASSERT(DeallocateCalls == Expected.Deallocate);

  const auto Destroyed = Hooks::ObserveLastStateDestruction();
  RC_ASSERT(Destroyed.Observed);
  RC_ASSERT(Destroyed.RefusedNewInvocations);
  RC_ASSERT(Destroyed.RetainedCleanupMetadata);
  RC_ASSERT(Destroyed.IncompleteMetadata == 0);

  RC_ASSERT(Destroyed.ReleasedDuringClose == 0);
  RC_ASSERT(Destroyed.ReleasedAfterClose == RemainingBeforeDestruction);
  RC_ASSERT(Hooks::ObserveUserdataCollections().ContainedException == 0);

  for (std::size_t Index = 0; Index < Count; ++Index) {
    if (SharedObjects[Index])
      RC_ASSERT(SharedObjects[Index].use_count() == 1);
    if (BorrowedObjects[Index])
      RC_ASSERT(BorrowedObjects[Index]->Value == 23);
    if (UnownedStorage[Index] != nullptr)
      ::operator delete(UnownedStorage[Index]);
  }
}

} // namespace

namespace {

enum class WriteVariant { Valid, ExpiredHandle, MissingHandle, MissingShared };

enum class WriteEnding { Nothing, InvalidateHandle, Release };

enum class ReuseVariant {
  None,
  Identical,
  ConflictingOwnership,
  IncompatibleAccess
};

struct GeneratedWrite final {
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  WriteVariant Variant = WriteVariant::Valid;
  WriteEnding End = WriteEnding::Nothing;
  ReleaseCause Cause = ReleaseCause::GarbageCollection;
  bool DestructionThrows = false;
};

[[nodiscard]] GeneratedWrite GenerateWrite(ByteCursor &Cursor) {
  GeneratedWrite Value;
  Value.Ownership = GeneratedOwnership(Cursor.Pick(3));

  switch (Cursor.Pick(6)) {
  case 0:
    if (Value.Ownership == OwnershipModel::Borrowed)
      Value.Variant = WriteVariant::ExpiredHandle;
    else if (Value.Ownership == OwnershipModel::Shared)
      Value.Variant = WriteVariant::MissingShared;
    break;
  case 1:
    if (Value.Ownership == OwnershipModel::Borrowed)
      Value.Variant = WriteVariant::MissingHandle;
    break;
  default:
    break;
  }

  switch (Cursor.Pick(4)) {
  case 0:
    Value.End = Value.Ownership == OwnershipModel::Borrowed
                    ? WriteEnding::InvalidateHandle
                    : WriteEnding::Nothing;
    break;
  case 1:
    Value.End = WriteEnding::Nothing;
    break;
  default:
    Value.End = WriteEnding::Release;
    break;
  }

  Value.Cause = GeneratedCause(Cursor.Pick(4));
  Value.DestructionThrows =
      Value.Ownership == OwnershipModel::LuaOwned && Cursor.Pick(5) == 0;
  return Value;
}

[[nodiscard]] std::string_view
ExpectedWriteFailure(const GeneratedWrite &Value) noexcept {
  switch (Value.Variant) {
  case WriteVariant::ExpiredHandle:
  case WriteVariant::MissingHandle:
    return "expired_userdata";
  case WriteVariant::MissingShared:
    return "internal_failure";
  case WriteVariant::Valid:
    break;
  }
  return "none";
}

[[nodiscard]] Luna::Detail::ClassValueWriteObservation
ExposeThroughWritePath(Luna::State &Host, const std::string &Path,
                       void *Storage, OwnershipModel Ownership,
                       ConstAccess Access, const Luna::LifetimeHandle &Handle,
                       std::shared_ptr<void> Shared,
                       const ClassAllocator &Allocator) {
  Luna::Detail::ClassValueExposureRequest Request;
  Request.QualifiedName = "Probe";
  Request.Path = Path;
  Request.Storage = Storage;
  Request.Ownership = Ownership;
  Request.Access = Access;
  Request.Handle = Handle;
  Request.SharedOwnership = std::move(Shared);
  Request.Allocator = Allocator;
  return Hooks::ExposeClassValue(Host, Request);
}

[[nodiscard]] Luna::Detail::ClassAccessObservation
ReadThroughAccessPath(Luna::State &Host, const std::string &Path,
                      const void *Expected) {
  Luna::Detail::ClassAccessRequest Request;
  Request.QualifiedName = "Probe";
  Request.Path = Path;
  Request.ExpectedStorage = Expected;
  return Hooks::AccessClassUserdata(Host, Request);
}

void VerifyWritePathSequence(ByteCursor &Cursor) {
  ResetStorageCounters();
  Hooks::ResetUserdataCollections();

  const std::size_t Count = 1 + Cursor.Pick(3);
  std::vector<GeneratedWrite> Generated;
  Generated.reserve(Count);
  for (std::size_t Index = 0; Index < Count; ++Index)
    Generated.push_back(GenerateWrite(Cursor));

  ReuseVariant Reuse = ReuseVariant::None;
  switch (Cursor.Pick(4)) {
  case 0:
    Reuse = ReuseVariant::Identical;
    break;
  case 1:
    Reuse = ReuseVariant::ConflictingOwnership;
    break;
  case 2:
    Reuse = ReuseVariant::IncompatibleAccess;
    break;
  default:
    break;
  }
  const bool MovesState = Cursor.Pick(3) == 0;
  const bool CollectsEverything = Cursor.Pick(2) == 0;

  std::vector<std::unique_ptr<Probe>> BorrowedObjects(Count);
  std::vector<std::shared_ptr<Probe>> SharedObjects(Count);
  std::vector<Luna::LifetimeHandle> Handles(Count);
  std::vector<void *> Storage(Count, nullptr);
  std::vector<std::string> Paths;
  Paths.reserve(Count);

  std::vector<std::unique_ptr<StoragePolicy>> Policies(Count);
  for (std::size_t Index = 0; Index < Count; ++Index) {
    Policies[Index] = std::make_unique<StoragePolicy>();
    Policies[Index]->DestructionThrows = Generated[Index].DestructionThrows;
  }
  StoragePolicy Quiet;

  std::vector<ModelRecord> Model(Count);
  std::vector<LifetimeState> ExpectedHeaderState(Count,
                                                 LifetimeState::Published);
  ModelCounters Expected;
  std::uint64_t ExpectedContained = 0;
  std::size_t ExpectedArrivals = 0;
  std::size_t Arrivals = 0;
  std::size_t WrittenValues = 0;

  {
    Luna::State Primary;
    RC_ASSERT(Primary.IsReady());
    {
      Luna::BindingRegistry Registry = Primary.Bindings();
      Luna::ClassBuilder<Probe> Class = Registry.RegisterClass<Probe>(
          "Probe", Luna::StableTypeKey("Studio.OwnedProbe"));
      RC_ASSERT(Class.Commit().IsSuccess());
    }
    RC_ASSERT(Hooks::UserdataCollectorIsInstalled(Primary));
    RC_ASSERT(!Hooks::ClassMetatableIsCreated(Primary, "Probe"));

    std::optional<Luna::State> Moved;
    Luna::State *Active = &Primary;

    for (std::size_t Index = 0; Index < Count; ++Index) {
      const GeneratedWrite &Value = Generated[Index];
      ModelRecord &Record = Model[Index];
      Record.Ownership = Value.Ownership;
      Paths.push_back("Slot" + std::to_string(Index));

      std::shared_ptr<void> Shared;
      ClassAllocator Allocator;
      switch (Value.Ownership) {
      case OwnershipModel::Borrowed:
        BorrowedObjects[Index] = std::make_unique<Probe>();
        Storage[Index] = BorrowedObjects[Index].get();
        break;
      case OwnershipModel::LuaOwned:
        Storage[Index] = AllocateLiveProbe();
        Allocator = OwnedStorageProtocol(Policies[Index].get());
        break;
      case OwnershipModel::Shared:
        SharedObjects[Index] = std::make_shared<Probe>();
        Storage[Index] = SharedObjects[Index].get();
        if (Value.Variant != WriteVariant::MissingShared)
          Shared = SharedObjects[Index];
        break;
      }
      Record.OwnsStorage = Allocator.OwnsStorage();
      Record.HasDestructionStep = Allocator.DeclaresDestruction();

      if (Value.Variant == WriteVariant::ExpiredHandle)
        Handles[Index].Invalidate();

      Luna::LifetimeHandle Declared = Luna::LifetimeHandle::Undeclared();
      if (Value.Ownership == OwnershipModel::Borrowed &&
          Value.Variant != WriteVariant::MissingHandle)
        Declared = Handles[Index];

      const auto Written = ExposeThroughWritePath(
          *Active, Paths[Index], Storage[Index], Value.Ownership,
          ConstAccess::Mutable, Declared, Shared, Allocator);

      const std::string_view Failure = ExpectedWriteFailure(Value);
      RC_ASSERT(Written.Failure == Failure);
      RC_ASSERT(Written.Published == (Failure == "none"));
      RC_ASSERT(Written.PublishedCount == (Written.Published ? 1 : 0));

      RC_ASSERT(Written.FinalStackDepth == Written.EntryStackDepth);

      if (Written.Published) {
        Record.IsRecorded = true;
        Record.WasConstructed = true;
        Record.Lifetime = LifetimeState::Published;
        Record.HeaderLifetime = LifetimeState::Published;
        Record.HoldsSharedReference = Value.Ownership == OwnershipModel::Shared;
      } else if (Value.Variant == WriteVariant::MissingShared) {
        Record.IsRecorded = true;
        Record.WasConstructed = true;
        Record.Lifetime = LifetimeState::Constructed;
        Record.HeaderLifetime = LifetimeState::Constructed;
        RC_ASSERT(ModelRelease(Record, Expected, true));
      }

      if (Written.Published)
        ++WrittenValues;
    }

    const auto CountRecorded = [&Model]() {
      std::size_t Recorded = 0;
      for (const ModelRecord &Record : Model) {
        if (Record.IsRecorded)
          ++Recorded;
      }
      return Recorded;
    };
    const auto CountPublished = [&Model]() {
      std::size_t Published = 0;
      for (const ModelRecord &Record : Model) {
        if (Record.IsRecorded && Record.Lifetime == LifetimeState::Published)
          ++Published;
      }
      return Published;
    };

    const auto VerifyCounts = [&](Luna::State &Host) {
      RC_ASSERT(Hooks::OwnedUserdataCount(Host) == CountRecorded());
      RC_ASSERT(Hooks::PublishedUserdataCount(Host) == CountPublished());
      RC_ASSERT(Hooks::LiveCachedIdentityCount(Host) == CountPublished());
      RC_ASSERT(Hooks::CachedIdentityCount(Host) == CountPublished());

      const auto Counted = Hooks::UserdataReleaseCounters(Host);
      RC_ASSERT(Counted.Invalidate == Expected.Invalidate);
      RC_ASSERT(Counted.CacheRemoval == Expected.CacheRemoval);
      RC_ASSERT(Counted.Destroy == Expected.Destroy);
      RC_ASSERT(Counted.SharedRelease == Expected.SharedRelease);
      RC_ASSERT(Counted.Deallocate == Expected.Deallocate);
      RC_ASSERT(Counted.MetadataRelease == Expected.MetadataRelease);
      RC_ASSERT(Counted.ContainedException == ExpectedContained);
      RC_ASSERT(Counted.IncompleteMetadata == 0);
      RC_ASSERT(DestroyCalls == Expected.Destroy);
      RC_ASSERT(DeallocateCalls == Expected.Deallocate);
    };

    VerifyCounts(*Active);

    bool AnyPublished = false;
    for (const ModelRecord &Record : Model) {
      AnyPublished =
          AnyPublished || Record.Lifetime == LifetimeState::Published;
    }
    RC_ASSERT(Hooks::ClassMetatableIsCreated(*Active, "Probe") == AnyPublished);
    RC_ASSERT(Hooks::ClassMetatableCreationCount(*Active, "Probe") ==
              (AnyPublished ? 1U : 0U));

    for (std::size_t Index = 0; Index < Count; ++Index) {
      const ModelRecord &Record = Model[Index];
      const auto Read =
          ReadThroughAccessPath(*Active, Paths[Index], Storage[Index]);
      const bool Permitted = ModelPermitsAccess(Record);
      if (Permitted)
        ++ExpectedArrivals;
      if (Read.ReachedNativeCode)
        ++Arrivals;
      RC_ASSERT(Read.ReachedNativeCode == Permitted);
      RC_ASSERT(Read.DeliveredExpectedObject == Permitted);
      if (Permitted) {
        RC_ASSERT(Read.PermitsMutation);
        const auto Header = Hooks::ObserveClassUserdata(*Active, Paths[Index]);
        RC_ASSERT(Header.has_value());
        RC_ASSERT(Header->Lifetime == ExpectedHeaderState[Index]);
        RC_ASSERT(Header->Ownership == Record.Ownership);
        RC_ASSERT(Header->HasRequiredLifetimeHandle());
        RC_ASSERT(Header->Identity.IsValid());
      }
    }

    std::optional<std::size_t> First;
    for (std::size_t Index = 0; Index < Count && !First; ++Index) {
      if (Model[Index].Lifetime == LifetimeState::Published)
        First = Index;
    }

    if (First && Reuse != ReuseVariant::None) {
      const std::size_t Index = *First;
      const GeneratedWrite &Value = Generated[Index];
      const ModelCounters Before = Expected;

      OwnershipModel Requested = Value.Ownership;
      ConstAccess Access = ConstAccess::Mutable;
      Luna::LifetimeHandle Declared = Luna::LifetimeHandle::Undeclared();
      std::shared_ptr<void> Shared;
      ClassAllocator Allocator;

      switch (Reuse) {
      case ReuseVariant::Identical:
        break;
      case ReuseVariant::ConflictingOwnership:
        Requested = Value.Ownership == OwnershipModel::Borrowed
                        ? OwnershipModel::LuaOwned
                        : OwnershipModel::Borrowed;
        break;
      case ReuseVariant::IncompatibleAccess:
        Access = ConstAccess::Const;
        break;
      case ReuseVariant::None:
        break;
      }

      if (Requested == OwnershipModel::Borrowed)
        Declared = Handles[Index];
      if (Requested == OwnershipModel::LuaOwned)
        Allocator = DestroyOnlyProtocol(&Quiet);
      if (Requested == OwnershipModel::Shared)
        Shared = SharedObjects[Index];

      const auto Again = ExposeThroughWritePath(
          *Active, Paths[Index], Storage[Index], Requested, Access, Declared,
          Shared, Allocator);

      if (Reuse == ReuseVariant::Identical) {
        RC_ASSERT(Again.Failure == "none");
        RC_ASSERT(Again.Published);
        RC_ASSERT(Again.PublishedCount == 1);
      } else {
        RC_ASSERT(Again.Failure == "conflicting_ownership");
        RC_ASSERT(!Again.Published);
        RC_ASSERT(Again.PublishedCount == 0);
        RC_ASSERT(Again.FinalStackDepth == Again.EntryStackDepth);
      }

      RC_ASSERT(Expected.MetadataRelease == Before.MetadataRelease);
      VerifyCounts(*Active);
      RC_ASSERT(ReadThroughAccessPath(*Active, Paths[Index], Storage[Index])
                    .DeliveredExpectedObject);
      ++Arrivals;
      ++ExpectedArrivals;
    }

    if (MovesState) {
      Moved.emplace(std::move(Primary));
      Active = &*Moved;
      RC_ASSERT(Active->IsReady());
      RC_ASSERT(Hooks::UserdataCollectorIsInstalled(*Active));
      VerifyCounts(*Active);
    }

    for (std::size_t Index = 0; Index < Count; ++Index) {
      const GeneratedWrite &Value = Generated[Index];
      ModelRecord &Record = Model[Index];
      if (Record.Lifetime != LifetimeState::Published)
        continue;

      switch (Value.End) {
      case WriteEnding::Nothing:
        break;

      case WriteEnding::InvalidateHandle: {
        const ModelCounters Before = Expected;
        Handles[Index].Invalidate();
        Record.HandleIsLive = false;
        RC_ASSERT(Expected.MetadataRelease == Before.MetadataRelease);
        break;
      }

      case WriteEnding::Release: {
        RC_ASSERT(
            Hooks::ReleaseClassValue(*Active, Storage[Index], Value.Cause));
        const std::uint64_t BeforeDestroy = Expected.Destroy;
        RC_ASSERT(ModelRelease(Record, Expected, false));
        if (Value.DestructionThrows && Expected.Destroy != BeforeDestroy)
          ++ExpectedContained;

        ExpectedHeaderState[Index] = LifetimeState::Invalid;

        RC_ASSERT(
            !Hooks::ReleaseClassValue(*Active, Storage[Index], Value.Cause));
        break;
      }
      }

      const auto Read =
          ReadThroughAccessPath(*Active, Paths[Index], Storage[Index]);
      const bool Permitted = ModelPermitsAccess(Record);
      if (Permitted)
        ++ExpectedArrivals;
      if (Read.ReachedNativeCode)
        ++Arrivals;
      RC_ASSERT(Read.ReachedNativeCode == Permitted);
      if (!Permitted)
        RC_ASSERT(Read.Failure == "expired_userdata");

      const auto Header = Hooks::ObserveClassUserdata(*Active, Paths[Index]);
      RC_ASSERT(Header.has_value());
      RC_ASSERT(Header->Lifetime == ExpectedHeaderState[Index]);
    }

    VerifyCounts(*Active);

    if (CollectsEverything) {
      for (const std::string &Path : Paths)
        RC_ASSERT(Active->Execute(Path + " = nil").IsSuccess());
      RC_ASSERT(Hooks::CollectGarbage(*Active));
      RC_ASSERT(Hooks::CollectGarbage(*Active));

      for (std::size_t Index = 0; Index < Count; ++Index) {
        ModelRecord &Record = Model[Index];
        const std::uint64_t BeforeDestroy = Expected.Destroy;
        if (ModelRelease(Record, Expected, true) &&
            Generated[Index].DestructionThrows &&
            Expected.Destroy != BeforeDestroy)
          ++ExpectedContained;
      }

      VerifyCounts(*Active);
      RC_ASSERT(Hooks::ObserveUserdataCollections().ContainedException == 0);
      RC_ASSERT(Hooks::ObserveUserdataCollections().ForeignBlock == 0);
      RC_ASSERT(Hooks::ObserveUserdataCollections().UnroutedOrigin == 0);

      RC_ASSERT(Hooks::ClassMetatableCreationCount(*Active, "Probe") <= 1);
    }

    if (WrittenValues == 0)
      RC_TAG("write path: every exposure refused");
    else if (CollectsEverything)
      RC_TAG("write path: published then collected");
    else if (CountRecorded() == 0)
      RC_TAG("write path: published then released");
    else
      RC_TAG("write path: values left for State destruction");

    RC_ASSERT(Active->Execute("Recovered = 7").IsSuccess());
    RC_ASSERT(Hooks::ObserveIntegerGlobal(*Active, "Recovered") ==
              std::optional<int>(7));
  }

  for (ModelRecord &Record : Model)
    static_cast<void>(ModelRelease(Record, Expected, false));
  RC_ASSERT(DestroyCalls == Expected.Destroy);
  RC_ASSERT(DeallocateCalls == Expected.Deallocate);
  RC_ASSERT(Arrivals == ExpectedArrivals);

  const auto Destroyed = Hooks::ObserveLastStateDestruction();
  RC_ASSERT(Destroyed.Observed);
  RC_ASSERT(Destroyed.RefusedNewInvocations);
  RC_ASSERT(Destroyed.RetainedCleanupMetadata);
  RC_ASSERT(Destroyed.IncompleteMetadata == 0);
  RC_ASSERT(Hooks::ObserveUserdataCollections().ContainedException == 0);

  for (std::size_t Index = 0; Index < Count; ++Index) {
    if (SharedObjects[Index])
      RC_ASSERT(SharedObjects[Index].use_count() == 1);
    if (BorrowedObjects[Index])
      RC_ASSERT(BorrowedObjects[Index]->Value == 23);
  }
}

} // namespace

int RunUserdataOwnershipLifetimeProperties() {

  const bool Passed = rc::check(

      "Userdata ownership and lifetime transitions match the release state "
      "machine",
      [](const std::vector<std::uint8_t> &OwnershipBytes,
         const std::vector<std::uint8_t> &ExposureBytes) {
        ByteCursor Ownership(OwnershipBytes);
        VerifyGateDrivenSequence(Ownership);

        ByteCursor Exposure(ExposureBytes);
        VerifyWritePathSequence(Exposure);
      });
  return Passed ? 0 : 1;
}
