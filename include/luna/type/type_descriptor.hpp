#pragma once

// clang-format off
#include <luna/type/stable_type_key.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna {

enum class FixedTypeKey {
  Void,
  Boolean,
  Int32,
  Float,
  Double,
  String,
  StringView,
  CString,
  Null,
  Value,
  ValuePack
};

enum class TypeKind {
  Unsupported,
  Fixed,
  Enumeration,
  Class,
  Converted,
  Pointer,
  Array,
  Optional,
  Sequence,
  Map,
  Pair,
  Tuple,
  SharedOwnership,
  BorrowedReference,
  ArgumentPack,
  ReturnPack,
  Callable
};

enum class CvQualification { None, Const, Volatile, ConstVolatile };

[[nodiscard]] constexpr std::string_view
FixedTypeKeyText(FixedTypeKey Key) noexcept {
  switch (Key) {
  case FixedTypeKey::Void:
    return "luna.void";
  case FixedTypeKey::Boolean:
    return "luna.bool";
  case FixedTypeKey::Int32:
    return "luna.int32";
  case FixedTypeKey::Float:
    return "luna.float32";
  case FixedTypeKey::Double:
    return "luna.float64";
  case FixedTypeKey::String:
    return "luna.string";
  case FixedTypeKey::StringView:
    return "luna.string_view";
  case FixedTypeKey::CString:
    return "luna.cstring";
  case FixedTypeKey::Null:
    return "luna.null";
  case FixedTypeKey::Value:
    return "luna.value";
  case FixedTypeKey::ValuePack:
    return "luna.value_pack";
  }
  return "luna.unknown";
}

[[nodiscard]] constexpr std::string_view TypeKindText(TypeKind Kind) noexcept {
  switch (Kind) {
  case TypeKind::Unsupported:
    return "unsupported";
  case TypeKind::Fixed:
    return "fixed";
  case TypeKind::Enumeration:
    return "enum";
  case TypeKind::Class:
    return "class";
  case TypeKind::Converted:
    return "converted";
  case TypeKind::Pointer:
    return "pointer";
  case TypeKind::Array:
    return "array";
  case TypeKind::Optional:
    return "optional";
  case TypeKind::Sequence:
    return "sequence";
  case TypeKind::Map:
    return "map";
  case TypeKind::Pair:
    return "pair";
  case TypeKind::Tuple:
    return "tuple";
  case TypeKind::SharedOwnership:
    return "shared";
  case TypeKind::BorrowedReference:
    return "borrowed";
  case TypeKind::ArgumentPack:
    return "argument_pack";
  case TypeKind::ReturnPack:
    return "return_pack";
  case TypeKind::Callable:
    return "callable";
  }
  return "unsupported";
}

[[nodiscard]] constexpr std::string_view
CvQualificationText(CvQualification Qualification) noexcept {
  switch (Qualification) {
  case CvQualification::None:
    return "none";
  case CvQualification::Const:
    return "const";
  case CvQualification::Volatile:
    return "volatile";
  case CvQualification::ConstVolatile:
    return "const_volatile";
  }
  return "none";
}

[[nodiscard]] constexpr bool
TypeKindAcceptsChildCount(TypeKind Kind, std::size_t ChildCount) noexcept {
  switch (Kind) {
  case TypeKind::Unsupported:
    return false;
  case TypeKind::Fixed:
  case TypeKind::Enumeration:
  case TypeKind::Class:
  case TypeKind::Converted:
    return ChildCount == 0;
  case TypeKind::Pointer:
  case TypeKind::Array:
  case TypeKind::Optional:
  case TypeKind::Sequence:
  case TypeKind::SharedOwnership:
  case TypeKind::BorrowedReference:
    return ChildCount == 1;
  case TypeKind::Map:
  case TypeKind::Pair:
    return ChildCount == 2;
  case TypeKind::Tuple:
  case TypeKind::ArgumentPack:
  case TypeKind::ReturnPack:
    return true;
  case TypeKind::Callable:
    // The first child is the result type; the rest are the parameter types.
    return ChildCount >= 1;
  }
  return false;
}

class TypeDescriptor {
public:
  TypeDescriptor() = default;

