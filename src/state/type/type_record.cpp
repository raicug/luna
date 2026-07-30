// clang-format off
#include "state/type/type_record.hpp"

#include <luna/binding/delegate.hpp>
#include <luna/binding/value.hpp>
#include <luna/type/type_descriptor.hpp>

#include <algorithm>
#include <span>
#include <vector>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
// clang-format on

namespace Luna::Detail {

bool EnumerationDomain::Accepts(std::int64_t Candidate) const noexcept {
  if (IsBitflags) {
    return (Candidate & ~SupportedBits) == 0;
  }
  if (Values.empty())
    return true;
  return std::binary_search(Values.begin(), Values.end(), Candidate);
}

std::string_view
EnumerationDomain::NameOf(std::int64_t Candidate) const noexcept {
  const auto Found = std::lower_bound(Values.begin(), Values.end(), Candidate);
  if (Found == Values.end() || *Found != Candidate)
    return {};
  const auto Position =
      static_cast<std::size_t>(std::distance(Values.begin(), Found));
  if (Position >= Names.size())
    return {};
  return Names[Position];
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

  if (IsVoid())
    return !IsReadable && !IsWritable && !ValueRepresentation &&
           Representation == LuauRepresentation::None && !Read && !Write &&
           !StructuredRead && !StructuredWrite;

  if (!IsReadable && !IsWritable)
    return false;

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

TypeDescriptor CanonicalDelegateType(const DelegateShape &Declared) {
  std::vector<TypeDescriptor> Children;
  Children.reserve(Declared.Parameters.size() + 1);
  Children.push_back(Declared.Result
                         ? CanonicalValueType(*Declared.Result)
                         : TypeDescriptor::ForFixed(FixedTypeKey::Void));
  for (const ValueKind Kind : Declared.Parameters)
    Children.push_back(CanonicalValueType(Kind));

  for (const TypeDescriptor &Child : Children) {
    if (!Child.IsValid())
      return TypeDescriptor::Unsupported();
  }
  return TypeDescriptor::ForStructure(TypeKind::Callable, std::move(Children));
}

bool IsCanonicalDelegateType(const TypeDescriptor &Type) noexcept {
  return Type.Kind() == TypeKind::Callable && Type.ChildCount() >= 1 &&
         Type.IsValid();
}

std::string CanonicalTypeText(const TypeDescriptor &Type) {
  if (const auto Fixed = Type.FixedKey())
    return std::string(FixedTypeKeyText(*Fixed));
  if (IsCanonicalDelegateType(Type)) {
    const std::span<const TypeDescriptor> Children = Type.Children();
    std::string Text = "callable ";
    Text += CanonicalTypeText(Children[0]);
    Text += "(";
    for (std::size_t Index = 1; Index < Children.size(); ++Index) {
      if (Index != 1)
        Text += ",";
      Text += CanonicalTypeText(Children[Index]);
    }
    Text += ")";
    return Text;
  }
  if (!Type.Key().IsEmpty())
    return Type.Key().Text();
  return std::string(TypeKindText(Type.Kind()));
}

} // namespace Luna::Detail
