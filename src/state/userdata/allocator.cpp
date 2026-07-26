// clang-format off
#include "state/userdata/allocator.hpp"

#include <luna/binding/class_allocator.hpp>
#include <luna/binding/class_builder.hpp>

#include <string_view>
#include <utility>
// clang-format on

namespace Luna::Detail {

std::string_view
ConstructionMilestoneText(ConstructionMilestone Reached) noexcept {
  switch (Reached) {
  case ConstructionMilestone::None:
    return "none";
  case ConstructionMilestone::Allocated:
    return "allocated";
  case ConstructionMilestone::Constructed:
    return "constructed";
  case ConstructionMilestone::Owned:
    return "owned";
  case ConstructionMilestone::Published:
    return "published";
  }
  return "unknown";
}

StorageRequest StorageRequestFor(const ClassPolicy &Policy) noexcept {
  StorageRequest Requested;
  Requested.ByteCount = Policy.ByteCount;
  Requested.Alignment = Policy.Alignment;
  return Requested;
}

const ClassAllocator &BorrowedStorageProtocol() {
  // One shared immutable record for every borrowed value in the process. It
  // declares no step, so it can never destroy or deallocate anything, and
  // sharing it keeps a borrowed exposure free of any allocation of its own.
  static const ClassAllocator Protocol = ClassAllocator::FromOperations(
      "Luna.BorrowedStorage", StorageRequest(),
      ClassAllocator::AllocateOperation(), ClassAllocator::ConstructOperation(),
      ClassAllocator::DestroyOperation(),
      ClassAllocator::DeallocateOperation());
  return Protocol;
}

const void *AllocatorRecordIdentity(const ClassAllocator &Allocator) noexcept {
  return static_cast<const void *>(ClassAllocatorAccess::Observe(Allocator));
}

StorageAllocationOutcome
AllocateObjectStorage(const ClassAllocator &Allocator) noexcept {
  StorageAllocationOutcome Outcome;
  const AllocatorRecord *Held = ClassAllocatorAccess::Observe(Allocator);
  if (Held == nullptr || !Held->DeclaresAllocation())
    return Outcome;

  // An allocation that throws produced nothing, so nothing is cleaned up after
  // it: this is the one failure the protocol answers with no cleanup call.
  try {
    Outcome.Storage = Held->Allocate();
  } catch (...) {
    Outcome.Storage = nullptr;
    Outcome.ContainedException = true;
  }
  return Outcome;
}

AllocatorStepOutcome ConstructObject(const ClassAllocator &Allocator,
                                     void *Storage,
                                     const ObjectConstruction &Build) noexcept {
  AllocatorStepOutcome Outcome;
  if (Storage == nullptr)
    return Outcome;

  // A caller that knows how to build the object owns that step; otherwise the
  // protocol's own construction step is the only one there is.
  if (Build) {
    try {
      Outcome.Performed = Build(Storage);
    } catch (...) {
      Outcome.Performed = false;
      Outcome.ContainedException = true;
    }
    return Outcome;
  }

  const AllocatorRecord *Held = ClassAllocatorAccess::Observe(Allocator);
  if (Held == nullptr || !Held->DeclaresConstruction())
    return Outcome;

  try {
    Outcome.Performed = Held->Construct(Storage).Performed;
  } catch (...) {
    Outcome.Performed = false;
    Outcome.ContainedException = true;
  }
  return Outcome;
}

AllocatorStepOutcome
DestroyKnownConstructedObject(const ClassAllocator &Allocator,
                              void *Storage) noexcept {
  AllocatorStepOutcome Outcome;
  const AllocatorRecord *Held = ClassAllocatorAccess::Observe(Allocator);
  if (Held == nullptr || !Held->DeclaresDestruction() || Storage == nullptr)
    return Outcome;

  // A destruction that throws still counts as the one destruction this object
  // gets: the object is gone either way, and every remaining step still runs.
  try {
    Outcome.Performed = Held->Destroy(Storage).Performed;
  } catch (...) {
    Outcome.Performed = true;
    Outcome.ContainedException = true;
  }
  return Outcome;
}

AllocatorStepOutcome DeallocateObjectStorage(const ClassAllocator &Allocator,
                                             void *Storage) noexcept {
  AllocatorStepOutcome Outcome;
  const AllocatorRecord *Held = ClassAllocatorAccess::Observe(Allocator);
  if (Held == nullptr || !Held->DeclaresDeallocation() || Storage == nullptr)
    return Outcome;

  try {
    Outcome.Performed = Held->Deallocate(Storage).Performed;
  } catch (...) {
    Outcome.Performed = true;
    Outcome.ContainedException = true;
  }
  return Outcome;
}

} // namespace Luna::Detail
