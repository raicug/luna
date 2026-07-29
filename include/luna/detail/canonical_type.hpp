#pragma once

// clang-format off
#include <luna/binding/conversion.hpp>
#include <luna/binding/value.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {

class UserKeyCursor {
public:
  explicit UserKeyCursor(std::span<const StableTypeKey> Keys) noexcept
      : KeysValue(Keys) {}

  [[nodiscard]] const StableTypeKey *Next() noexcept {
    if (IndexValue >= KeysValue.size()) {
      ExhaustedValue = true;
      return nullptr;
    }
    const StableTypeKey *Key = &KeysValue[IndexValue];
    ++IndexValue;
    return Key;
  }

  [[nodiscard]] bool WasExhausted() const noexcept { return ExhaustedValue; }

  [[nodiscard]] std::size_t Remaining() const noexcept {
    return KeysValue.size() - IndexValue;
  }

private:
  std::span<const StableTypeKey> KeysValue;
  std::size_t IndexValue = 0;
  bool ExhaustedValue = false;
};

template <class Type>
[[nodiscard]] constexpr CvQualification QualificationOf() noexcept {
  if constexpr (std::is_const_v<Type> && std::is_volatile_v<Type>)
    return CvQualification::ConstVolatile;
  else if constexpr (std::is_const_v<Type>)
    return CvQualification::Const;
  else if constexpr (std::is_volatile_v<Type>)
    return CvQualification::Volatile;
  else
    return CvQualification::None;
}

template <class Type>
[[nodiscard]] constexpr std::optional<FixedTypeKey> FixedKeyFor() noexcept {
  using Leaf = std::remove_cv_t<Type>;
  if constexpr (std::is_void_v<Leaf>)
    return FixedTypeKey::Void;
  else if constexpr (std::is_same_v<Leaf, bool>)
    return FixedTypeKey::Boolean;
  else if constexpr (std::is_same_v<Leaf, int>)
    return FixedTypeKey::Int32;
  else if constexpr (std::is_same_v<Leaf, float>)
    return FixedTypeKey::Float;
  else if constexpr (std::is_same_v<Leaf, double>)
    return FixedTypeKey::Double;
  else if constexpr (std::is_same_v<Leaf, std::string>)
    return FixedTypeKey::String;
  else if constexpr (std::is_same_v<Leaf, std::string_view>)
    return FixedTypeKey::StringView;
  else if constexpr (std::is_same_v<Leaf, std::nullptr_t>)
    return FixedTypeKey::Null;
  else if constexpr (std::is_same_v<Leaf, Value>)
    return FixedTypeKey::Value;
  else
    return std::nullopt;
}

template <class Type> struct CanonicalType {
  static constexpr std::optional<FixedTypeKey> FixedLeafKey =
      FixedKeyFor<Type>();
  static constexpr bool IsFixed = FixedLeafKey.has_value();

  // A class with its own `Luna::TypeConverter<T>` specialization is a
  // *converted* leaf: its shape lives entirely in consumer code, so it
  // never needs registration as a Luna class or enum to appear as a
  // property or field value.
  static constexpr bool IsConverted = !IsFixed &&
                                      !std::is_enum_v<std::remove_cv_t<Type>> &&
                                      std::is_class_v<std::remove_cv_t<Type>> &&
                                      ConversionCapable<std::remove_cv_t<Type>>;

  static constexpr bool IsUserLeaf = !IsFixed && !IsConverted &&
                                     (std::is_enum_v<std::remove_cv_t<Type>> ||
                                      std::is_class_v<std::remove_cv_t<Type>>);
  static constexpr std::size_t UserLeafCount =
      (IsUserLeaf || IsConverted) ? 1 : 0;

  [[nodiscard]] static TypeDescriptor Build(UserKeyCursor &Keys) {
    if constexpr (IsFixed) {
      return TypeDescriptor::ForFixed(*FixedLeafKey);
    } else if constexpr (IsConverted) {
      const StableTypeKey *Key = Keys.Next();
      if (Key == nullptr || !Key->IsValid())
        return TypeDescriptor::Unsupported();
      return TypeDescriptor::ForConverted(*Key);
    } else if constexpr (IsUserLeaf) {
      const StableTypeKey *Key = Keys.Next();
      if (Key == nullptr || !Key->IsValid())
        return TypeDescriptor::Unsupported();
      if constexpr (std::is_enum_v<std::remove_cv_t<Type>>)
        return TypeDescriptor::ForEnumeration(*Key);
      else
        return TypeDescriptor::ForClass(*Key);
    } else {
      return TypeDescriptor::Unsupported();
    }
  }
};

template <class Type> struct CanonicalType<Type &> {
  static constexpr std::size_t UserLeafCount =
      CanonicalType<std::remove_cv_t<Type>>::UserLeafCount;

  [[nodiscard]] static TypeDescriptor Build(UserKeyCursor &Keys) {
    return CanonicalType<std::remove_cv_t<Type>>::Build(Keys);
  }
};

