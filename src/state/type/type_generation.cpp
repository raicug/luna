// clang-format off
#include "state/type/type_generation.hpp"

#include <luna/binding/value.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/type/foundation_types.hpp"
#include "state/type/type_record.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

// Classification of one candidate against one already accepted declaration.
[[nodiscard]] TypeDeclarationStatus Compare(const TypeRecord &Existing,
                                            const TypeRecord &Candidate) {
  if (Existing.Descriptor == Candidate.Descriptor) {
    // One canonical descriptor has exactly one identity. A second identity for
    // the same descriptor contradicts the canonical model.
    if (Existing.Identity != Candidate.Identity)
      return TypeDeclarationStatus::DescriptorCollision;
    if (!HasSameConverters(Existing, Candidate))
      return TypeDeclarationStatus::ConflictingConverter;
    if (!HasSameDeclaration(Existing, Candidate))
      return TypeDeclarationStatus::IncompatibleDuplicate;
    return TypeDeclarationStatus::IdempotentDuplicate;
  }

  // An identity match with an unequal descriptor is a collision, never an
  // order-dependent reassignment.
  if (Existing.Identity == Candidate.Identity)
    return TypeDeclarationStatus::DescriptorCollision;

  return TypeDeclarationStatus::Acceptable;
}

[[nodiscard]] bool KnowsIdentity(const TypeGeneration &Current,
                                 std::span<const TypeRecord> Pending,
                                 const TypeId &Identity) {
  if (Current.Find(Identity))
    return true;
  return std::any_of(Pending.begin(), Pending.end(),
                     [&Identity](const TypeRecord &Record) {
                       return Record.Identity == Identity;
                     });
}

} // namespace

std::string_view
TypeDeclarationStatusText(TypeDeclarationStatus Status) noexcept {
  switch (Status) {
  case TypeDeclarationStatus::Acceptable:
    return "acceptable";
  case TypeDeclarationStatus::IdempotentDuplicate:
    return "idempotent_duplicate";
  case TypeDeclarationStatus::IncompleteRecord:
    return "incomplete_type_record";
  case TypeDeclarationStatus::ConflictingConverter:
    return "conflicting_converter";
  case TypeDeclarationStatus::IncompatibleDuplicate:
    return "incompatible_duplicate_declaration";
  case TypeDeclarationStatus::UnavailableNestedType:
    return "unavailable_nested_type";
  case TypeDeclarationStatus::DescriptorCollision:
    return "descriptor_collision";
  }
  return "unknown";
}

TypeDeclarationStatus
ClassifyTypeDeclaration(const TypeGeneration &Current,
                        std::span<const TypeRecord> Pending,
                        const TypeRecord &Candidate) {
  if (!Candidate.IsComplete())
    return TypeDeclarationStatus::IncompleteRecord;

  for (const TypeRecord &Existing : Current.All()) {
    const TypeDeclarationStatus Status = Compare(Existing, Candidate);
    if (Status != TypeDeclarationStatus::Acceptable)
      return Status;
  }
  for (const TypeRecord &Existing : Pending) {
    const TypeDeclarationStatus Status = Compare(Existing, Candidate);
    if (Status != TypeDeclarationStatus::Acceptable)
      return Status;
  }

  // Every nested type a declaration is built from must already be available:
  // a converter can never recurse into a type the registry does not describe.
  for (const TypeId &Nested : Candidate.NestedTypes) {
    if (!KnowsIdentity(Current, Pending, Nested))
      return TypeDeclarationStatus::UnavailableNestedType;
  }

  return TypeDeclarationStatus::Acceptable;
}

std::shared_ptr<const TypeGeneration> TypeGeneration::Foundation() {
  static const std::shared_ptr<const TypeGeneration> Shared = [] {
    TypeDeclarationStatus Status = TypeDeclarationStatus::Acceptable;
    return Build(FoundationTypeRecords(), Status);
  }();
  return Shared;
}

