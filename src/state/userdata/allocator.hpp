#pragma once

// clang-format off
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/class_builder.hpp>

#include <cstdint>
#include <functional>
#include <string_view>
// clang-format on

namespace Luna::Detail {

enum class ConstructionMilestone : std::uint8_t {
  None,

  Allocated,

  Constructed,

  Owned,

  Published
};

[[nodiscard]] std::string_view
ConstructionMilestoneText(ConstructionMilestone Reached) noexcept;

using ObjectConstruction = std::function<bool(void *Storage)>;

struct AllocatorStepOutcome final {
  bool Performed = false;
  bool ContainedException = false;
};

struct StorageAllocationOutcome final {
  void *Storage = nullptr;
  bool ContainedException = false;

  [[nodiscard]] bool Succeeded() const noexcept { return Storage != nullptr; }
};

[[nodiscard]] StorageRequest
StorageRequestFor(const ClassPolicy &Policy) noexcept;

[[nodiscard]] const ClassAllocator &BorrowedStorageProtocol();

[[nodiscard]] const void *
AllocatorRecordIdentity(const ClassAllocator &Allocator) noexcept;

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