  [[nodiscard]] static TypeDescriptor Unsupported() { return TypeDescriptor(); }

  [[nodiscard]] static TypeDescriptor ForFixed(FixedTypeKey Key) {
    TypeDescriptor Descriptor;
    Descriptor.KindValue = TypeKind::Fixed;
    Descriptor.FixedKeyValue = Key;
    return Descriptor;
  }

  [[nodiscard]] static TypeDescriptor ForEnumeration(StableTypeKey Key) {
    return ForLeaf(TypeKind::Enumeration, std::move(Key));
  }

  [[nodiscard]] static TypeDescriptor ForClass(StableTypeKey Key) {
    return ForLeaf(TypeKind::Class, std::move(Key));
  }

  // A user-supplied `Luna::TypeConverter<T>` leaf: identified by the same
  // kind of stable key an enum or a class carries, but converted through the
  // consumer-owned probe/read/write boundary rather than through Luna's own
  // machinery.
  [[nodiscard]] static TypeDescriptor ForConverted(StableTypeKey Key) {
    return ForLeaf(TypeKind::Converted, std::move(Key));
  }

  [[nodiscard]] static TypeDescriptor ForPointer(TypeDescriptor Pointee) {
    std::vector<TypeDescriptor> Children;
    Children.push_back(std::move(Pointee));
    return ForStructure(TypeKind::Pointer, std::move(Children));
  }

  [[nodiscard]] static TypeDescriptor ForArray(TypeDescriptor Element,
                                               std::size_t Extent) {
    std::vector<TypeDescriptor> Children;
    Children.push_back(std::move(Element));
    TypeDescriptor Descriptor =
        ForStructure(TypeKind::Array, std::move(Children));
    Descriptor.ExtentValue = Extent;
    return Descriptor;
  }

  [[nodiscard]] static TypeDescriptor
  ForStructure(TypeKind Kind, std::vector<TypeDescriptor> Children) {
    if (Kind == TypeKind::Unsupported ||
        !TypeKindAcceptsChildCount(Kind, Children.size()))
      return Unsupported();
    TypeDescriptor Descriptor;
    Descriptor.KindValue = Kind;
    Descriptor.ChildrenValue =
        std::make_shared<const ChildList>(std::move(Children));
    return Descriptor;
  }

  [[nodiscard]] TypeDescriptor
  WithQualification(CvQualification Qualification) const {
    TypeDescriptor Descriptor = *this;
    Descriptor.QualificationValue = Qualification;
    return Descriptor;
  }

  [[nodiscard]] TypeKind Kind() const noexcept { return KindValue; }

  [[nodiscard]] std::optional<FixedTypeKey> FixedKey() const noexcept {
    return KindValue == TypeKind::Fixed ? FixedKeyValue
                                        : std::optional<FixedTypeKey>();
  }

  [[nodiscard]] const StableTypeKey &Key() const noexcept { return KeyValue; }

  [[nodiscard]] std::size_t ArrayExtent() const noexcept { return ExtentValue; }

  [[nodiscard]] CvQualification Qualification() const noexcept {
    return QualificationValue;
  }

  [[nodiscard]] std::span<const TypeDescriptor> Children() const noexcept {
    if (!ChildrenValue)
      return {};
    return *ChildrenValue;
  }

  [[nodiscard]] std::size_t ChildCount() const noexcept {
    return ChildrenValue ? ChildrenValue->size() : 0;
  }

  [[nodiscard]] std::size_t PointerDepth() const {
    std::size_t Depth = 0;
    const TypeDescriptor *Current = this;
    while (Current->KindValue == TypeKind::Pointer &&
           Current->ChildCount() == 1) {
      ++Depth;
      const std::span<const TypeDescriptor> Nested = Current->Children();
      Current = Nested.data();
    }
    return Depth;
  }

  [[nodiscard]] bool IsValid() const {
    if (KindValue == TypeKind::Unsupported)
      return false;
    if (!TypeKindAcceptsChildCount(KindValue, ChildCount()))
      return false;
    const bool IsKeyedLeaf = KindValue == TypeKind::Enumeration ||
                             KindValue == TypeKind::Class ||
                             KindValue == TypeKind::Converted;
    if (IsKeyedLeaf && !KeyValue.IsValid())
      return false;
    if (!IsKeyedLeaf && !KeyValue.IsEmpty())
      return false;
    if (KindValue != TypeKind::Array && ExtentValue != 0)
      return false;
    for (const TypeDescriptor &Child : Children()) {
      if (!Child.IsValid())
        return false;
    }
    return true;
  }