std::shared_ptr<const TypeGeneration>
TypeGeneration::Build(std::vector<TypeRecord> Records,
                      TypeDeclarationStatus &Status) {
  Status = TypeDeclarationStatus::Acceptable;

  std::vector<TypeRecord> Accepted;
  Accepted.reserve(Records.size());

  static const TypeGeneration EmptyGeneration;
  for (TypeRecord &Candidate : Records) {
    const TypeDeclarationStatus Classified =
        ClassifyTypeDeclaration(EmptyGeneration, Accepted, Candidate);
    if (!TypeDeclarationIsAccepted(Classified)) {
      Status = Classified;
      return nullptr;
    }
    if (Classified == TypeDeclarationStatus::IdempotentDuplicate)
      continue;
    Accepted.push_back(std::move(Candidate));
  }

  // Canonical order, never declaration order: an equivalent declaration set
  // always produces one identical generation.
  std::sort(Accepted.begin(), Accepted.end(), TypeRecordPrecedes);

  std::shared_ptr<TypeGeneration> Built(new TypeGeneration());
  Built->GenerationValue = 0;
  Built->Records = std::move(Accepted);
  return Built;
}

std::shared_ptr<const TypeGeneration>
TypeGeneration::Derive(const TypeGeneration &Current,
                       std::vector<TypeRecord> Added,
                       TypeDeclarationStatus &Status) {
  Status = TypeDeclarationStatus::Acceptable;

  std::vector<TypeRecord> Accepted;
  Accepted.reserve(Added.size());
  for (TypeRecord &Candidate : Added) {
    const TypeDeclarationStatus Classified =
        ClassifyTypeDeclaration(Current, Accepted, Candidate);
    if (!TypeDeclarationIsAccepted(Classified)) {
      Status = Classified;
      return nullptr;
    }
    if (Classified == TypeDeclarationStatus::IdempotentDuplicate)
      continue;
    Accepted.push_back(std::move(Candidate));
  }

  std::vector<TypeRecord> Records;
  Records.reserve(Current.Records.size() + Accepted.size());
  Records.insert(Records.end(), Current.Records.begin(), Current.Records.end());
  for (TypeRecord &Record : Accepted)
    Records.push_back(std::move(Record));
  std::sort(Records.begin(), Records.end(), TypeRecordPrecedes);

  std::shared_ptr<TypeGeneration> Next(new TypeGeneration());
  Next->GenerationValue = Current.GenerationValue + 1;
  Next->Records = std::move(Records);
  return Next;
}

const TypeRecord *TypeGeneration::At(std::size_t Index) const noexcept {
  return Index < Records.size() ? &Records[Index] : nullptr;
}

const TypeRecord *TypeGeneration::Find(const TypeId &Identity) const noexcept {
  for (const TypeRecord &Record : Records) {
    if (Record.Identity == Identity)
      return &Record;
  }
  return nullptr;
}

const TypeRecord *
TypeGeneration::Find(const TypeDescriptor &Descriptor) const noexcept {
  for (const TypeRecord &Record : Records) {
    if (Record.Descriptor == Descriptor)
      return &Record;
  }
  return nullptr;
}

const TypeRecord *TypeGeneration::Find(ValueKind Kind) const noexcept {
  const TypeDescriptor Descriptor = CanonicalValueType(Kind);
  if (!Descriptor.IsValid())
    return nullptr;
  const TypeRecord *Record = Find(Descriptor);
  if (!Record || Record->ValueRepresentation != Kind)
    return nullptr;
  return Record;
}

bool TypeGeneration::Contains(const TypeDescriptor &Descriptor) const noexcept {
  return Find(Descriptor) != nullptr;
}

bool TypeGeneration::IsAvailable(const TypeDescriptor &Descriptor,
                                 bool AllowVoid) const noexcept {
  const TypeRecord *Record = Find(Descriptor);
  if (!Record)
    return false;
  if (Record->IsVoid())
    return AllowVoid;
  return Record->IsReadable || Record->IsWritable;
}

bool TypeGeneration::IsAvailableForRead(
    const TypeDescriptor &Descriptor) const noexcept {
  const TypeRecord *Record = Find(Descriptor);
  return Record && !Record->IsVoid() && Record->IsReadable &&
         (Record->Read || Record->StructuredRead);
}

bool TypeGeneration::IsAvailableForWrite(
    const TypeDescriptor &Descriptor) const noexcept {
  const TypeRecord *Record = Find(Descriptor);
  return Record && !Record->IsVoid() && Record->IsWritable &&
         (Record->Write || Record->StructuredWrite);
}

std::string_view
TypeGeneration::PublicNameOf(const TypeDescriptor &Descriptor) const noexcept {
  const TypeRecord *Record = Find(Descriptor);
  return Record ? std::string_view(Record->PublicName) : std::string_view();
}

std::string_view TypeGeneration::PublicNameOf(ValueKind Kind) const noexcept {
  const TypeRecord *Record = Find(Kind);
  return Record ? std::string_view(Record->PublicName) : std::string_view();
}

} // namespace Luna::Detail
