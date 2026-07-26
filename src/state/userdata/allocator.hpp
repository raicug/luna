#pragma once

// Luna's side of the semantic allocator protocol: running one declared step,
// exactly once, without ever letting it escape.
//
// The protocol itself is public and Luau-free - allocate suitably aligned
// storage, construct one object in it, destroy an object known to be
// constructed, deallocate the storage - and the consumer supplies whichever of
// those steps it owns. What is private is when each step runs and what happens
// when one of them fails: a garbage collector and a State destructor both reach
// these steps, so nothing a consumer's operation throws may cross back out.
//
// Every function here answers the same two questions about one step: did it
// perform its work, and did Luna have to contain something. The release gate
// counts exactly those answers, which is why the accounting of a value can be
// predicted from the milestones it reached rather than from what its operations
// happened to do.
//
// The milestones are the whole cleanup rule. Storage exists or it does not; an
// object was constructed in it or it was not; ownership was established or it
// was not. Allocation failure needs no cleanup at all, construction failure
// gives the storage back without destroying anything, and a failure after
// construction destroys before it deallocates. Nothing here decides ownership
// or publication: the release gate owns those, and it uses exactly these steps.

// clang-format off
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/class_builder.hpp>

#include <cstdint>
#include <functional>
#include <string_view>
// clang-format on

namespace Luna::Detail {

// How far one object got before something stopped it. Each milestone is
// completed work, so the cleanup it warrants is decided by the milestone alone.
enum class ConstructionMilestone : std::uint8_t {
  // Nothing was done: an allocation that never produced storage needs no
  // cleanup call whatsoever.
  None,

  // Storage exists and nothing is constructed in it. Cleanup deallocates it and
  // destroys nothing.
  Allocated,

  // One object is constructed in the storage. Cleanup destroys it before
  // deallocating.
  Constructed,

  // Ownership of the constructed object was established, so cleanup also
  // releases exactly what that ownership took.
  Owned,

  // The value is published and visible. From here only the ordinary release
  // gate ends it.
  Published
};

[[nodiscard]] std::string_view
ConstructionMilestoneText(ConstructionMilestone Reached) noexcept;

// The one construction step of one staged object, supplied by whoever knows how
// to build it: a constructor candidate, a factory, or the allocator protocol
// itself. All the gate needs to know is whether an object now exists in the
// storage it staged, so that is all this step reports; a step that throws says
// the same thing and is contained. One of a protocol's own construction
// operations bridges to it through its reported result.
using ObjectConstruction = std::function<bool(void *Storage)>;

// What one semantic step did. `Performed` is what the release accounting
// counts; `ContainedException` is what Luna caught instead of letting it out.
struct AllocatorStepOutcome final {
  bool Performed = false;
  bool ContainedException = false;
};

// What one semantic allocation produced.
struct StorageAllocationOutcome final {
  void *Storage = nullptr;
  bool ContainedException = false;

  [[nodiscard]] bool Succeeded() const noexcept { return Storage != nullptr; }
};

// The storage one registered class needs, taken from the declared C++ shape its
// consumer's translation unit captured. The backend never sees the class type,
// so this is the only description of its storage it has.
[[nodiscard]] StorageRequest
StorageRequestFor(const ClassPolicy &Policy) noexcept;

// The protocol Luna attaches to a value it neither created nor releases. It
// declares no step at all, so every cleanup decision made from it is "do
// nothing", and it is one shared immutable record rather than one per value.
[[nodiscard]] const ClassAllocator &BorrowedStorageProtocol();

// The immutable record behind one protocol. It is what a userdata header names,
// and it stays valid until the last value that depends on it completes cleanup.
[[nodiscard]] const void *
AllocatorRecordIdentity(const ClassAllocator &Allocator) noexcept;

// The four semantic steps, each contained. A step the protocol never declared
// is never called and reports that it performed nothing.
[[nodiscard]] StorageAllocationOutcome
AllocateObjectStorage(const ClassAllocator &Allocator) noexcept;
[[nodiscard]] AllocatorStepOutcome
ConstructObject(const ClassAllocator &Allocator, void *Storage,
                const ObjectConstruction &Build) noexcept;
[[nodiscard]] AllocatorStepOutcome
DestroyKnownConstructedObject(const ClassAllocator &Allocator,
                              void *Storage) noexcept;
[[nodiscard]] AllocatorStepOutcome
DeallocateObjectStorage(const ClassAllocator &Allocator,
                        void *Storage) noexcept;

} // namespace Luna::Detail