template <class Type> struct CanonicalType<Type &&> {
  static constexpr std::size_t UserLeafCount =
      CanonicalType<std::remove_cv_t<Type>>::UserLeafCount;

  [[nodiscard]] static TypeDescriptor Build(UserKeyCursor &Keys) {
    return CanonicalType<std::remove_cv_t<Type>>::Build(Keys);
  }
};

template <class Type> struct CanonicalType<Type *> {
  static constexpr bool IsCString =
      std::is_same_v<std::remove_cv_t<Type>, char>;
  static constexpr std::size_t UserLeafCount =
      IsCString ? 0 : CanonicalType<std::remove_cv_t<Type>>::UserLeafCount;

  [[nodiscard]] static TypeDescriptor Build(UserKeyCursor &Keys) {
    if constexpr (IsCString) {
      return TypeDescriptor::ForFixed(FixedTypeKey::CString);
    } else {
      const TypeDescriptor Pointee =
          CanonicalType<std::remove_cv_t<Type>>::Build(Keys).WithQualification(
              QualificationOf<Type>());
      return TypeDescriptor::ForPointer(Pointee);
    }
  }
};

template <class Type, std::size_t Extent> struct CanonicalType<Type[Extent]> {
  static constexpr std::size_t UserLeafCount =
      CanonicalType<std::remove_cv_t<Type>>::UserLeafCount;

  [[nodiscard]] static TypeDescriptor Build(UserKeyCursor &Keys) {
    const TypeDescriptor Element =
        CanonicalType<std::remove_cv_t<Type>>::Build(Keys).WithQualification(
            QualificationOf<Type>());
    return TypeDescriptor::ForArray(Element, Extent);
  }
};

template <class Type, std::size_t Extent>
struct CanonicalType<std::array<Type, Extent>> {
  static constexpr std::size_t UserLeafCount =
      CanonicalType<std::remove_cv_t<Type>>::UserLeafCount;

  [[nodiscard]] static TypeDescriptor Build(UserKeyCursor &Keys) {
    const TypeDescriptor Element =
        CanonicalType<std::remove_cv_t<Type>>::Build(Keys).WithQualification(
            QualificationOf<Type>());
    return TypeDescriptor::ForArray(Element, Extent);
  }
};

template <class Type> struct CanonicalType<std::optional<Type>> {
  static constexpr std::size_t UserLeafCount =
      CanonicalType<std::remove_cv_t<Type>>::UserLeafCount;

  [[nodiscard]] static TypeDescriptor Build(UserKeyCursor &Keys) {
    std::vector<TypeDescriptor> Children;
    Children.push_back(CanonicalType<std::remove_cv_t<Type>>::Build(Keys));
    return TypeDescriptor::ForStructure(TypeKind::Optional,
                                        std::move(Children));
  }
};

template <class Type> struct CanonicalType<std::shared_ptr<Type>> {
  static constexpr std::size_t UserLeafCount =
      CanonicalType<std::remove_cv_t<Type>>::UserLeafCount;

  [[nodiscard]] static TypeDescriptor Build(UserKeyCursor &Keys) {
    const TypeDescriptor Owned =
        CanonicalType<std::remove_cv_t<Type>>::Build(Keys).WithQualification(
            QualificationOf<Type>());
    std::vector<TypeDescriptor> Children;
    Children.push_back(Owned);
    return TypeDescriptor::ForStructure(TypeKind::SharedOwnership,
                                        std::move(Children));
  }
};

template <class Type> struct CanonicalType<std::reference_wrapper<Type>> {
  static constexpr std::size_t UserLeafCount =
      CanonicalType<std::remove_cv_t<Type>>::UserLeafCount;

  [[nodiscard]] static TypeDescriptor Build(UserKeyCursor &Keys) {
    const TypeDescriptor Referenced =
        CanonicalType<std::remove_cv_t<Type>>::Build(Keys).WithQualification(
            QualificationOf<Type>());
    std::vector<TypeDescriptor> Children;
    Children.push_back(Referenced);
    return TypeDescriptor::ForStructure(TypeKind::BorrowedReference,
                                        std::move(Children));
  }
};

template <class Type, class Allocator>
struct CanonicalType<std::vector<Type, Allocator>> {
  static constexpr std::size_t UserLeafCount =
      CanonicalType<std::remove_cv_t<Type>>::UserLeafCount;

  [[nodiscard]] static TypeDescriptor Build(UserKeyCursor &Keys) {
    std::vector<TypeDescriptor> Children;
    Children.push_back(CanonicalType<std::remove_cv_t<Type>>::Build(Keys));
    return TypeDescriptor::ForStructure(TypeKind::Sequence,
                                        std::move(Children));
  }
};

