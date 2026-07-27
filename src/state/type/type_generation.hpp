#pragma once

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

enum class TypeDeclarationStatus {
  Acceptable,
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
  [[nodiscard]] static std::shared_ptr<const TypeGeneration> Foundation();

  [[nodiscard]] static std::shared_ptr<const TypeGeneration>
  Build(std::vector<TypeRecord> Records, TypeDeclarationStatus &Status);

  [[nodiscard]] static std::shared_ptr<const TypeGeneration>
  Derive(const TypeGeneration &Current, std::vector<TypeRecord> Added,
         TypeDeclarationStatus &Status);

  [[nodiscard]] static std::shared_ptr<const TypeGeneration>
  Retain(const TypeGeneration &Current, std::vector<TypeRecord> Retained,
         TypeDeclarationStatus &Status);

  [[nodiscard]] std::uint64_t Generation() const noexcept {
    return GenerationValue;
  }

  [[nodiscard]] std::size_t Size() const noexcept { return Records.size(); }
  [[nodiscard]] bool IsEmpty() const noexcept { return Records.empty(); }

  [[nodiscard]] const TypeRecord *At(std::size_t Index) const noexcept;

  [[nodiscard]] std::span<const TypeRecord> All() const noexcept {
    return Records;
  }

  [[nodiscard]] const TypeRecord *Find(const TypeId &Identity) const noexcept;
  [[nodiscard]] const TypeRecord *
  Find(const TypeDescriptor &Descriptor) const noexcept;

  [[nodiscard]] const TypeRecord *Find(ValueKind Kind) const noexcept;

  [[nodiscard]] bool Contains(const TypeDescriptor &Descriptor) const noexcept;

  [[nodiscard]] bool IsAvailable(const TypeDescriptor &Descriptor,
                                 bool AllowVoid) const noexcept;
  [[nodiscard]] bool
  IsAvailableForRead(const TypeDescriptor &Descriptor) const noexcept;
  [[nodiscard]] bool
  IsAvailableForWrite(const TypeDescriptor &Descriptor) const noexcept;

  [[nodiscard]] std::string_view
  PublicNameOf(const TypeDescriptor &Descriptor) const noexcept;
  [[nodiscard]] std::string_view PublicNameOf(ValueKind Kind) const noexcept;

private:
  TypeGeneration() = default;

  std::uint64_t GenerationValue = 0;
  std::vector<TypeRecord> Records;
};

[[nodiscard]] TypeDeclarationStatus
ClassifyTypeDeclaration(const TypeGeneration &Current,
                        std::span<const TypeRecord> Pending,
                        const TypeRecord &Candidate);

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
