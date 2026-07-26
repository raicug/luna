// Property 26: userdata ownership and lifetime transitions match the release
// state machine.
//
// Two halves are generated together, and both of them are compared with an
// independent release state machine written here rather than with Luna's own
// accounting.
//
// The first half drives the one idempotent release gate directly, exactly the
// way construction, publication, collection, an explicit invalidation, a
// lifecycle action, and State destruction drive it. It generates borrowed,
// Lua-owned, and shared values, ownership statements that agree with their
// model and statements that deliberately do not, entry through one combined
// exposure or through the individual staging, construction, establishment, and
// publication steps, and one ending per value: nothing, invalidation,
// invalidation of the borrowed lifetime itself, an explicit release under a
// generated cause, a second release, a release entered by the native object
// instead of its value, or the collector's own entry. The model predicts the
// deterministic refusal of every step, the lifetime state of every value, and
// the exact number of times each release step ran: invalidate, cache eviction,
// destroy, shared release, deallocate, and metadata release.
//
// The second half publishes the same three ownership models through exactly the
// conversion write path a returned object takes, then reads them back through
// exactly the access path an ordinary argument takes, so what a refusal costs
// is observable: a refused access never reaches native code at all. It
// generates identical and conflicting re-exposures of one object, borrowed
// lifetime invalidation, explicit release, a State move, a collection of
// everything the script has dropped, and finally State destruction, and
// compares the owner count, the published count, the live cache-entry count,
// every release counter, and the destruction ordering with the same model.
//
// Both halves also check what cleanup must never do. A conflicting re-exposure
// creates no second owner of one object. A destruction step that throws is
// contained and every remaining step still runs exactly once. Releasing the
// same value twice performs no second destruction, no second deallocation, and
// no second shared release. And no cleanup step ever runs without the type,
// allocator, metatable, and dispatch metadata it requires, through the final
// release of the last value.

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

// Deterministic byte source. Equal bytes always drive the equal scenario, so a
// shrunk counterexample replays exactly the same ownership sequence.
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

// ---------------------------------------------------------------------------
// One representative native object, plus the exact number of times each storage
// step ran on it. Nothing in the model reads these; they are what the model's
// predictions are compared against.
// ---------------------------------------------------------------------------

struct Probe final {
  int Value = 23;
};

std::size_t DestroyCalls = 0;
std::size_t DeallocateCalls = 0;

// The per-value policy the storage steps of one value are handed as their
// context. It is what decides whether a destruction reports a failure, and it
// is named by identity rather than by address, so recycled storage never
// inherits another value's behaviour.
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

// The destruction step of one value's protocol. The per-value policy is
// captured by the step itself, which is exactly how the semantic protocol
// carries consumer state: Luna retains the step, so it retains the state with
// it.
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

// A Lua-owned object Luna destroys but never deallocates, so "destroyed exactly
// once" and "deallocated exactly once" stay separately observable.
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

// ---------------------------------------------------------------------------
// The independent release state machine.
// ---------------------------------------------------------------------------

// Exactly how many times each release step must have run. This mirrors the
// counters Luna reports, and every field is predicted from the generated
// sequence alone.
struct ModelCounters final {
  std::uint64_t Invalidate = 0;
  std::uint64_t CacheRemoval = 0;
  std::uint64_t Destroy = 0;
  std::uint64_t SharedRelease = 0;
  std::uint64_t Deallocate = 0;
  std::uint64_t MetadataRelease = 0;
};

// One value the model tracks: its ownership model, whether Luna owns its
// storage, whether it was known-constructed, whether it still holds a shared
// ownership reference, and where it is in the release state machine.
struct ModelRecord final {
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  bool OwnsStorage = false;
  bool HasDestructionStep = false;
  bool WasConstructed = false;
  bool HoldsSharedReference = false;

  // Present only while the value has an ownership record. A value whose record
  // is gone - refused, or released - has none.
  bool IsRecorded = false;
  LifetimeState Lifetime = LifetimeState::Allocated;

  // What the value's own header still says. A release entered by the native
  // object rather than by its value never touches the header, which is exactly
  // why the record - not the header - is what permits access.
  LifetimeState HeaderLifetime = LifetimeState::Allocated;

  // The borrowed lifetime this value was published under is still the declared
  // one. Any other ownership model is always live.
  bool HandleIsLive = true;
};