template <class Type, class Allocator>
struct CanonicalType<std::deque<Type, Allocator>> {
  static constexpr std::size_t UserLeafCount =
      CanonicalType<std::remove_cv_t<Type>>::UserLeafCount;

  [[nodiscard]] static TypeDescriptor Build(UserKeyCursor &Keys) {
    std::vector<TypeDescriptor> Children;
    Children.push_back(CanonicalType<std::remove_cv_t<Type>>::Build(Keys));
    return TypeDescriptor::ForStructure(TypeKind::Sequence,
                                        std::move(Children));
  }
};

template <class Type, class Allocator>
struct CanonicalType<std::list<Type, Allocator>> {
  static constexpr std::size_t UserLeafCount =
      CanonicalType<std::remove_cv_t<Type>>::UserLeafCount;

  [[nodiscard]] static TypeDescriptor Build(UserKeyCursor &Keys) {
    std::vector<TypeDescriptor> Children;
    Children.push_back(CanonicalType<std::remove_cv_t<Type>>::Build(Keys));
    return TypeDescriptor::ForStructure(TypeKind::Sequence,
                                        std::move(Children));
  }
};

template <class Key, class Mapped, class Compare, class Allocator>
struct CanonicalType<std::map<Key, Mapped, Compare, Allocator>> {
  static constexpr std::size_t UserLeafCount =
      CanonicalType<std::remove_cv_t<Key>>::UserLeafCount +
      CanonicalType<std::remove_cv_t<Mapped>>::UserLeafCount;

  [[nodiscard]] static TypeDescriptor Build(UserKeyCursor &Keys) {
    std::vector<TypeDescriptor> Children;
    Children.push_back(CanonicalType<std::remove_cv_t<Key>>::Build(Keys));
    Children.push_back(CanonicalType<std::remove_cv_t<Mapped>>::Build(Keys));
    return TypeDescriptor::ForStructure(TypeKind::Map, std::move(Children));
  }
};

template <class Key, class Mapped, class Hash, class Equal, class Allocator>
struct CanonicalType<std::unordered_map<Key, Mapped, Hash, Equal, Allocator>> {
  static constexpr std::size_t UserLeafCount =
      CanonicalType<std::remove_cv_t<Key>>::UserLeafCount +
      CanonicalType<std::remove_cv_t<Mapped>>::UserLeafCount;

  [[nodiscard]] static TypeDescriptor Build(UserKeyCursor &Keys) {
    std::vector<TypeDescriptor> Children;
    Children.push_back(CanonicalType<std::remove_cv_t<Key>>::Build(Keys));
    Children.push_back(CanonicalType<std::remove_cv_t<Mapped>>::Build(Keys));
    return TypeDescriptor::ForStructure(TypeKind::Map, std::move(Children));
  }
};

template <class First, class Second>
struct CanonicalType<std::pair<First, Second>> {
  static constexpr std::size_t UserLeafCount =
      CanonicalType<std::remove_cv_t<First>>::UserLeafCount +
      CanonicalType<std::remove_cv_t<Second>>::UserLeafCount;

  [[nodiscard]] static TypeDescriptor Build(UserKeyCursor &Keys) {
    std::vector<TypeDescriptor> Children;
    Children.push_back(CanonicalType<std::remove_cv_t<First>>::Build(Keys));
    Children.push_back(CanonicalType<std::remove_cv_t<Second>>::Build(Keys));
    return TypeDescriptor::ForStructure(TypeKind::Pair, std::move(Children));
  }
};

template <class... Types> struct CanonicalType<std::tuple<Types...>> {
  static constexpr std::size_t UserLeafCount =
      (std::size_t{0} + ... +
       CanonicalType<std::remove_cv_t<Types>>::UserLeafCount);

  [[nodiscard]] static TypeDescriptor Build(UserKeyCursor &Keys) {
    std::vector<TypeDescriptor> Children;
    (Children.push_back(CanonicalType<std::remove_cv_t<Types>>::Build(Keys)),
     ...);
    return TypeDescriptor::ForStructure(TypeKind::Tuple, std::move(Children));
  }
};

template <class Type>
inline constexpr std::size_t UserDefinedLeafCount =
    CanonicalType<std::remove_cvref_t<Type>>::UserLeafCount;

template <class Type>
[[nodiscard]] inline TypeDescriptor
CanonicalDescriptorFor(std::span<const StableTypeKey> UserKeys = {}) {
  UserKeyCursor Keys(UserKeys);
  const TypeDescriptor Descriptor =
      CanonicalType<std::remove_cvref_t<Type>>::Build(Keys);
  if (Keys.WasExhausted() || Keys.Remaining() != 0)
    return TypeDescriptor::Unsupported();
  if (!Descriptor.IsValid())
    return TypeDescriptor::Unsupported();
  return Descriptor;
}

} // namespace Luna::Detail
