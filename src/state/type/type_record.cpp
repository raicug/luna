// clang-format off
#include "state/type/type_record.hpp"

#include <luna/binding/value.hpp>
#include <luna/type/type_descriptor.hpp>

#include <algorithm>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
// clang-format on

namespace Luna::Detail {

bool EnumerationDomain::Accepts(std::int64_t Candidate) const noexcept {
  if (IsBitflags) {
    // Every unsupported bit rejects the whole value: the mask is never applied
    // to the candidate, so nothing is truncated on the way in or out.
    return (Candidate & ~SupportedBits) == 0;
  }
  if (Values.empty())
    return true;
  return std::binary_search(Values.begin(), Values.end(), Candidate);
}

std::string_view
LuauRepresentationText(LuauRepresentation Representation) noexcept {
  switch (Representation) {
  case LuauRepresentation::None:
    return "none";
  case LuauRepresentation::Nil:
    return "nil";
  case LuauRepresentation::Boolean:
    return "boolean";
  case LuauRepresentation::Number:
    return "number";
  case LuauRepresentation::String:
    return "string";
  case LuauRepresentation::Table:
    return "table";
  case LuauRepresentation::Userdata:
    return "userdata";
  case LuauRepresentation::Function:
    return "function";
  }
  return "none";
}

std::string_view
ConversionRankCategoryText(ConversionRankCategory Rank) noexcept {
  switch (Rank) {
  case ConversionRankCategory::Exact:
    return "exact";
  case ConversionRankCategory::SafeBuiltIn:
    return "safe_builtin";
  case ConversionRankCategory::User:
    return "user";
  }
  return "exact";
}

bool TypeRecord::IsStructural() const noexcept {
  return StructuredRead != nullptr || StructuredWrite != nullptr;
}

bool TypeRecord::IsVoid() const noexcept {
  const auto Fixed = Descriptor.FixedKey();
  return Fixed && *Fixed == FixedTypeKey::Void;
}

bool TypeRecord::IsComplete() const {
  if (!Identity.IsValid() || !Descriptor.IsValid() || PublicName.empty())
    return false;

  // `void` is the one type that carries no value: it is neither readable nor
  // writable and has no representation, no converter, and no value mapping.
  if (IsVoid())
    return !IsReadable && !IsWritable && !ValueRepresentation &&
           Representation == LuauRepresentation::None && !Read && !Write &&
           !StructuredRead && !StructuredWrite;

  if (!IsReadable && !IsWritable)
    return false;

  // One type converts either as a scalar or as a structural aggregate. Mixing
  // the two pairs would leave the direction a converter runs in ambiguous.
  if ((Read || Write) && IsStructural())
    return false;
  if (IsReadable && !Read && !StructuredRead)
    return false;
  if (IsWritable && !Write && !StructuredWrite)
    return false;
  if (Representation == LuauRepresentation::None)
    return false;

  for (const TypeId &Nested : NestedTypes) {
    if (!Nested.IsValid())
      return false;
  }
  return true;
}

bool HasSameConverters(const TypeRecord &Left,
                       const TypeRecord &Right) noexcept {
  return Left.Read == Right.Read && Left.Write == Right.Write &&
         Left.StructuredRead == Right.StructuredRead &&
         Left.StructuredWrite == Right.StructuredWrite;
}

bool HasSameDeclaration(const TypeRecord &Left, const TypeRecord &Right) {
  return Left.Identity == Right.Identity &&
         Left.Descriptor == Right.Descriptor &&
         Left.PublicName == Right.PublicName &&
         Left.Representation == Right.Representation &&
         Left.IsNullable == Right.IsNullable &&
         Left.IsReadable == Right.IsReadable &&
         Left.IsWritable == Right.IsWritable &&
         Left.NestedTypes == Right.NestedTypes && Left.Rank == Right.Rank &&
         Left.ValueRepresentation == Right.ValueRepresentation &&
         Left.MaximumByteCount == Right.MaximumByteCount &&
         Left.Enumeration == Right.Enumeration &&
         HasSameConverters(Left, Right);
}

bool TypeRecordPrecedes(const TypeRecord &Left, const TypeRecord &Right) {
  if (const auto Order =
          TypeDescriptor::Compare(Left.Descriptor, Right.Descriptor);
      Order != std::strong_ordering::equal)
    return Order == std::strong_ordering::less;
  return Left.Identity < Right.Identity;
}

TypeDescriptor CanonicalValueType(ValueKind Kind) noexcept {
  switch (Kind) {
  case ValueKind::Boolean:
    return TypeDescriptor::ForFixed(FixedTypeKey::Boolean);
  case ValueKind::Integer:
    return TypeDescriptor::ForFixed(FixedTypeKey::Int32);
  case ValueKind::Number:
    return TypeDescriptor::ForFixed(FixedTypeKey::Double);
  case ValueKind::String:
    return TypeDescriptor::ForFixed(FixedTypeKey::String);
  }
  return TypeDescriptor::Unsupported();
}

std::string CanonicalTypeText(const TypeDescriptor &Type) {
  if (const auto Fixed = Type.FixedKey())
    return std::string(FixedTypeKeyText(*Fixed));
  if (!Type.Key().IsEmpty())
    return Type.Key().Text();
  return std::string(TypeKindText(Type.Kind()));
}

} // namespace Luna::Detail