// The one release gate, expressed once against the model. The order is fixed:
// invalidate, evict the cache entry, destroy an owned constructed object,
// release the one shared ownership reference, deallocate storage Luna
// allocated, and only then release the metadata. Calling it again does nothing.
[[nodiscard]] bool ModelRelease(ModelRecord &Record, ModelCounters &Counted,
                                bool UpdatesHeader) {
  if (!Record.IsRecorded || Record.Lifetime == LifetimeState::Released)
    return false;

  // A value that never got past `Allocated` was never accessible, so it needs
  // no invalidation.
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

// Invalidation stops access without releasing anything, and invalidating an
// already invalid or released value changes nothing.
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

// Access is only permitted while the record and its header both say published,
// and a borrowed value is only reachable while its declared lifetime is still
// the one it was published under.
[[nodiscard]] bool ModelPermitsAccess(const ModelRecord &Record) noexcept {
  if (!Record.IsRecorded || Record.Lifetime != LifetimeState::Published)
    return false;
  if (Record.HeaderLifetime != LifetimeState::Published)
    return false;
  return Record.Ownership != OwnershipModel::Borrowed || Record.HandleIsLive;
}

// ---------------------------------------------------------------------------
// One generated value of the gate-driven half.
// ---------------------------------------------------------------------------

// How the ownership statement of one value relates to its ownership model.
// Every flaw is one deterministic refusal, and none of them makes cleanup
// metadata unreachable: a Lua-owned value always arrives with its destruction
// step, because without one Luna would refuse to own the object at all.
enum class OwnershipFlaw {
  None,
  MissingHandle,
  ExpiredHandle,
  UnexpectedHandle,
  MissingShared,
  UnexpectedShared,
  BorrowedStorage
};

// How the value enters the gate.
enum class EntryMode {
  Exposed,              // one combined exposure
  Stepped,              // stage, construct, establish, publish
  SkippedEstablishment, // stage, construct, publish - refused, then released
  StagedOnly            // stage, then a construction failure
};

// What ends the value.
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

  // Whether Luna allocated the storage and therefore deallocates it, and
  // whether the destruction step of that storage throws.
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

// One flaw the generated ownership model can actually exhibit. A borrowed value
// can lack or outlive its handle or be asked to own its storage; an owning
// model can be handed a handle it must refuse; a shared value can lack its one
// reference and a non-shared value can be handed one.
[[nodiscard]] OwnershipFlaw GeneratedFlaw(OwnershipModel Ownership,
                                          std::size_t Choice) noexcept {
  // Biased so most values publish and every refusal still occurs often. A
  // sequence in which nothing is ever owned would exercise no release step at
  // all.
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

  // Only a borrowed value has a lifetime a consumer can end without releasing
  // anything, so the other two models end the same sequence by invalidation.
  if (Value.End == Ending::InvalidateHandle &&
      Value.Ownership != OwnershipModel::Borrowed)
    Value.End = Ending::Invalidate;

  Value.Cause = GeneratedCause(Cursor.Pick(4));

  // Luna never owns the storage of an object it only borrows, and never
  // deallocates storage a `std::shared_ptr` already owns. Asking it to own
  // borrowed storage is the `BorrowedStorage` flaw, and it is the only way that
  // combination is generated.
  Value.OwnsStorage = Value.Ownership == OwnershipModel::LuaOwned
                          ? Cursor.Pick(4) != 0
                          : Value.Flaw == OwnershipFlaw::BorrowedStorage;
  Value.DestructionThrows =
      Value.Ownership == OwnershipModel::LuaOwned && Cursor.Pick(5) == 0;
  return Value;
}

// The deterministic refusal one ownership statement earns at establishment, or
// none when the statement agrees with its model.
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

// ---------------------------------------------------------------------------
// One State with one registered class, so every header carries a complete, real
// class identity.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// The gate-driven half: one generated ownership sequence, compared step by step
// with the model above.
// ---------------------------------------------------------------------------

void VerifyGateDrivenSequence(ByteCursor &Cursor) {
  ResetStorageCounters();
  Hooks::ResetUserdataCollections();

  const std::size_t Count = 1 + Cursor.Pick(4);
  std::vector<GeneratedValue> Generated;
  Generated.reserve(Count);
  for (std::size_t Index = 0; Index < Count; ++Index)
    Generated.push_back(GenerateValue(Cursor));

  // Storage outlives the State, so a value Luna never releases is still the
  // test's to clean up and a shared reference count is still observable
  // afterwards.
  std::vector<std::unique_ptr<Probe>> BorrowedObjects(Count);
  std::vector<std::shared_ptr<Probe>> SharedObjects(Count);
  std::vector<Probe *> UnownedStorage(Count, nullptr);
  std::vector<Luna::LifetimeHandle> Handles(Count);
  std::vector<std::shared_ptr<Probe>> Spares(Count);

  // One stable policy per value, so the storage steps of one value can never be
  // taken for another value's.
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

      // Luna refuses to own the storage of an object it only borrows, and it
      // refuses that before anything is recorded.
      const bool StageRefused = Value.Ownership == OwnershipModel::Borrowed &&
                                Allocator.OwnsStorage();
      const OwnershipFailure Establishment = ModelEstablishmentFailure(Value);
      const bool Throws = Value.DestructionThrows;

      // Every consequence of one release, predicted by the model rather than
      // read back from Luna.
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
            // A reference Luna must refuse. It names another object entirely,
            // so accepting it could never be mistaken for correct.
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

            // A refused exposure releases exactly what its earlier steps
            // established, and leaves no owner behind.
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
            // Construction never happened, so nothing is destroyed and the
            // staged storage is simply given back.
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

          // Ownership is established exactly once.
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

      // Luna retains exactly one shared ownership reference, and only for a
      // shared value whose ownership was established.
      if (Value.Ownership == OwnershipModel::Shared) {
        RC_ASSERT(SharedObjects[Index].use_count() ==
                  (Record.HoldsSharedReference ? 2 : 1));
      }

      RC_ASSERT(Header.Lifetime == Record.HeaderLifetime);
      RC_ASSERT(Gate.LifetimePermitsAccess(Header) ==
                ModelPermitsAccess(Record));

      if (Record.Lifetime != LifetimeState::Published) {
        // Nothing a refused or already released value is asked to do performs a
        // second step.
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
        // Ending the declared lifetime releases nothing at all; it only makes
        // every later access fail.
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
        // The native object knows itself, not the virtual-machine block that
        // carries it, so this release never touches the header.
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

    // Every counter, predicted from the generated sequence alone.
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

    // Nothing a cleanup step throws escapes the gate, and nothing runs without
    // the metadata it requires.
    RC_ASSERT(Counted.ContainedException == ExpectedContained);
    RC_ASSERT(Counted.IncompleteMetadata == 0);
    RC_ASSERT(Gate.RetainsCleanupMetadata());

    RC_ASSERT(DestroyCalls == Expected.Destroy);
    RC_ASSERT(DeallocateCalls == Expected.Deallocate);
  }

  // State destruction releases everything left, exactly once each, while its
  // cleanup metadata is still valid.
  for (ModelRecord &Record : Model)
    static_cast<void>(ModelRelease(Record, Expected, false));
  RC_ASSERT(DestroyCalls == Expected.Destroy);
  RC_ASSERT(DeallocateCalls == Expected.Deallocate);

  const auto Destroyed = Hooks::ObserveLastStateDestruction();
  RC_ASSERT(Destroyed.Observed);
  RC_ASSERT(Destroyed.RefusedNewInvocations);
  RC_ASSERT(Destroyed.RetainedCleanupMetadata);
  RC_ASSERT(Destroyed.IncompleteMetadata == 0);

  // None of these values was ever a virtual-machine block, so the machine
  // finalized none of them and the explicit final sweep released every one.
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

// ---------------------------------------------------------------------------
// The write-path half: the same three ownership models published through
// exactly the conversion write path a returned object takes, read back through
// exactly the access path an ordinary argument takes.
// ---------------------------------------------------------------------------

// The ownership statement of one exposure, and whether it agrees with its
// model.
enum class WriteVariant { Valid, ExpiredHandle, MissingHandle, MissingShared };

// What ends one published value.
enum class WriteEnding { Nothing, InvalidateHandle, Release };

// What one re-exposure of an already exposed object asks for.
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

  // Only the refusals a consumer can actually cause are generated: a borrowed
  // object whose declared lifetime is missing or already over, and a shared
  // object with no shared ownership reference.
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

// The deterministic refusal text the write path reports for one variant, and
// "none" for a statement that agrees with its model.
[[nodiscard]] std::string_view
ExpectedWriteFailure(const GeneratedWrite &Value) noexcept {
  switch (Value.Variant) {
  case WriteVariant::ExpiredHandle:
  case WriteVariant::MissingHandle:
    // The object's owner already ended, or never declared, the lifetime the
    // value would be exposed under.
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

    // One exposure per generated value, each through exactly the conversion
    // write path a returned object takes.
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

      // A refused exposure publishes nothing and leaves the stack exactly as it
      // found it.
      RC_ASSERT(Written.FinalStackDepth == Written.EntryStackDepth);

      if (Written.Published) {
        Record.IsRecorded = true;
        Record.WasConstructed = true;
        Record.Lifetime = LifetimeState::Published;
        Record.HeaderLifetime = LifetimeState::Published;
        Record.HoldsSharedReference = Value.Ownership == OwnershipModel::Shared;
      } else if (Value.Variant == WriteVariant::MissingShared) {
        // The cache created nothing, but ownership establishment ran against a
        // staged, known-constructed record, so its release is observable.
        Record.IsRecorded = true;
        Record.WasConstructed = true;
        Record.Lifetime = LifetimeState::Constructed;
        Record.HeaderLifetime = LifetimeState::Constructed;
        RC_ASSERT(ModelRelease(Record, Expected, true));
      }

      if (Written.Published)
        ++WrittenValues;
    }

    // Exactly the values the model says exist, owned once each, published once
    // each, and recorded in the identity cache once each.
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

    // The class metatable is a consequence of publishing a value, never of
    // registering a class or of refusing an exposure, and one class owns
    // exactly one of them.
    bool AnyPublished = false;
    for (const ModelRecord &Record : Model) {
      AnyPublished =
          AnyPublished || Record.Lifetime == LifetimeState::Published;
    }
    RC_ASSERT(Hooks::ClassMetatableIsCreated(*Active, "Probe") == AnyPublished);
    RC_ASSERT(Hooks::ClassMetatableCreationCount(*Active, "Probe") ==
              (AnyPublished ? 1U : 0U));

    // Every published value reaches native code and delivers exactly its own
    // object; nothing else does.
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

    // One re-exposure of the first published object. A compatible request hands
    // back the value that already exists; an incompatible one is refused
    // without creating a second owner.
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

      // Neither answer creates a second owner, spends a release step, or
      // retains a second shared ownership reference.
      RC_ASSERT(Expected.MetadataRelease == Before.MetadataRelease);
      VerifyCounts(*Active);
      RC_ASSERT(ReadThroughAccessPath(*Active, Paths[Index], Storage[Index])
                    .DeliveredExpectedObject);
      ++Arrivals;
      ++ExpectedArrivals;
    }

    // A State move preserves the logical identity every value and every route
    // is keyed by, so nothing about ownership changes.
    if (MovesState) {
      Moved.emplace(std::move(Primary));
      Active = &*Moved;
      RC_ASSERT(Active->IsReady());
      RC_ASSERT(Hooks::UserdataCollectorIsInstalled(*Active));
      VerifyCounts(*Active);
    }

    // One generated ending per published value.
    for (std::size_t Index = 0; Index < Count; ++Index) {
      const GeneratedWrite &Value = Generated[Index];
      ModelRecord &Record = Model[Index];
      if (Record.Lifetime != LifetimeState::Published)
        continue;

      switch (Value.End) {
      case WriteEnding::Nothing:
        break;

      case WriteEnding::InvalidateHandle: {
        // Ending the declared lifetime releases nothing and destroys nothing;
        // it only stops every later access, before any native code runs.
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

        // The identity-cache entry went before the payload, so the value's own
        // header already refuses every access.
        ExpectedHeaderState[Index] = LifetimeState::Invalid;

        // The gate is idempotent: releasing the same object again performs no
        // second destruction, deallocation, or shared release.
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

    // Everything the script has dropped is collected, and every value the
    // collector reaches ends through exactly the same gate.
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

      // The class keeps its one metatable through a collection of every value.
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

    // The State keeps executing and exposing after every generated refusal,
    // release, and collection.
    RC_ASSERT(Active->Execute("Recovered = 7").IsSuccess());
    RC_ASSERT(Hooks::ObserveIntegerGlobal(*Active, "Recovered") ==
              std::optional<int>(7));
  }

  // State destruction releases everything left, exactly once each.
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
  // clang-format off
  // **Validates: Requirements 11.3, 11.4, 11.5, 11.6, 11.7, 11.8, 11.9, 11.10, 11.11**
  // Feature: reflection-driven-binding-system, Property 26: Userdata ownership and lifetime transitions match the release state machine
  const bool Passed = rc::check(
      // clang-format on
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
