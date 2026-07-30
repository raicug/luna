#pragma once

// clang-format off
#include <luna/core/results/registration_result.hpp>
#include <luna/type/stable_type_key.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
// clang-format on

namespace Luna {

class State;

namespace Detail {

class NamespaceBuilderState;

struct EnumerationPolicy final {
  std::int64_t Minimum = 0;
  std::int64_t Maximum = 0;
  bool IsScoped = true;
  bool UnderlyingIsSigned = true;
};

template <class Enum>
[[nodiscard]] constexpr EnumerationPolicy EnumerationPolicyFor() noexcept {
  using Underlying = std::underlying_type_t<Enum>;

  EnumerationPolicy Policy;

  Policy.IsScoped = !std::is_convertible_v<Enum, Underlying>;
  Policy.UnderlyingIsSigned = std::is_signed_v<Underlying>;

  if constexpr (std::is_signed_v<Underlying>) {
    Policy.Minimum =
        static_cast<std::int64_t>(std::numeric_limits<Underlying>::min());
    Policy.Maximum =
        static_cast<std::int64_t>(std::numeric_limits<Underlying>::max());
  } else {
    constexpr auto Highest =
        static_cast<std::uint64_t>(std::numeric_limits<Underlying>::max());
    constexpr auto Representable =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    Policy.Minimum = 0;
    Policy.Maximum = static_cast<std::int64_t>(
        Highest < Representable ? Highest : Representable);
  }
  return Policy;
}

class EnumStaging final {
public:
  EnumStaging() noexcept;
  EnumStaging(std::shared_ptr<NamespaceBuilderState> Plan,
              std::size_t Node) noexcept;

  EnumStaging(const EnumStaging &) = delete;
  EnumStaging &operator=(const EnumStaging &) = delete;
  EnumStaging(EnumStaging &&Other) noexcept;
  EnumStaging &operator=(EnumStaging &&Other) noexcept;
  ~EnumStaging();

  void StageValue(std::string_view Name, std::int64_t Numeric);
  void StageAlias(std::string_view AliasName, std::string_view CanonicalName);
  void StageBitflags(bool HasDeclaredMask, std::int64_t SupportedBits);
  void StageUnscopedOptIn();
  void StageObjectRepresentation();

  void StageDocumentation(std::string_view Member, std::string_view Text);
  void StageAttribute(std::string_view Member, std::string_view Name,
                      std::string_view AttributeValue);
  void StageExample(std::string_view Member, std::string_view Text);

  [[nodiscard]] RegistrationResult Commit();
  [[nodiscard]] std::string_view QualifiedName() const noexcept;

private:
  std::shared_ptr<NamespaceBuilderState> Plan;
  std::size_t Node = 0;
};

[[nodiscard]] EnumStaging
StageEnumeration(std::shared_ptr<NamespaceBuilderState> Plan,
                 std::size_t ScopeNode, std::string_view Name,
                 const StableTypeKey &Key, const EnumerationPolicy &Policy);

[[nodiscard]] EnumStaging StageRootEnumeration(State &Owner,
                                               std::string_view Name,
                                               const StableTypeKey &Key,
                                               const EnumerationPolicy &Policy);

} // namespace Detail

template <class Enum> class EnumBuilder final {
  static_assert(std::is_enum_v<Enum>,
                "Luna enum registration requires an enumeration type.");

public:
  using Underlying = std::underlying_type_t<Enum>;

  EnumBuilder(const EnumBuilder &) = delete;
  EnumBuilder &operator=(const EnumBuilder &) = delete;
  EnumBuilder(EnumBuilder &&Other) noexcept = default;
  EnumBuilder &operator=(EnumBuilder &&Other) noexcept = default;

  ~EnumBuilder() = default;

  EnumBuilder &Value(std::string_view Name, Enum Enumerator) {
    Staging.StageValue(Name, NumericValueOf(Enumerator));
    return *this;
  }

  EnumBuilder &Value(std::string_view Name, std::int64_t Numeric) {
    Staging.StageValue(Name, Numeric);
    return *this;
  }

  EnumBuilder &Alias(std::string_view AliasName,
                     std::string_view CanonicalName) {
    Staging.StageAlias(AliasName, CanonicalName);
    return *this;
  }

  EnumBuilder &Bitflags() {
    Staging.StageBitflags(false, 0);
    return *this;
  }

  EnumBuilder &Bitflags(Enum SupportedBits) {
    Staging.StageBitflags(true, NumericValueOf(SupportedBits));
    return *this;
  }

  EnumBuilder &Bitflags(std::int64_t SupportedBits) {
    Staging.StageBitflags(true, SupportedBits);
    return *this;
  }

  EnumBuilder &AllowUnscoped() {
    Staging.StageUnscopedOptIn();
    return *this;
  }

  // Publishes each enumerator as one interned enumerator object rather than
  // as its bare number. An object reports `typeof` as "EnumItem", carries
  // `Name`, `Value`, and `EnumName`, and compares equal only to itself, so a
  // script can never hand a bare number where the enumeration is declared.
  EnumBuilder &AsObjects() {
    Staging.StageObjectRepresentation();
    return *this;
  }

  EnumBuilder &Documentation(std::string_view Text) {
    Staging.StageDocumentation(std::string_view(), Text);
    return *this;
  }

  EnumBuilder &Documentation(std::string_view Member, std::string_view Text) {
    Staging.StageDocumentation(Member, Text);
    return *this;
  }

  EnumBuilder &Attribute(std::string_view Name,
                         std::string_view AttributeValue) {
    Staging.StageAttribute(std::string_view(), Name, AttributeValue);
    return *this;
  }

  EnumBuilder &Attribute(std::string_view Member, std::string_view Name,
                         std::string_view AttributeValue) {
    Staging.StageAttribute(Member, Name, AttributeValue);
    return *this;
  }

  EnumBuilder &Example(std::string_view Text) {
    Staging.StageExample(std::string_view(), Text);
    return *this;
  }

  EnumBuilder &Example(std::string_view Member, std::string_view Text) {
    Staging.StageExample(Member, Text);
    return *this;
  }

  [[nodiscard]] std::string_view QualifiedName() const noexcept {
    return Staging.QualifiedName();
  }

  [[nodiscard]] RegistrationResult Commit() { return Staging.Commit(); }

private:
  friend class BindingRegistry;
  friend class NamespaceBuilder;

  explicit EnumBuilder(Detail::EnumStaging Staged) noexcept
      : Staging(std::move(Staged)) {}

  [[nodiscard]] static std::int64_t NumericValueOf(Enum Enumerator) noexcept {
    return static_cast<std::int64_t>(static_cast<Underlying>(Enumerator));
  }

  Detail::EnumStaging Staging;
};

} // namespace Luna
