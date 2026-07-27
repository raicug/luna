// clang-format off
#include "state/registration/return_shape.hpp"

#include <luna/type/stable_type_key.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/type/structural_types.hpp"
#include "state/type/type_record.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] TypeDescriptor DynamicPackType() {
  return TypeDescriptor::ForFixed(FixedTypeKey::ValuePack);
}

[[nodiscard]] bool IsOrderedPackDescriptor(const TypeDescriptor &Type) {
  switch (Type.Kind()) {
  case TypeKind::Pair:
  case TypeKind::Tuple:
  case TypeKind::ReturnPack:
    return true;
  default:
    return false;
  }
}

} // namespace

TypeDescriptor CanonicalReturnType(const ReturnMetadata &Return) {
  switch (Return.Disposition()) {
  case ReturnDisposition::Value:
    if (Return.Kind())
      return CanonicalValueType(*Return.Kind());
    break;
  case ReturnDisposition::Pack: {
    if (!Return.HasDeclaredPackShape())
      return DynamicPackType();
    std::vector<TypeDescriptor> Elements;
    const std::span<const ValueKind> Kinds = Return.PackKinds();
    Elements.reserve(Kinds.size());
    for (const ValueKind Kind : Kinds)
      Elements.push_back(CanonicalValueType(Kind));
    return ReturnPackTypeOf(std::move(Elements));
  }
  case ReturnDisposition::Instance:
    if (const StableTypeKey *Class = Return.InstanceKey())
      return TypeDescriptor::ForClass(*Class);
    break;
  case ReturnDisposition::Void:
  case ReturnDisposition::Suppress:
    break;
  }
  return TypeDescriptor::ForFixed(FixedTypeKey::Void);
}

std::vector<TypeDescriptor>
PublishedReturnTypes(const TypeDescriptor &ReturnType) {
  std::vector<TypeDescriptor> Published;

  if (ReturnType.FixedKey() == FixedTypeKey::ValuePack)
    return Published;

  if (IsOrderedPackDescriptor(ReturnType)) {
    const std::span<const TypeDescriptor> Children = ReturnType.Children();
    Published.reserve(Children.size());
    for (const TypeDescriptor &Element : Children)
      Published.push_back(Element);
    return Published;
  }

  Published.push_back(ReturnType);
  return Published;
}

ReturnShape ReflectedReturnShapeOf(const ReturnMetadata &Return) {
  switch (Return.Disposition()) {
  case ReturnDisposition::Value:
    return Return.Kind() ? ReturnShape::Scalar : ReturnShape::Zero;
  case ReturnDisposition::Pack: {
    if (!Return.HasDeclaredPackShape())
      return ReturnShape::Multiple;
    const std::size_t Count = Return.PackKinds().size();
    if (Count == 0)
      return ReturnShape::Zero;
    if (Count == 1)
      return ReturnShape::Scalar;
    return ReturnShape::Multiple;
  }
  case ReturnDisposition::Instance:
    return Return.InstanceKey() ? ReturnShape::Scalar : ReturnShape::Zero;
  case ReturnDisposition::Void:
  case ReturnDisposition::Suppress:
    break;
  }
  return ReturnShape::Zero;
}

std::vector<ReflectionReturnFields>
MakeReflectedReturnFields(const ReturnMetadata &Return) {
  std::vector<ReflectionReturnFields> Reflected;

  const auto Describe = [](std::string Name, const TypeDescriptor &Type) {
    ReflectionReturnFields Fields;
    Fields.Name = std::move(Name);
    Fields.Descriptor = Type;
    if (const auto Identity = TypeIdentityRegistry::ComputeIdentity(Type))
      Fields.Type = *Identity;
    return Fields;
  };

  switch (Return.Disposition()) {
  case ReturnDisposition::Value:
    if (Return.Kind())
      Reflected.push_back(
          Describe("Result", CanonicalValueType(*Return.Kind())));
    return Reflected;
  case ReturnDisposition::Pack: {
    if (!Return.HasDeclaredPackShape())
      return Reflected;
    const std::span<const ValueKind> Kinds = Return.PackKinds();
    Reflected.reserve(Kinds.size());
    for (std::size_t Index = 0; Index < Kinds.size(); ++Index)
      Reflected.push_back(Describe("Result" + std::to_string(Index + 1),
                                   CanonicalValueType(Kinds[Index])));
    return Reflected;
  }
  case ReturnDisposition::Instance:
    if (const StableTypeKey *Class = Return.InstanceKey())
      Reflected.push_back(Describe("Result", TypeDescriptor::ForClass(*Class)));
    return Reflected;
  case ReturnDisposition::Void:
  case ReturnDisposition::Suppress:
    break;
  }
  return Reflected;
}

} // namespace Luna::Detail
