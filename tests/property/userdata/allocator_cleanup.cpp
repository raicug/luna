// Property 27: construction and allocator cleanup follow completed milestones
// exactly once.
//
// Creating a native object on behalf of a script is a walk through four
// milestones - storage exists, an object is constructed in it, ownership of
// that object is established, the value is published - and the cleanup a
// failure warrants is decided by the milestones that completed, never by which
// step happened to fail. An allocation that produced nothing is cleaned up by
// nothing at all. Storage nothing was constructed in is given back without
// being destroyed. An object that exists but is not owned is destroyed, then
// whatever ownership took is released, then its storage is given back, in that
// order and once each. And a published value ends through exactly the same
// gate, once.
//
// Two halves are generated together, and both are compared with an independent
// milestone model written here rather than with Luna's own accounting.
//
// The first half drives the milestones directly, exactly the way a constructor
// candidate drives them: the semantic allocation step, staging, the
// construction step - either one the caller supplied or the protocol's own -
// ownership establishment, and publication. It generates protocols that declare
// every step and protocols that deliberately declare fewer, allocations and
// constructions that succeed, decline, and throw, ownership statements that
// agree with their model and statements that do not, a lifetime ended between
// establishment and publication, destruction steps that throw, and one ending
// per published value.
//
// The second half constructs values through exactly the conversion write path a
// constructor or factory return takes, so what a refusal costs is observable
// from the outside: nothing published at the path it named, nothing left on the
// stack, no owner behind, no identity-cache entry, and no class metatable that
// a refused construction created. It then ends those values by release, by
// ending a borrowed lifetime, by collection, and by State destruction.
//
// Both halves check what cleanup must never do: no destruction of storage
// nothing was constructed in, no step performed twice, no step performed that
// its protocol never declared, no exception escaping into a collector or a
// State destructor, and no cleanup step running without the metadata it
// requires. And after every generated refusal the State still constructs,
// executes, and publishes.

// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/lifetime_handle.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/test_hooks.hpp"
#include "state/userdata/allocator.hpp"
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
using Luna::Detail::ConstructionMilestone;
using Luna::Detail::ConstructionMilestoneText;
using Luna::Detail::LifetimeState;
using Luna::Detail::ObjectConstruction;
using Luna::Detail::OwnershipFailure;
using Luna::Detail::OwnershipModel;
using Luna::Detail::OwnershipRegistry;
using Luna::Detail::OwnershipRequest;
using Luna::Detail::ReleaseCause;
using Luna::Detail::StagedStorage;
using Luna::Detail::StorageAllocationOutcome;
using Luna::Detail::UserdataHeader;

// Deterministic byte source. Equal bytes always drive the equal scenario, so a
// shrunk counterexample replays exactly the same construction sequence.
class ByteCursor final {
public:
  explicit ByteCursor(const std::vector<std::uint8_t> &Bytes) noexcept
      : BytesValue(&Bytes) {}