  [[nodiscard]] std::size_t Hash() const {
    std::uint64_t Accumulator = 0xcbf29ce484222325ULL;
    MixHash(Accumulator, static_cast<std::uint64_t>(KindValue));
    MixHash(Accumulator, static_cast<std::uint64_t>(QualificationValue));
    MixHash(Accumulator, static_cast<std::uint64_t>(ExtentValue));
    MixHash(Accumulator, KindValue == TypeKind::Fixed
                             ? static_cast<std::uint64_t>(FixedKeyValue) + 1
                             : 0);
    MixHash(Accumulator, static_cast<std::uint64_t>(KeyValue.Hash()));
    MixHash(Accumulator, static_cast<std::uint64_t>(ChildCount()));
    for (const TypeDescriptor &Child : Children())
      MixHash(Accumulator, static_cast<std::uint64_t>(Child.Hash()));
    return static_cast<std::size_t>(Accumulator);
  }

  [[nodiscard]] static std::strong_ordering
  Compare(const TypeDescriptor &Left, const TypeDescriptor &Right) {
    if (const auto Order = Left.KindValue <=> Right.KindValue;
        Order != std::strong_ordering::equal)
      return Order;
    if (Left.KindValue == TypeKind::Fixed) {
      if (const auto Order = Left.FixedKeyValue <=> Right.FixedKeyValue;
          Order != std::strong_ordering::equal)
        return Order;
    }
    if (const auto Order = Left.KeyValue <=> Right.KeyValue;
        Order != std::strong_ordering::equal)
      return Order;
    if (const auto Order = Left.QualificationValue <=> Right.QualificationValue;
        Order != std::strong_ordering::equal)
      return Order;
    if (const auto Order = Left.ExtentValue <=> Right.ExtentValue;
        Order != std::strong_ordering::equal)
      return Order;
    if (const auto Order = Left.ChildCount() <=> Right.ChildCount();
        Order != std::strong_ordering::equal)
      return Order;
    const std::span<const TypeDescriptor> LeftChildren = Left.Children();
    const std::span<const TypeDescriptor> RightChildren = Right.Children();
    for (std::size_t Index = 0; Index < LeftChildren.size(); ++Index) {
      if (const auto Order = Compare(LeftChildren[Index], RightChildren[Index]);
          Order != std::strong_ordering::equal)
        return Order;
    }
    return std::strong_ordering::equal;
  }

  [[nodiscard]] friend bool operator==(const TypeDescriptor &Left,
                                       const TypeDescriptor &Right) {
    return Compare(Left, Right) == std::strong_ordering::equal;
  }

  [[nodiscard]] friend std::strong_ordering
  operator<=>(const TypeDescriptor &Left, const TypeDescriptor &Right) {
    return Compare(Left, Right);
  }

private:
  using ChildList = std::vector<TypeDescriptor>;

  [[nodiscard]] static TypeDescriptor ForLeaf(TypeKind Kind,
                                              StableTypeKey Key) {
    TypeDescriptor Descriptor;
    Descriptor.KindValue = Kind;
    Descriptor.KeyValue = std::move(Key);
    return Descriptor;
  }

  static constexpr void MixHash(std::uint64_t &Accumulator,
                                std::uint64_t Component) noexcept {
    for (std::size_t Index = 0; Index < 8; ++Index) {
      Accumulator ^= (Component >> (Index * 8)) & 0xffULL;
      Accumulator *= 0x100000001b3ULL;
    }
  }

  TypeKind KindValue = TypeKind::Unsupported;
  FixedTypeKey FixedKeyValue = FixedTypeKey::Void;
  StableTypeKey KeyValue;
  CvQualification QualificationValue = CvQualification::None;
  std::size_t ExtentValue = 0;
  std::shared_ptr<const ChildList> ChildrenValue;
};

} // namespace Luna
