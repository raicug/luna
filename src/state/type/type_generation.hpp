#pragma once

// One immutable type generation. A generation is the canonical type half of the
// committed model: an ordered, immutable set of `TypeRecord` values keyed by
// `TypeId`. It is built once, never mutated, and holds no virtual-machine
// resource, so publication can swap it atomically, one invocation can capture
// it at entry and keep it stable for the whole call, and a reader can retain it
// after the State that published it is gone.
//
// Every declaration is classified before it enters a generation. A conflicting
// converter, an incompatible duplicate declaration, an unavailable nested type,
// and a canonical-descriptor collision are all rejected deterministically,
// which is what lets registration reject them transactionally instead of
// publishing a contradictory generation.

// clang-format off
#include <luna/binding/value.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/type/type_record.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

// Deterministic classification of one candidate type declaration.
enum class TypeDeclarationStatus {
  // The declaration adds one new canonical type.
  Acceptable,
  // The declaration repeats an existing one exactly, so it adds nothing.
  IdempotentDuplicate,
  IncompleteRecord,
  ConflictingConverter,
  IncompatibleDuplicate,
  UnavailableNestedType,
  DescriptorCollision
};

[[nodiscard]] std::string_view
TypeDeclarationStatusText(TypeDeclarationStatus Status) noexcept;

[[nodiscard]] constexpr bool
TypeDeclarationIsAccepted(TypeDeclarationStatus Status) noexcept {
  return Status == TypeDeclarationStatus::Acceptable ||
         Status == TypeDeclarationStatus::IdempotentDuplicate;
}

class TypeGeneration final {
public:
  // The migrated foundation generation: `void`, `bool`, signed 32-bit `int`,
  // `double`, and `std::string`. Every State starts by observing it.
  [[nodiscard]] static std::shared_ptr<const TypeGeneration> Foundation();

  // Builds one generation from scratch. On rejection the returned pointer is
  // null and `Status` names the first deterministic reason.
  [[nodiscard]] static std::shared_ptr<const TypeGeneration>
  Build(std::vector<TypeRecord> Records, TypeDeclarationStatus &Status);

  // The successor of `Current` extended by `Added`. An exact repeat of an
  // existing declaration is accepted and adds nothing; every other conflict is
  // rejected and leaves `Current` untouched.
  [[nodiscard]] static std::shared_ptr<const TypeGeneration>
  Derive(const TypeGeneration &Current, std::vector<TypeRecord> Added,
         TypeDeclarationStatus &Status);

  [[nodiscard]] std::uint64_t Generation() const noexcept {
    return GenerationValue;
  }

  [[nodiscard]] std::size_t Size() const noexcept { return Records.size(); }
  [[nodiscard]] bool IsEmpty() const noexcept { return Records.empty(); }

  // Canonical position access: index 0 is the canonically first type.
  [[nodiscard]] const TypeRecord *At(std::size_t Index) const noexcept;

  [[nodiscard]] std::span<const TypeRecord> All() const noexcept {
    return Records;
  }

  [[nodiscard]] const TypeRecord *Find(const TypeId &Identity) const noexcept;
  [[nodiscard]] const TypeRecord *
  Find(const TypeDescriptor &Descriptor) const noexcept;

  // The record of one foundation value kind, or null when the generation does
  // not describe it.
  [[nodiscard]] const TypeRecord *Find(ValueKind Kind) const noexcept;

  [[nodiscard]] bool Contains(const TypeDescriptor &Descriptor) const noexcept;

  // Availability of one canonical type for conversion in this generation.
  [[nodiscard]] bool IsAvailable(const TypeDescriptor &Descriptor,
                                 bool AllowVoid) const noexcept;
  [[nodiscard]] bool
  IsAvailableForRead(const TypeDescriptor &Descriptor) const noexcept;
  [[nodiscard]] bool
  IsAvailableForWrite(const TypeDescriptor &Descriptor) const noexcept;

  // Public name of one canonical type, or an empty view when the generation
  // does not describe it.
  [[nodiscard]] std::string_view
  PublicNameOf(const TypeDescriptor &Descriptor) const noexcept;
  [[nodiscard]] std::string_view PublicNameOf(ValueKind Kind) const noexcept;

private:
  TypeGeneration() = default;

  std::uint64_t GenerationValue = 0;
  std::vector<TypeRecord> Records;
};

// Classifies one candidate declaration against a committed generation plus the
// declarations already pending in the same transaction. Validation and
// preparation both read this classification, so they can never disagree.
[[nodiscard]] TypeDeclarationStatus
ClassifyTypeDeclaration(const TypeGeneration &Current,
                        std::span<const TypeRecord> Pending,
                        const TypeRecord &Candidate);

// The type generation one invocation captures at entry. The source is owned by
// the State, publication replaces the whole generation in one step, and an
// invocation copies the shared pointer once so its viability, conversion, and
// diagnostics stay stable for the entire call even if a later registration
// publishes a new generation meanwhile.
class TypeGenerationSource final {
public:
  TypeGenerationSource() : Types(TypeGeneration::Foundation()) {}

  [[nodiscard]] std::shared_ptr<const TypeGeneration> Capture() const {
    return Types ? Types : TypeGeneration::Foundation();
  }

  void Publish(std::shared_ptr<const TypeGeneration> Published) {
    if (Published)
      Types = std::move(Published);
  }

private:
  std::shared_ptr<const TypeGeneration> Types;
};

} // namespace Luna::Detail