  [[nodiscard]] std::uint8_t Next() noexcept {
    const std::size_t Index = IndexValue++;
    if (BytesValue->empty())
      return static_cast<std::uint8_t>(Index * 41U + 7U);
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
// One representative native object, and one per-value protocol whose steps
// record exactly what they were asked to do.
// ---------------------------------------------------------------------------

struct Probe final {
  int Value = 0;
  bool IsLive = false;
};

// The state one generated protocol captures, and the observations its steps
// leave behind. It belongs to exactly one generated value and is carried by the
// steps themselves - which is how a consumer's arena travels with its protocol
// - so recycled storage can never make one value inherit another's behaviour.
struct StoragePolicy final {
  // What the steps of this one value are asked to do.
  bool AllocationDeclines = false;
  bool AllocationThrows = false;
  bool ConstructionDeclines = false;
  bool ConstructionThrows = false;
  bool DestructionThrows = false;
  int ConstructedValue = 0;

  // Exactly how many times each declared step ran on this one value.
  unsigned AllocateCalls = 0;
  unsigned ConstructCalls = 0;
  unsigned DestroyCalls = 0;
  unsigned DeallocateCalls = 0;

  // The storage this value's allocation step produced, and whether an object is
  // constructed in it right now.
  void *Storage = nullptr;
  bool ObjectIsLive = false;

  // Set if a destruction step was ever handed storage nothing was constructed
  // in. It must stay false: that is the one thing the protocol promises never
  // to do.
  bool DestroyedUnconstructed = false;
};

// The four steps of one generated protocol. Each one is an ordinary callable
// over the per-value policy, exactly as a consumer would write it.
[[nodiscard]] ClassAllocator::AllocateOperation
AllocationStep(StoragePolicy *Policy) {
  return [Policy](const StorageRequest &Wanted) -> void * {
    ++Policy->AllocateCalls;
    if (Policy->AllocationThrows)
      throw std::runtime_error("probe allocation reported a failure");
    if (Policy->AllocationDeclines)
      return nullptr;
    Policy->Storage = ::operator new(Wanted.ByteCount);
    return Policy->Storage;
  };
}

[[nodiscard]] ClassAllocator::ConstructOperation
ConstructionStep(StoragePolicy *Policy) {
  return [Policy](void *Storage) {
    ++Policy->ConstructCalls;
    if (Policy->ConstructionThrows)
      throw std::runtime_error("probe construction reported a failure");
    if (Policy->ConstructionDeclines)
      return AllocatorStepResult::Declined();

    Probe *Built = new (Storage) Probe{};
    Built->Value = Policy->ConstructedValue;
    Built->IsLive = true;
    Policy->ObjectIsLive = true;
    return AllocatorStepResult::Done();
  };
}

[[nodiscard]] ClassAllocator::DestroyOperation
DestructionStep(StoragePolicy *Policy) {
  return [Policy](void *Storage) {
    ++Policy->DestroyCalls;
    if (!Policy->ObjectIsLive)
      Policy->DestroyedUnconstructed = true;
    Policy->ObjectIsLive = false;
    static_cast<Probe *>(Storage)->~Probe();
    if (Policy->DestructionThrows)
      throw std::runtime_error("probe destruction reported a failure");
    return AllocatorStepResult::Done();
  };
}

[[nodiscard]] ClassAllocator::DeallocateOperation
DeallocationStep(StoragePolicy *Policy) {
  return [Policy](void *Storage, const StorageRequest &) {
    ++Policy->DeallocateCalls;
    ::operator delete(Storage);
    Policy->Storage = nullptr;
    return AllocatorStepResult::Done();
  };
}

// Storage the protocol produced and Luna never gave back, because the protocol
// never declared a step that could. The test owns it, so the test frees it.
void FreeUnreleasedStorage(StoragePolicy &Policy) {
  if (Policy.Storage == nullptr)
    return;
  ::operator delete(Policy.Storage);
  Policy.Storage = nullptr;
}

std::uint64_t NextNonce = 0;

// ---------------------------------------------------------------------------
// One generated construction.
// ---------------------------------------------------------------------------

// Which steps the generated protocol declares at all. The declared steps are
// the whole cleanup rule, so declaring fewer of them is a statement rather than
// a defect.
enum class ProtocolShape {
  // Allocation, construction, destruction, and deallocation as generated.
  Complete,

  // No allocation step: this protocol can only describe an object that already
  // exists, so it can never create one.
  NoAllocationStep,

  // No construction step of any kind, neither the protocol's own nor one the
  // caller supplied, so no object can come into existence.
  NoConstructionStep
};

// What one generated step does when it runs.
enum class StepOutcome { Performs, Declines, Throws };

// How the ownership statement of one value relates to its ownership model, and
// what happens to a declared lifetime between establishment and publication.
enum class OwnershipStatement {
  Valid,
  MissingHandle,
  ExpiredHandle,
  UnexpectedHandle,
  MissingShared,

  // Publication attempted without establishing ownership at all.
  SkippedEstablishment,

  // Ownership established, then the declared lifetime ended before the value
  // could be published. It is the one way a value reaches the `Owned` milestone
  // and still never becomes visible.
  ExpiredAfterEstablishment
};

// What ends one published value.
enum class Ending {
  Nothing,
  Release,
  ReleaseTwice,
  ReleaseByStorage,
  Collected,
  InvalidateHandle
};

struct GeneratedConstruction final {
  OwnershipModel Ownership = OwnershipModel::LuaOwned;
  ProtocolShape Shape = ProtocolShape::Complete;

  // Whether the caller supplies the one construction step, and whether the
  // protocol declares one of its own. Only one of them ever runs.
  bool SuppliesConstruction = false;
  bool DeclaresConstruction = true;

  bool DeclaresDestruction = true;
  bool OwnsStorage = true;

  StepOutcome Allocation = StepOutcome::Performs;
  StepOutcome Construction = StepOutcome::Performs;
  OwnershipStatement Statement = OwnershipStatement::Valid;
  bool DestructionThrows = false;
  Ending End = Ending::Nothing;
  int ConstructedValue = 0;
};

// Whether one generated protocol can create an object at all: it needs a step
// that produces storage and a step that constructs into it.
[[nodiscard]] bool
DeclaresCreation(const GeneratedConstruction &Value) noexcept {
  if (Value.Shape == ProtocolShape::NoAllocationStep)
    return false;
  return Value.SuppliesConstruction || Value.DeclaresConstruction;
}

[[nodiscard]] bool
HasConstructionStep(const GeneratedConstruction &Value) noexcept {
  return Value.SuppliesConstruction || Value.DeclaresConstruction;
}

[[nodiscard]] ClassAllocator ProtocolFor(const GeneratedConstruction &Value,
                                         StoragePolicy *Policy) {
  ClassAllocator::AllocateOperation Allocate;
  if (Value.Shape != ProtocolShape::NoAllocationStep)
    Allocate = AllocationStep(Policy);

  ClassAllocator::ConstructOperation Construct;
  if (Value.DeclaresConstruction)
    Construct = ConstructionStep(Policy);

  ClassAllocator::DestroyOperation Destroy;
  if (Value.DeclaresDestruction)
    Destroy = DestructionStep(Policy);

  ClassAllocator::DeallocateOperation Deallocate;
  if (Value.OwnsStorage)
    Deallocate = DeallocationStep(Policy);

  return ClassAllocator::FromOperations(
      "Studio.MilestoneArena", StorageRequest::ForClass<Probe>(),
      std::move(Allocate), std::move(Construct), std::move(Destroy),
      std::move(Deallocate));
}

// The one construction step a caller supplies, when the generated value says it
// does. It reports whether an object now exists, which is all the gate needs.
[[nodiscard]] ObjectConstruction SuppliedConstruction(StoragePolicy *Policy) {
  return [Policy](void *Storage) {
    ++Policy->ConstructCalls;
    if (Policy->ConstructionThrows)
      throw std::runtime_error("supplied construction reported a failure");
    if (Policy->ConstructionDeclines)
      return false;

    Probe *Built = new (Storage) Probe{};
    Built->Value = Policy->ConstructedValue;
    Built->IsLive = true;
    Policy->ObjectIsLive = true;
    return true;
  };
}

[[nodiscard]] std::unique_ptr<StoragePolicy>
PolicyFor(const GeneratedConstruction &Value) {
  auto Policy = std::make_unique<StoragePolicy>();
  Policy->AllocationDeclines = Value.Allocation == StepOutcome::Declines;
  Policy->AllocationThrows = Value.Allocation == StepOutcome::Throws;
  Policy->ConstructionDeclines = Value.Construction == StepOutcome::Declines;
  Policy->ConstructionThrows = Value.Construction == StepOutcome::Throws;
  Policy->DestructionThrows = Value.DestructionThrows;
  Policy->ConstructedValue = Value.ConstructedValue;
  return Policy;
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// The independent milestone model.
// ---------------------------------------------------------------------------

// Exactly how many times each build step must have run, mirroring what the
// State reports.
struct ModelBuildCounters final {
  std::uint64_t Allocate = 0;
  std::uint64_t AllocationFailure = 0;
  std::uint64_t Construct = 0;
  std::uint64_t ConstructionFailure = 0;
  std::uint64_t ContainedException = 0;
};

// Exactly how many times each release step must have run.
struct ModelReleaseCounters final {
  std::uint64_t Invalidate = 0;
  std::uint64_t CacheRemoval = 0;
  std::uint64_t Destroy = 0;
  std::uint64_t SharedRelease = 0;
  std::uint64_t Deallocate = 0;
  std::uint64_t MetadataRelease = 0;
  std::uint64_t ContainedException = 0;
};

// Everything the milestone model predicts about one attempted construction,
// derived from the generated value alone: how far it gets, which deterministic
// refusal stops it, how many times each of its protocol's steps runs, and every
// counter the attempt and its cleanup spend.
struct MilestonePrediction final {
  ConstructionMilestone Reached = ConstructionMilestone::None;
  OwnershipFailure Refusal = OwnershipFailure::None;

  // Whether the attempt was refused before its storage could even be requested,
  // and whether the refusal was one a value's own owner caused by ending or
  // never declaring the lifetime it would be exposed under.
  bool RefusedBeforeAllocation = false;
  bool RefusedLifetime = false;

  // Whether a record ever described this value, and whether it was published.
  bool Staged = false;
  bool Published = false;

  // Protocol step calls the attempt and its cleanup perform.
  unsigned AllocateCalls = 0;
  unsigned ConstructCalls = 0;
  unsigned DestroyCalls = 0;
  unsigned DeallocateCalls = 0;

  ModelBuildCounters Built;
  ModelReleaseCounters Released;
};

// The deterministic refusal one ownership statement earns at establishment, and
// `None` when the statement agrees with its model.
[[nodiscard]] OwnershipFailure
EstablishmentRefusal(const GeneratedConstruction &Value) noexcept {
  switch (Value.Ownership) {
  case OwnershipModel::Borrowed:
    if (Value.Statement == OwnershipStatement::MissingHandle)
      return OwnershipFailure::MissingLifetimeHandle;
    if (Value.Statement == OwnershipStatement::ExpiredHandle)
      return OwnershipFailure::ExpiredLifetimeHandle;
    return OwnershipFailure::None;
  case OwnershipModel::LuaOwned:
    if (Value.Statement == OwnershipStatement::UnexpectedHandle)
      return OwnershipFailure::UnexpectedLifetimeHandle;
    return OwnershipFailure::None;
  case OwnershipModel::Shared:
    if (Value.Statement == OwnershipStatement::UnexpectedHandle)
      return OwnershipFailure::UnexpectedLifetimeHandle;
    if (Value.Statement == OwnershipStatement::MissingShared)
      return OwnershipFailure::MissingSharedOwnership;
    return OwnershipFailure::None;
  }
  return OwnershipFailure::None;
}

// The whole cleanup rule of one milestone, expressed once. A value that never
// became accessible needs no invalidation; an object that exists is destroyed
// only when Luna owns it; ownership releases exactly what it took; storage is
// given back only by a protocol that declares how; and the metadata goes last.
void PredictCleanup(const GeneratedConstruction &Value,
                    MilestonePrediction &Predicted, bool WasConstructed,
                    bool HoldsSharedReference, bool WasAccessible) {
  if (WasAccessible)
    ++Predicted.Released.Invalidate;
  ++Predicted.Released.CacheRemoval;

  if (Value.Ownership == OwnershipModel::LuaOwned && WasConstructed) {
    ++Predicted.Released.Destroy;
    if (Value.DeclaresDestruction) {
      ++Predicted.DestroyCalls;
      if (Value.DestructionThrows)
        ++Predicted.Released.ContainedException;
    }
  }

  if (HoldsSharedReference)
    ++Predicted.Released.SharedRelease;

  if (Value.OwnsStorage) {
    ++Predicted.Released.Deallocate;
    ++Predicted.DeallocateCalls;
  }

  ++Predicted.Released.MetadataRelease;
}

// How far one generated construction gets, and what its failure costs. The
// write path is told apart from the direct milestone walk in exactly two ways:
// it refuses a protocol that could never create an object, and it refuses a
// borrowed value whose lifetime is missing or already over, both before any
// storage is requested.
[[nodiscard]] MilestonePrediction Predict(const GeneratedConstruction &Value,
                                          bool WritePath) {
  MilestonePrediction Predicted;

  if (WritePath) {
    if (!DeclaresCreation(Value)) {
      Predicted.RefusedBeforeAllocation = true;
      return Predicted;
    }
    if (Value.Ownership == OwnershipModel::Borrowed &&
        (Value.Statement == OwnershipStatement::MissingHandle ||
         Value.Statement == OwnershipStatement::ExpiredHandle)) {
      Predicted.RefusedBeforeAllocation = true;
      Predicted.RefusedLifetime = true;
      Predicted.Refusal = EstablishmentRefusal(Value);
      return Predicted;
    }
  }

  // The semantic allocation step. A step the protocol never declared is never
  // called, and an allocation that produced nothing is cleaned up by nothing at
  // all.
  const bool DeclaresAllocation =
      Value.Shape != ProtocolShape::NoAllocationStep;
  if (DeclaresAllocation)
    ++Predicted.AllocateCalls;
  if (!DeclaresAllocation || Value.Allocation != StepOutcome::Performs) {
    ++Predicted.Built.AllocationFailure;
    if (DeclaresAllocation && Value.Allocation == StepOutcome::Throws)
      ++Predicted.Built.ContainedException;
    return Predicted;
  }

  ++Predicted.Built.Allocate;
  Predicted.Reached = ConstructionMilestone::Allocated;

  // Luna never owns the storage of an object it only borrows, so staging
  // refuses that before any record describes it. The storage is given straight
  // back, which is the one deallocation that happens outside the release gate.
  if (Value.Ownership == OwnershipModel::Borrowed && Value.OwnsStorage) {
    Predicted.Refusal = OwnershipFailure::BorrowedStorageOwnership;
    ++Predicted.Released.Deallocate;
    ++Predicted.DeallocateCalls;
    return Predicted;
  }
  Predicted.Staged = true;

  // The one construction step, whether the caller supplied it or the protocol
  // declares its own. Either way, a step that declined or threw means no object
  // exists.
  if (HasConstructionStep(Value))
    ++Predicted.ConstructCalls;
  if (!HasConstructionStep(Value) ||
      Value.Construction != StepOutcome::Performs) {
    ++Predicted.Built.ConstructionFailure;
    if (HasConstructionStep(Value) && Value.Construction == StepOutcome::Throws)
      ++Predicted.Built.ContainedException;
    Predicted.Refusal = OwnershipFailure::RefusedConstruction;
    PredictCleanup(Value, Predicted, false, false, false);
    return Predicted;
  }

  ++Predicted.Built.Construct;
  Predicted.Reached = ConstructionMilestone::Constructed;

  const OwnershipFailure Establishment = EstablishmentRefusal(Value);
  if (Establishment != OwnershipFailure::None ||
      Value.Statement == OwnershipStatement::SkippedEstablishment) {
    Predicted.Refusal =
        Value.Statement == OwnershipStatement::SkippedEstablishment
            ? OwnershipFailure::MissingOwnership
            : Establishment;
    PredictCleanup(Value, Predicted, true, false, true);
    return Predicted;
  }

  Predicted.Reached = ConstructionMilestone::Owned;
  const bool HoldsShared = Value.Ownership == OwnershipModel::Shared;

  // A declared lifetime that ended between establishment and publication: the
  // value is completely owned and still never becomes visible, so its cleanup
  // releases everything ownership took.
  if (Value.Statement == OwnershipStatement::ExpiredAfterEstablishment) {
    Predicted.Refusal = OwnershipFailure::ExpiredLifetimeHandle;
    PredictCleanup(Value, Predicted, true, HoldsShared, true);
    return Predicted;
  }

  Predicted.Reached = ConstructionMilestone::Published;
  Predicted.Published = true;
  return Predicted;
}

void Accumulate(ModelBuildCounters &Built, ModelReleaseCounters &Released,
                const MilestonePrediction &Predicted) {
  Built.Allocate += Predicted.Built.Allocate;
  Built.AllocationFailure += Predicted.Built.AllocationFailure;
  Built.Construct += Predicted.Built.Construct;
  Built.ConstructionFailure += Predicted.Built.ConstructionFailure;
  Built.ContainedException += Predicted.Built.ContainedException;

  Released.Invalidate += Predicted.Released.Invalidate;
  Released.CacheRemoval += Predicted.Released.CacheRemoval;
  Released.Destroy += Predicted.Released.Destroy;
  Released.SharedRelease += Predicted.Released.SharedRelease;
  Released.Deallocate += Predicted.Released.Deallocate;
  Released.MetadataRelease += Predicted.Released.MetadataRelease;
  Released.ContainedException += Predicted.Released.ContainedException;
}

// One published value the model still tracks, together with the step calls its
// protocol has performed so far.
struct ModelValue final {
  OwnershipModel Ownership = OwnershipModel::LuaOwned;
  bool OwnsStorage = false;
  bool DeclaresDestruction = false;
  bool DestructionThrows = false;

  bool IsRecorded = false;
  bool WasConstructed = false;
  bool HoldsSharedReference = false;
  LifetimeState Lifetime = LifetimeState::Allocated;
  LifetimeState HeaderLifetime = LifetimeState::Allocated;
  bool HandleIsLive = true;

  unsigned DestroySteps = 0;
  unsigned DeallocateSteps = 0;
};

// The one idempotent release gate, expressed once against the model. The order
// is fixed: stop access, evict the cache entry, destroy an owned constructed
// object, release the one shared ownership reference, give owned storage back,
// and only then release the metadata. Calling it again does nothing.
[[nodiscard]] bool ModelRelease(ModelValue &Record,
                                ModelReleaseCounters &Counted,
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
    if (Record.DeclaresDestruction) {
      ++Record.DestroySteps;
      if (Record.DestructionThrows)
        ++Counted.ContainedException;
    }
    Record.WasConstructed = false;
  }

  if (Record.HoldsSharedReference) {
    ++Counted.SharedRelease;
    Record.HoldsSharedReference = false;
  }

  if (Record.OwnsStorage) {
    ++Counted.Deallocate;
    ++Record.DeallocateSteps;
  }

  ++Counted.MetadataRelease;
  Record.Lifetime = LifetimeState::Released;
  Record.IsRecorded = false;
  if (UpdatesHeader)
    Record.HeaderLifetime = LifetimeState::Released;
  return true;
}

// Access is permitted only while the record and the value's own header both say
// published, and a borrowed value only while its declared lifetime is still the
// one it was published under.
[[nodiscard]] bool ModelPermitsAccess(const ModelValue &Record) noexcept {
  if (!Record.IsRecorded || Record.Lifetime != LifetimeState::Published)
    return false;
  if (Record.HeaderLifetime != LifetimeState::Published)
    return false;
  return Record.Ownership != OwnershipModel::Borrowed || Record.HandleIsLive;
}

// ---------------------------------------------------------------------------
// Generation.
// ---------------------------------------------------------------------------

// Biased so most steps perform their work and every refusal still occurs often.
// A sequence in which nothing is ever constructed would exercise no cleanup at
// all.
[[nodiscard]] StepOutcome GeneratedStepOutcome(std::size_t Choice) noexcept {
  switch (Choice % 10) {
  case 8:
    return StepOutcome::Declines;
  case 9:
    return StepOutcome::Throws;
  default:
    break;
  }
  return StepOutcome::Performs;
}

[[nodiscard]] OwnershipStatement GeneratedStatement(ByteCursor &Cursor,
                                                    OwnershipModel Ownership,
                                                    bool WritePath) noexcept {
  const std::size_t Choice = Cursor.Pick(8);
  switch (Ownership) {
  case OwnershipModel::Borrowed:
    switch (Choice) {
    case 4:
      return OwnershipStatement::MissingHandle;
    case 5:
      return OwnershipStatement::ExpiredHandle;
    case 6:
      return WritePath ? OwnershipStatement::Valid
                       : OwnershipStatement::SkippedEstablishment;
    case 7:
      return WritePath ? OwnershipStatement::Valid
                       : OwnershipStatement::ExpiredAfterEstablishment;
    default:
      break;
    }
    return OwnershipStatement::Valid;
  case OwnershipModel::LuaOwned:
    switch (Choice) {
    case 6:
      return WritePath ? OwnershipStatement::Valid
                       : OwnershipStatement::UnexpectedHandle;
    case 7:
      return WritePath ? OwnershipStatement::Valid
                       : OwnershipStatement::SkippedEstablishment;
    default:
      break;
    }
    return OwnershipStatement::Valid;
  case OwnershipModel::Shared:
    switch (Choice) {
    case 5:
      return OwnershipStatement::MissingShared;
    case 6:
      return OwnershipStatement::UnexpectedHandle;
    case 7:
      return OwnershipStatement::SkippedEstablishment;
    default:
      break;
    }
    return OwnershipStatement::Valid;
  }
  return OwnershipStatement::Valid;
}

[[nodiscard]] GeneratedConstruction
GenerateConstruction(ByteCursor &Cursor, bool WritePath, int Serial) {
  GeneratedConstruction Value;

  // A constructed object is usually Lua's to own; the other two models are what
  // a factory or a singleton accessor hands back. Shared ownership of a value
  // Luna created is driven through the milestone walk, where the shared
  // reference can name exactly the storage the protocol produced.
  const std::size_t OwnershipChoice = Cursor.Pick(8);
  if (WritePath) {
    Value.Ownership = OwnershipChoice < 6 ? OwnershipModel::LuaOwned
                                          : OwnershipModel::Borrowed;
  } else if (OwnershipChoice < 5) {
    Value.Ownership = OwnershipModel::LuaOwned;
  } else if (OwnershipChoice < 7) {
    Value.Ownership = OwnershipModel::Shared;
  } else {
    Value.Ownership = OwnershipModel::Borrowed;
  }

  switch (Cursor.Pick(16)) {
  case 0:
    Value.Shape = ProtocolShape::NoAllocationStep;
    break;
  case 1:
    Value.Shape = ProtocolShape::NoConstructionStep;
    break;
  default:
    Value.Shape = ProtocolShape::Complete;
    break;
  }

  if (Value.Shape == ProtocolShape::NoConstructionStep) {
    Value.SuppliesConstruction = false;
    Value.DeclaresConstruction = false;
  } else {
    Value.SuppliesConstruction = Cursor.Pick(2) == 0;
    Value.DeclaresConstruction =
        Value.SuppliesConstruction ? Cursor.Pick(2) == 0 : true;
  }

  Value.OwnsStorage = Cursor.Pick(4) != 0;

  // A Lua-owned object is only ownable while its destruction step is reachable,
  // so Luna refuses to own one without it. Every other model may declare it or
  // not.
  Value.DeclaresDestruction =
      Value.Ownership == OwnershipModel::LuaOwned || Cursor.Pick(2) == 0;

  Value.Allocation = GeneratedStepOutcome(Cursor.Pick(10));
  Value.Construction = GeneratedStepOutcome(Cursor.Pick(10));
  Value.Statement = GeneratedStatement(Cursor, Value.Ownership, WritePath);
  Value.DestructionThrows = Value.DeclaresDestruction && Cursor.Pick(6) == 0;
  Value.ConstructedValue = 100 + Serial;

  switch (Cursor.Pick(8)) {
  case 0:
    Value.End = Ending::Nothing;
    break;
  case 1:
  case 2:
    Value.End = Ending::Release;
    break;
  case 3:
    Value.End = Ending::ReleaseTwice;
    break;
  case 4:
    Value.End = WritePath ? Ending::Release : Ending::ReleaseByStorage;
    break;
  case 5:
    Value.End = Ending::Collected;
    break;
  case 6:
    Value.End = Value.Ownership == OwnershipModel::Borrowed
                    ? Ending::InvalidateHandle
                    : Ending::Release;
    break;
  default:
    Value.End = Ending::Nothing;
    break;
  }
  return Value;
}

// One State with one registered class, so every constructed value carries a
// complete, real class identity.
class Fixture final {
public:
  Fixture() {
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Probe> Class = Registry.RegisterClass<Probe>(
        "Probe", Luna::StableTypeKey("Studio.MilestoneProbe"));
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

// Which milestone families one generated sequence exercised. It is the outcome
// distribution of the property, so it names the interesting mix rather than one
// value.
[[nodiscard]] std::string_view
MilestoneTag(const std::vector<MilestonePrediction> &Predicted) noexcept {
  bool AnyPublished = false;
  bool AnyUnallocated = false;
  bool AnyUnconstructed = false;
  bool AnyUnowned = false;
  for (const MilestonePrediction &Wanted : Predicted) {
    AnyPublished = AnyPublished || Wanted.Published;
    AnyUnallocated =
        AnyUnallocated || Wanted.Reached == ConstructionMilestone::None;
    AnyUnconstructed = AnyUnconstructed ||
                       (Wanted.Reached == ConstructionMilestone::Allocated);
    AnyUnowned = AnyUnowned ||
                 Wanted.Reached == ConstructionMilestone::Constructed ||
                 Wanted.Reached == ConstructionMilestone::Owned;
  }

  if (!AnyPublished)
    return "milestones: no value reached publication";
  if (!AnyUnallocated && !AnyUnconstructed && !AnyUnowned)
    return "milestones: every value published";
  if (AnyUnowned)
    return "milestones: publication mixed with ownership failures";
  if (AnyUnconstructed)
    return "milestones: publication mixed with construction failures";
  return "milestones: publication mixed with allocation failures";
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// The milestone walk: exactly the steps a constructor candidate performs,
// compared with the model above at every milestone.
// ---------------------------------------------------------------------------

void VerifyMilestoneWalk(ByteCursor &Cursor) {
  Hooks::ResetUserdataCollections();

  const std::size_t Count = 1 + Cursor.Pick(4);
  std::vector<GeneratedConstruction> Generated;
  std::vector<MilestonePrediction> Predicted;
  Generated.reserve(Count);
  Predicted.reserve(Count);
  for (std::size_t Index = 0; Index < Count; ++Index) {
    Generated.push_back(
        GenerateConstruction(Cursor, false, static_cast<int>(Index)));
    Predicted.push_back(Predict(Generated[Index], false));
  }

  // One stable policy per value, carried by that value's own steps, so recycled
  // storage can never make one value's behaviour another value's.
  std::vector<std::unique_ptr<StoragePolicy>> Policies(Count);
  for (std::size_t Index = 0; Index < Count; ++Index)
    Policies[Index] = PolicyFor(Generated[Index]);

  std::vector<Luna::LifetimeHandle> Handles(Count);
  std::vector<std::shared_ptr<void>> SharedOwners(Count);
  std::vector<ModelValue> Model(Count);
  ModelBuildCounters Built;
  ModelReleaseCounters Expected;
  std::size_t RemainingBeforeDestruction = 0;

  {
    Fixture Owner;
    RC_ASSERT(Owner.IsUsable());
    OwnershipRegistry &Gate = Owner.Gateway();

    for (std::size_t Index = 0; Index < Count; ++Index) {
      const GeneratedConstruction &Value = Generated[Index];
      const MilestonePrediction &Wanted = Predicted[Index];
      StoragePolicy &Policy = *Policies[Index];
      ModelValue &Record = Model[Index];
      Record.Ownership = Value.Ownership;
      Record.OwnsStorage = Value.OwnsStorage;
      Record.DeclaresDestruction = Value.DeclaresDestruction;
      Record.DestructionThrows = Value.DestructionThrows;
      Record.DestroySteps = Wanted.DestroyCalls;
      Record.DeallocateSteps = Wanted.DeallocateCalls;
      Accumulate(Built, Expected, Wanted);

      const ClassAllocator Allocator = ProtocolFor(Value, &Policy);

      // The protocol declares exactly the steps its consumer supplied, and
      // those declarations are the whole cleanup rule.
      RC_ASSERT(Allocator.IsDeclared());
      RC_ASSERT(Allocator.DeclaresAllocation() ==
                (Value.Shape != ProtocolShape::NoAllocationStep));
      RC_ASSERT(Allocator.DeclaresConstruction() == Value.DeclaresConstruction);
      RC_ASSERT(Allocator.DeclaresDestruction() == Value.DeclaresDestruction);
      RC_ASSERT(Allocator.OwnsStorage() == Value.OwnsStorage);
      RC_ASSERT(Allocator.Storage().ByteCount == sizeof(Probe));
      RC_ASSERT(!ConstructionMilestoneText(Wanted.Reached).empty());

      // The first milestone: storage exists, or it does not.
      const StorageAllocationOutcome Allocated = Gate.Allocate(Allocator);
      RC_ASSERT(Allocated.ContainedException ==
                (Value.Shape != ProtocolShape::NoAllocationStep &&
                 Value.Allocation == StepOutcome::Throws));
      RC_ASSERT(Allocated.Succeeded() ==
                (Wanted.Reached != ConstructionMilestone::None));
      RC_ASSERT(Policy.AllocateCalls == Wanted.AllocateCalls);
      if (!Allocated.Succeeded()) {
        // An allocation that produced nothing is cleaned up by nothing at all.
        RC_ASSERT(Policy.ConstructCalls == 0);
        RC_ASSERT(Policy.DestroyCalls == 0);
        RC_ASSERT(Policy.DeallocateCalls == 0);
        continue;
      }
      const bool StorageIsTheProtocolsOwn = Allocated.Storage == Policy.Storage;
      RC_ASSERT(StorageIsTheProtocolsOwn);

      const auto Described = Owner.HeaderFor(Value.Ownership);
      RC_ASSERT(Described.has_value());
      UserdataHeader Header = *Described;
      RC_ASSERT(Header.HasCanonicalLayout());
      RC_ASSERT(Header.IdentifiesClass());

      StagedStorage Staged;
      Staged.Storage = Allocated.Storage;
      Staged.Identity.Address = Allocated.Storage;
      Staged.Identity.Nonce = ++NextNonce;
      Staged.Allocator = Allocator;

      const auto Staging = Gate.Stage(Header, Staged);
      RC_ASSERT(Staging.Succeeded == Wanted.Staged);
      if (!Staging.Succeeded) {
        RC_ASSERT(Staging.Failure == Wanted.Refusal);

        // Storage no record ever described is given straight back, and nothing
        // was constructed in it, so nothing is destroyed.
        Gate.DiscardStorage(Allocator, Allocated.Storage);
        RC_ASSERT(Policy.ConstructCalls == 0);
        RC_ASSERT(Policy.DestroyCalls == 0);
        RC_ASSERT(Policy.DeallocateCalls == Wanted.DeallocateCalls);
        continue;
      }

      // The staged value names the immutable protocol record its cleanup will
      // use, and nothing is constructed in its storage yet.
      RC_ASSERT(Header.Allocator.IsDeclared());
      RC_ASSERT(Header.Lifetime == LifetimeState::Allocated);
      RC_ASSERT(!Policy.ObjectIsLive);

      // The second milestone: one object is constructed in that storage, either
      // by the step the caller supplied or by the protocol's own.
      const ObjectConstruction Build = Value.SuppliesConstruction
                                           ? SuppliedConstruction(&Policy)
                                           : ObjectConstruction();
      const auto Constructed = Gate.Construct(Header, Build);
      RC_ASSERT(Constructed.Succeeded ==
                (Wanted.Reached != ConstructionMilestone::Allocated));
      RC_ASSERT(Policy.ConstructCalls == Wanted.ConstructCalls);
      if (!Constructed.Succeeded) {
        RC_ASSERT(Constructed.Failure == OwnershipFailure::RefusedConstruction);
        RC_ASSERT(Wanted.Refusal == OwnershipFailure::RefusedConstruction);

        // No object exists, so the storage is given back without destroying
        // anything, and the value is already fully released.
        RC_ASSERT(Policy.DestroyCalls == 0);
        RC_ASSERT(Policy.DeallocateCalls == Wanted.DeallocateCalls);
        RC_ASSERT(Header.Lifetime == LifetimeState::Released);
        RC_ASSERT(!Gate.Release(Header, ReleaseCause::ConstructionFailure));
        RC_ASSERT(Policy.DeallocateCalls == Wanted.DeallocateCalls);
        continue;
      }

      RC_ASSERT(Header.Lifetime == LifetimeState::Constructed);
      RC_ASSERT(Policy.ObjectIsLive);
      Record.WasConstructed = true;

      // The ownership statement of this value, exactly as its model requires it
      // or exactly as it does not.
      OwnershipRequest Request;
      if (Value.Statement != OwnershipStatement::MissingHandle &&
          (Value.Ownership == OwnershipModel::Borrowed ||
           Value.Statement == OwnershipStatement::UnexpectedHandle))
        Request.Handle = Handles[Index];
      if (Value.Ownership == OwnershipModel::Shared &&
          Value.Statement != OwnershipStatement::MissingShared) {
        // Exactly one shared ownership reference, naming exactly the object the
        // construction step built.
        SharedOwners[Index] =
            std::shared_ptr<void>(Allocated.Storage, [](void *) {});
        Request.SharedOwnership = SharedOwners[Index];
      }
      if (Value.Statement == OwnershipStatement::ExpiredHandle)
        Handles[Index].Invalidate();

      // Every consequence of one release, predicted by the model rather than
      // read back from Luna.
      const auto ApplyRelease = [&](bool UpdatesHeader) {
        return ModelRelease(Record, Expected, UpdatesHeader);
      };

      const bool Skipped =
          Value.Statement == OwnershipStatement::SkippedEstablishment;
      if (!Skipped) {
        const OwnershipFailure Refusal = EstablishmentRefusal(Value);
        const auto Established = Gate.Establish(Header, Request);
        RC_ASSERT(Established.Succeeded == (Refusal == OwnershipFailure::None));
        if (!Established.Succeeded) {
          RC_ASSERT(Established.Failure == Refusal);
          RC_ASSERT(Wanted.Refusal == Refusal);
          RC_ASSERT(Wanted.Reached == ConstructionMilestone::Constructed);

          // The third milestone was never completed, so cleanup destroys the
          // object, releases nothing ownership never took, and gives owned
          // storage back - in that order, once each.
          RC_ASSERT(Gate.Release(Header, ReleaseCause::PublicationFailure));
          RC_ASSERT(Policy.DestroyCalls == Wanted.DestroyCalls);
          RC_ASSERT(Policy.DeallocateCalls == Wanted.DeallocateCalls);
          RC_ASSERT(!Policy.DestroyedUnconstructed);
          RC_ASSERT(Header.Lifetime == LifetimeState::Released);
          RC_ASSERT(!Gate.Release(Header, ReleaseCause::PublicationFailure));
          RC_ASSERT(Policy.DestroyCalls == Wanted.DestroyCalls);
          RC_ASSERT(Policy.DeallocateCalls == Wanted.DeallocateCalls);
          continue;
        }

        // Ownership is established exactly once.
        RC_ASSERT(Gate.Establish(Header, Request).Failure ==
                  OwnershipFailure::OwnershipAlreadyEstablished);
      }

      if (Value.Statement == OwnershipStatement::ExpiredAfterEstablishment)
        Handles[Index].Invalidate();

      // The last milestone: the value becomes visible, and only then.
      const auto Published = Gate.Publish(Header);
      RC_ASSERT(Published.Succeeded == Wanted.Published);
      if (!Published.Succeeded) {
        RC_ASSERT(Published.Failure == Wanted.Refusal);
        RC_ASSERT(Gate.Release(Header, ReleaseCause::PublicationFailure));
        RC_ASSERT(Policy.DestroyCalls == Wanted.DestroyCalls);
        RC_ASSERT(Policy.DeallocateCalls == Wanted.DeallocateCalls);
        RC_ASSERT(!Policy.DestroyedUnconstructed);
        RC_ASSERT(Header.Lifetime == LifetimeState::Released);
        RC_ASSERT(!Gate.Release(Header, ReleaseCause::PublicationFailure));
        continue;
      }

      RC_ASSERT(Wanted.Reached == ConstructionMilestone::Published);
      RC_ASSERT(Header.Lifetime == LifetimeState::Published);
      Record.IsRecorded = true;
      Record.Lifetime = LifetimeState::Published;
      Record.HeaderLifetime = LifetimeState::Published;
      Record.HoldsSharedReference = Value.Ownership == OwnershipModel::Shared;

      // The object the value carries is the object the construction step built.
      RC_ASSERT(static_cast<Probe *>(Allocated.Storage)->Value ==
                Value.ConstructedValue);
      RC_ASSERT(Gate.LifetimePermitsAccess(Header) ==
                ModelPermitsAccess(Record));
      if (SharedOwners[Index]) {
        // One published shared value, one retained ownership reference.
        RC_ASSERT(SharedOwners[Index].use_count() ==
                  2 + (Request.SharedOwnership ? 1 : 0));
      }

      switch (Value.End) {
      case Ending::Nothing:
        break;

      case Ending::InvalidateHandle:
        // Ending the declared lifetime releases nothing at all; it only makes
        // every later access fail.
        Handles[Index].Invalidate();
        Record.HandleIsLive = false;
        break;

      case Ending::Release:
      case Ending::Collected:
        if (Value.End == Ending::Collected) {
          RC_ASSERT(Gate.ReleaseCollected(Header));
          RC_ASSERT(ApplyRelease(true));
          RC_ASSERT(!Gate.ReleaseCollected(Header));
        } else {
          RC_ASSERT(Gate.Release(Header, ReleaseCause::LifecycleAction));
          RC_ASSERT(ApplyRelease(true));
        }
        break;

      case Ending::ReleaseTwice: {
        RC_ASSERT(Gate.Release(Header, ReleaseCause::GarbageCollection));
        RC_ASSERT(ApplyRelease(true));
        const unsigned Destroyed = Policy.DestroyCalls;
        const unsigned Given = Policy.DeallocateCalls;

        // A second release performs no second destruction, no second shared
        // release, and no second deallocation.
        RC_ASSERT(!Gate.Release(Header, ReleaseCause::GarbageCollection));
        RC_ASSERT(Policy.DestroyCalls == Destroyed);
        RC_ASSERT(Policy.DeallocateCalls == Given);
        break;
      }

      case Ending::ReleaseByStorage:
        // The native object knows itself, not the block that carries it, so
        // this release never touches the header.
        RC_ASSERT(Gate.ReleaseByStorage(Allocated.Storage,
                                        ReleaseCause::ExplicitInvalidation));
        RC_ASSERT(ApplyRelease(false));
        RC_ASSERT(!Gate.ReleaseByStorage(Allocated.Storage,
                                         ReleaseCause::ExplicitInvalidation));
        break;
      }

      RC_ASSERT(Header.Lifetime == Record.HeaderLifetime);
      RC_ASSERT(Gate.LifetimePermitsAccess(Header) ==
                ModelPermitsAccess(Record));
      RC_ASSERT(Policy.DestroyCalls == Record.DestroySteps);
      RC_ASSERT(Policy.DeallocateCalls == Record.DeallocateSteps);
      RC_ASSERT(!Policy.DestroyedUnconstructed);

      // Luna retains exactly one shared ownership reference of a shared value
      // and releases exactly that one. The references the test itself still
      // holds - its own and the one in the statement it made - are counted out.
      if (SharedOwners[Index]) {
        const long Held = 1 + (Request.SharedOwnership ? 1 : 0);
        RC_ASSERT(SharedOwners[Index].use_count() ==
                  Held + (Record.HoldsSharedReference ? 1 : 0));
      }
    }

    // Exactly the values the model says exist, and exactly the ones it says are
    // visible.
    std::size_t ExpectedRecords = 0;
    std::size_t ExpectedPublished = 0;
    for (const ModelValue &Record : Model) {
      if (!Record.IsRecorded)
        continue;
      ++ExpectedRecords;
      if (Record.Lifetime == LifetimeState::Published)
        ++ExpectedPublished;
    }
    RemainingBeforeDestruction = ExpectedRecords;

    RC_ASSERT(Gate.RecordCount() == ExpectedRecords);
    RC_ASSERT(Gate.PublishedCount() == ExpectedPublished);
    RC_ASSERT(Hooks::OwnedUserdataCount(Owner.StateObject()) ==
              ExpectedRecords);
    RC_ASSERT(Hooks::PublishedUserdataCount(Owner.StateObject()) ==
              ExpectedPublished);

    // Every build and release counter, predicted from the generated milestones
    // alone.
    const auto BuiltCounts =
        Hooks::UserdataConstructionCounters(Owner.StateObject());
    RC_ASSERT(BuiltCounts.Allocate == Built.Allocate);
    RC_ASSERT(BuiltCounts.AllocationFailure == Built.AllocationFailure);
    RC_ASSERT(BuiltCounts.Construct == Built.Construct);
    RC_ASSERT(BuiltCounts.ConstructionFailure == Built.ConstructionFailure);
    RC_ASSERT(BuiltCounts.ContainedException == Built.ContainedException);

    const auto Counted = Hooks::UserdataReleaseCounters(Owner.StateObject());
    RC_ASSERT(Counted.Invalidate == Expected.Invalidate);
    RC_ASSERT(Counted.CacheRemoval == Expected.CacheRemoval);
    RC_ASSERT(Counted.Destroy == Expected.Destroy);
    RC_ASSERT(Counted.SharedRelease == Expected.SharedRelease);
    RC_ASSERT(Counted.Deallocate == Expected.Deallocate);
    RC_ASSERT(Counted.MetadataRelease == Expected.MetadataRelease);

    // Nothing a step threw escaped the gate, and no cleanup step ran without
    // the metadata it requires.
    RC_ASSERT(Counted.ContainedException == Expected.ContainedException);
    RC_ASSERT(Counted.IncompleteMetadata == 0);
    RC_ASSERT(Gate.RetainsCleanupMetadata());

    // The State keeps working after every generated refusal.
    RC_ASSERT(Owner.StateObject().Execute("Recovered = 5").IsSuccess());
    RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner.StateObject(), "Recovered") ==
              std::optional<int>(5));

    RC_TAG(MilestoneTag(Predicted));
  }

  // State destruction releases everything left, exactly once each, while its
  // cleanup metadata is still valid.
  for (ModelValue &Record : Model)
    static_cast<void>(ModelRelease(Record, Expected, false));
  for (std::size_t Index = 0; Index < Count; ++Index) {
    const StoragePolicy &Policy = *Policies[Index];
    RC_ASSERT(Policy.DestroyCalls == Model[Index].DestroySteps);
    RC_ASSERT(Policy.DeallocateCalls == Model[Index].DeallocateSteps);
    RC_ASSERT(!Policy.DestroyedUnconstructed);
  }

  const auto Destroyed = Hooks::ObserveLastStateDestruction();
  RC_ASSERT(Destroyed.Observed);
  RC_ASSERT(Destroyed.RefusedNewInvocations);
  RC_ASSERT(Destroyed.RetainedCleanupMetadata);
  RC_ASSERT(Destroyed.IncompleteMetadata == 0);

  // None of these values was ever a virtual-machine block, so the explicit
  // final sweep released every one of them.
  RC_ASSERT(Destroyed.ReleasedDuringClose == 0);
  RC_ASSERT(Destroyed.ReleasedAfterClose == RemainingBeforeDestruction);
  RC_ASSERT(Hooks::ObserveUserdataCollections().ContainedException == 0);

  for (std::size_t Index = 0; Index < Count; ++Index) {
    if (SharedOwners[Index])
      RC_ASSERT(SharedOwners[Index].use_count() == 1);

    // Storage a protocol declared no way to give back is still the test's.
    FreeUnreleasedStorage(*Policies[Index]);
  }
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// The write-path half: the same milestones taken through exactly the conversion
// write path a constructor or factory return takes, so what a refusal costs is
// observable from outside Luna.
// ---------------------------------------------------------------------------

// The deterministic refusal token the write path reports. A lifetime its owner
// ended or never declared is the one refusal a consumer causes; every other
// milestone failure is Luna's own internal accounting.
[[nodiscard]] std::string_view
ExpectedWriteFailure(const MilestonePrediction &Wanted) noexcept {
  if (Wanted.Published)
    return "none";
  if (Wanted.RefusedLifetime)
    return "expired_userdata";
  return "internal_failure";
}

[[nodiscard]] Luna::Detail::ClassValueWriteObservation
ConstructThroughWritePath(Luna::State &Host, const std::string &Path,
                          const GeneratedConstruction &Value,
                          const ClassAllocator &Allocator,
                          StoragePolicy *Policy,
                          const Luna::LifetimeHandle &Handle) {
  Luna::Detail::ClassValueConstructionRequest Request;
  Request.QualifiedName = "Probe";
  Request.Path = Path;
  Request.Ownership = Value.Ownership;
  Request.Access = ConstAccess::Mutable;
  Request.Allocator = Allocator;
  Request.Handle = Handle;
  if (Value.SuppliesConstruction)
    Request.Construct = SuppliedConstruction(Policy);
  return Hooks::ConstructClassValue(Host, Request);
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

void VerifyWritePathConstruction(ByteCursor &Cursor) {
  Hooks::ResetUserdataCollections();

  const std::size_t Count = 1 + Cursor.Pick(3);
  std::vector<GeneratedConstruction> Generated;
  std::vector<MilestonePrediction> Predicted;
  Generated.reserve(Count);
  Predicted.reserve(Count);
  for (std::size_t Index = 0; Index < Count; ++Index) {
    Generated.push_back(
        GenerateConstruction(Cursor, true, static_cast<int>(Index)));
    Predicted.push_back(Predict(Generated[Index], true));
  }
  const bool CollectsEverything = Cursor.Pick(2) == 0;

  std::vector<std::unique_ptr<StoragePolicy>> Policies(Count);
  for (std::size_t Index = 0; Index < Count; ++Index)
    Policies[Index] = PolicyFor(Generated[Index]);

  std::vector<Luna::LifetimeHandle> Handles(Count);
  std::vector<ModelValue> Model(Count);
  std::vector<std::string> Paths;
  Paths.reserve(Count);
  ModelBuildCounters Built;
  ModelReleaseCounters Expected;
  std::size_t RemainingBeforeDestruction = 0;

  {
    Fixture Owner;
    RC_ASSERT(Owner.IsUsable());
    Luna::State &Host = Owner.StateObject();
    RC_ASSERT(!Hooks::ClassMetatableIsCreated(Host, "Probe"));
    const std::optional<int> EntryRootDepth =
        Hooks::ObserveRootStackDepth(Host);
    RC_ASSERT(EntryRootDepth.has_value());

    const auto CountRecorded = [&Model]() {
      std::size_t Recorded = 0;
      for (const ModelValue &Record : Model) {
        if (Record.IsRecorded)
          ++Recorded;
      }
      return Recorded;
    };
    const auto CountPublished = [&Model]() {
      std::size_t Visible = 0;
      for (const ModelValue &Record : Model) {
        if (Record.IsRecorded && Record.Lifetime == LifetimeState::Published)
          ++Visible;
      }
      return Visible;
    };

    const auto VerifyCounts = [&]() {
      RC_ASSERT(Hooks::OwnedUserdataCount(Host) == CountRecorded());
      RC_ASSERT(Hooks::PublishedUserdataCount(Host) == CountPublished());
      RC_ASSERT(Hooks::CachedIdentityCount(Host) == CountPublished());
      RC_ASSERT(Hooks::LiveCachedIdentityCount(Host) == CountPublished());

      const auto BuiltCounts = Hooks::UserdataConstructionCounters(Host);
      RC_ASSERT(BuiltCounts.Allocate == Built.Allocate);
      RC_ASSERT(BuiltCounts.AllocationFailure == Built.AllocationFailure);
      RC_ASSERT(BuiltCounts.Construct == Built.Construct);
      RC_ASSERT(BuiltCounts.ConstructionFailure == Built.ConstructionFailure);
      RC_ASSERT(BuiltCounts.ContainedException == Built.ContainedException);

      const auto Counted = Hooks::UserdataReleaseCounters(Host);
      RC_ASSERT(Counted.Invalidate == Expected.Invalidate);
      RC_ASSERT(Counted.CacheRemoval == Expected.CacheRemoval);
      RC_ASSERT(Counted.Destroy == Expected.Destroy);
      RC_ASSERT(Counted.SharedRelease == Expected.SharedRelease);
      RC_ASSERT(Counted.Deallocate == Expected.Deallocate);
      RC_ASSERT(Counted.MetadataRelease == Expected.MetadataRelease);
      RC_ASSERT(Counted.ContainedException == Expected.ContainedException);
      RC_ASSERT(Counted.IncompleteMetadata == 0);
    };

    // One construction per generated value, each through exactly the write path
    // a returned object takes.
    for (std::size_t Index = 0; Index < Count; ++Index) {
      const GeneratedConstruction &Value = Generated[Index];
      const MilestonePrediction &Wanted = Predicted[Index];
      StoragePolicy &Policy = *Policies[Index];
      ModelValue &Record = Model[Index];
      Record.Ownership = Value.Ownership;
      Record.OwnsStorage = Value.OwnsStorage;
      Record.DeclaresDestruction = Value.DeclaresDestruction;
      Record.DestructionThrows = Value.DestructionThrows;
      Record.DestroySteps = Wanted.DestroyCalls;
      Record.DeallocateSteps = Wanted.DeallocateCalls;
      Accumulate(Built, Expected, Wanted);
      Paths.push_back("Slot" + std::to_string(Index));

      if (Value.Statement == OwnershipStatement::ExpiredHandle)
        Handles[Index].Invalidate();

      Luna::LifetimeHandle Declared = Luna::LifetimeHandle::Undeclared();
      if (Value.Ownership == OwnershipModel::Borrowed &&
          Value.Statement != OwnershipStatement::MissingHandle)
        Declared = Handles[Index];

      const ClassAllocator Allocator = ProtocolFor(Value, &Policy);
      const auto Written = ConstructThroughWritePath(
          Host, Paths[Index], Value, Allocator, &Policy, Declared);

      RC_ASSERT(Written.Failure == ExpectedWriteFailure(Wanted));
      RC_ASSERT(Written.Published == Wanted.Published);
      RC_ASSERT(Written.PublishedCount == (Wanted.Published ? 1 : 0));

      // Every attempt, refused or not, leaves the stack exactly as it found it.
      RC_ASSERT(Written.FinalStackDepth == Written.EntryStackDepth);

      // Exactly the steps the completed milestones warrant, and never a
      // destruction of storage nothing was constructed in.
      RC_ASSERT(Policy.AllocateCalls == Wanted.AllocateCalls);
      RC_ASSERT(Policy.ConstructCalls == Wanted.ConstructCalls);
      RC_ASSERT(Policy.DestroyCalls == Wanted.DestroyCalls);
      RC_ASSERT(Policy.DeallocateCalls == Wanted.DeallocateCalls);
      RC_ASSERT(!Policy.DestroyedUnconstructed);

      if (Wanted.Published) {
        Record.IsRecorded = true;
        Record.WasConstructed = true;
        Record.Lifetime = LifetimeState::Published;
        Record.HeaderLifetime = LifetimeState::Published;
        const bool StorageExists = Policy.Storage != nullptr;
        RC_ASSERT(StorageExists);
        RC_ASSERT(static_cast<Probe *>(Policy.Storage)->Value ==
                  Value.ConstructedValue);

        const auto Header = Hooks::ObserveClassUserdata(Host, Paths[Index]);
        RC_ASSERT(Header.has_value());
        RC_ASSERT(Header->Lifetime == LifetimeState::Published);
        RC_ASSERT(Header->Ownership == Value.Ownership);
        RC_ASSERT(Header->Allocator.IsDeclared());
        RC_ASSERT(Header->HasRequiredLifetimeHandle());
        const bool CarriesItsOwnObject =
            Header->Payload.Storage == Policy.Storage;
        RC_ASSERT(CarriesItsOwnObject);
      } else {
        // A refused construction publishes no partial value: nothing at the
        // path it named, no owner, and no identity-cache entry of its own.
        RC_ASSERT(!Hooks::ObserveClassUserdata(Host, Paths[Index]).has_value());
      }

      VerifyCounts();
    }

    // The class metatable is a consequence of publishing a value, never of
    // registering a class or of refusing a construction.
    const bool AnyPublished = CountPublished() != 0;
    RC_ASSERT(Hooks::ClassMetatableIsCreated(Host, "Probe") == AnyPublished);
    RC_ASSERT(Hooks::ClassMetatableCreationCount(Host, "Probe") ==
              (AnyPublished ? 1U : 0U));

    // Every published value reaches native code and delivers exactly its own
    // object; nothing else does.
    for (std::size_t Index = 0; Index < Count; ++Index) {
      const auto Read =
          ReadThroughAccessPath(Host, Paths[Index], Policies[Index]->Storage);
      const bool Permitted = ModelPermitsAccess(Model[Index]);
      RC_ASSERT(Read.ReachedNativeCode == Permitted);
      RC_ASSERT(Read.DeliveredExpectedObject == Permitted);
    }

    // One generated ending per published value.
    for (std::size_t Index = 0; Index < Count; ++Index) {
      const GeneratedConstruction &Value = Generated[Index];
      ModelValue &Record = Model[Index];
      StoragePolicy &Policy = *Policies[Index];
      if (Record.Lifetime != LifetimeState::Published)
        continue;

      switch (Value.End) {
      case Ending::Nothing:
      case Ending::Collected:
        break;

      case Ending::InvalidateHandle:
        if (Value.Ownership == OwnershipModel::Borrowed) {
          // Ending the declared lifetime releases nothing and destroys nothing;
          // it only stops every later access, before any native code runs.
          const ModelReleaseCounters Before = Expected;
          Handles[Index].Invalidate();
          Record.HandleIsLive = false;
          RC_ASSERT(Expected.MetadataRelease == Before.MetadataRelease);
        }
        break;

      case Ending::Release:
      case Ending::ReleaseTwice:
      case Ending::ReleaseByStorage: {
        void *const Storage = Policy.Storage;
        RC_ASSERT(Hooks::ReleaseClassValue(Host, Storage,
                                           ReleaseCause::LifecycleAction));

        // A release entered by the native object never touches the block that
        // carries it, but the cache entry goes before the payload, so the value
        // already refuses every access.
        RC_ASSERT(ModelRelease(Record, Expected, false));
        Record.HeaderLifetime = LifetimeState::Invalid;

        // The gate is idempotent: no second destruction, no second
        // deallocation.
        const unsigned Destroyed = Policy.DestroyCalls;
        RC_ASSERT(!Hooks::ReleaseClassValue(Host, Storage,
                                            ReleaseCause::LifecycleAction));
        RC_ASSERT(Policy.DestroyCalls == Destroyed);
        break;
      }
      }

      RC_ASSERT(Policy.DestroyCalls == Record.DestroySteps);
      RC_ASSERT(Policy.DeallocateCalls == Record.DeallocateSteps);
      RC_ASSERT(!Policy.DestroyedUnconstructed);

      const auto Read =
          ReadThroughAccessPath(Host, Paths[Index], Policy.Storage);
      RC_ASSERT(Read.ReachedNativeCode == ModelPermitsAccess(Record));
      if (!ModelPermitsAccess(Record))
        RC_ASSERT(Read.Failure == "expired_userdata");
    }

    VerifyCounts();

    // Everything the script has dropped is collected, and every value the
    // collector reaches ends through exactly the same gate.
    if (CollectsEverything) {
      for (const std::string &Path : Paths)
        RC_ASSERT(Host.Execute(Path + " = nil").IsSuccess());
      RC_ASSERT(Hooks::CollectGarbage(Host));
      RC_ASSERT(Hooks::CollectGarbage(Host));

      for (ModelValue &Record : Model)
        static_cast<void>(ModelRelease(Record, Expected, true));

      VerifyCounts();
      const auto Collections = Hooks::ObserveUserdataCollections();
      RC_ASSERT(Collections.ContainedException == 0);
      RC_ASSERT(Collections.ForeignBlock == 0);
      RC_ASSERT(Collections.UnroutedOrigin == 0);
      RC_ASSERT(Hooks::ClassMetatableCreationCount(Host, "Probe") <= 1);
    }

    for (std::size_t Index = 0; Index < Count; ++Index) {
      RC_ASSERT(Policies[Index]->DestroyCalls == Model[Index].DestroySteps);
      RC_ASSERT(Policies[Index]->DeallocateCalls ==
                Model[Index].DeallocateSteps);
    }
    RemainingBeforeDestruction = CountRecorded();

    // The stack is exactly where it started, and the State still constructs,
    // executes, and publishes after every generated refusal and release.
    RC_ASSERT(Hooks::ObserveRootStackDepth(Host) == EntryRootDepth);
    RC_ASSERT(Host.Execute("Recovered = 9").IsSuccess());
    RC_ASSERT(Hooks::ObserveIntegerGlobal(Host, "Recovered") ==
              std::optional<int>(9));

    StoragePolicy Complete;
    Complete.ConstructedValue = 77;
    GeneratedConstruction Working;
    Working.Ownership = OwnershipModel::LuaOwned;
    const ClassAllocator Reliable = ProtocolFor(Working, &Complete);
    const auto Again =
        ConstructThroughWritePath(Host, "Reused", Working, Reliable, &Complete,
                                  Luna::LifetimeHandle::Undeclared());
    RC_ASSERT(Again.Published);
    RC_ASSERT(Again.Failure == "none");
    RC_ASSERT(Complete.AllocateCalls == 1);
    RC_ASSERT(Complete.ConstructCalls == 1);
    RC_ASSERT(Hooks::ReleaseClassValue(Host, Complete.Storage,
                                       ReleaseCause::LifecycleAction));
    RC_ASSERT(Complete.DestroyCalls == 1);
    RC_ASSERT(Complete.DeallocateCalls == 1);
    RC_ASSERT(!Complete.DestroyedUnconstructed);

    if (!AnyPublished)
      RC_TAG("write path: every construction refused");
    else if (CollectsEverything)
      RC_TAG("write path: constructed then collected");
    else if (RemainingBeforeDestruction == 0)
      RC_TAG("write path: constructed then released");
    else
      RC_TAG("write path: values left for State destruction");
  }

  // State destruction releases everything left, exactly once each.
  for (ModelValue &Record : Model)
    static_cast<void>(ModelRelease(Record, Expected, false));
  for (std::size_t Index = 0; Index < Count; ++Index) {
    const StoragePolicy &Policy = *Policies[Index];
    RC_ASSERT(Policy.DestroyCalls == Model[Index].DestroySteps);
    RC_ASSERT(Policy.DeallocateCalls == Model[Index].DeallocateSteps);
    RC_ASSERT(!Policy.DestroyedUnconstructed);
  }

  const auto Destroyed = Hooks::ObserveLastStateDestruction();
  RC_ASSERT(Destroyed.Observed);
  RC_ASSERT(Destroyed.RefusedNewInvocations);
  RC_ASSERT(Destroyed.RetainedCleanupMetadata);
  RC_ASSERT(Destroyed.IncompleteMetadata == 0);
  RC_ASSERT(Destroyed.ReleasedDuringClose + Destroyed.ReleasedAfterClose ==
            RemainingBeforeDestruction);
  RC_ASSERT(Hooks::ObserveUserdataCollections().ContainedException == 0);

  for (std::size_t Index = 0; Index < Count; ++Index)
    FreeUnreleasedStorage(*Policies[Index]);
}

} // namespace

int RunAllocatorConstructionCleanupProperties() {
  // clang-format off
  // **Validates: Requirements 12.3, 12.4, 12.5, 12.6, 12.7, 12.8**
  // Feature: reflection-driven-binding-system, Property 27: Construction and allocator cleanup follow completed milestones exactly once
  const bool Passed = rc::check(
      // clang-format on
      "Construction and allocator cleanup follow completed milestones exactly "
      "once",
      [](const std::vector<std::uint8_t> &MilestoneBytes,
         const std::vector<std::uint8_t> &WriteBytes) {
        ByteCursor Milestones(MilestoneBytes);
        VerifyMilestoneWalk(Milestones);

        ByteCursor Writes(WriteBytes);
        VerifyWritePathConstruction(Writes);
      });
  return Passed ? 0 : 1;
}
