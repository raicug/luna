#pragma once

// One transaction-attached enum builder.
//
// `BindingRegistry::RegisterEnum` and `NamespaceBuilder::RegisterEnum` accept
// one validated identifier segment plus the enumeration's validated stable type
// key and return a builder that stages the whole enumeration in the pending
// plan of its chain. Every operation stages metadata only: no enumerator,
// alias, flag mask, documentation string, or attribute becomes visible before
// `Commit` submits the plan as one outermost registration transaction, and
// destroying an uncommitted builder has no virtual-machine effect at all.
//
// The enumeration is checked, never guessed. Scoped behavior is the default and
// exposing an unscoped enumeration requires the explicit `AllowUnscoped`
// opt-in. Bitflag behavior requires the explicit `Bitflags` declaration. Every
// value is validated against the enumeration's declared C++ underlying type and
// against the exact-integer domain Luna converts through, so an out-of-range
// value is rejected rather than narrowed, wrapped, or rounded. A duplicate name
// is rejected and a duplicate numeric value is rejected unless it is declared
// explicitly as an `Alias` of one canonical enumerator.

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

// The declared C++ shape of one enumeration, captured once where the
// enumeration's type is still known. The registration backend never sees the
// consumer's type, so this is what lets it validate every value against the
// declared underlying range and reject an unscoped enumeration that was not
// explicitly opted in.
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

  // An unscoped enumeration is exactly the one that converts implicitly to its
  // own underlying type; a scoped one never does.
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

// The erased half of one enum builder. It owns nothing but a reference to the
// shared pending plan and the staged enumeration inside it, so the public
// builder template stays free of Luna's private registration model.
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

  // An empty member documents or annotates the enumeration itself; otherwise
  // the named enumerator or alias, which must already be declared.
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

// Stages one enumeration inside a scope of an existing pending plan.
[[nodiscard]] EnumStaging
StageEnumeration(std::shared_ptr<NamespaceBuilderState> Plan,
                 std::size_t ScopeNode, std::string_view Name,
                 const StableTypeKey &Key, const EnumerationPolicy &Policy);

// Stages one root-scope enumeration in a new pending plan of its own.
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

  // Destroying an uncommitted builder discards its staged enumeration without
  // touching the virtual machine, reflection, or dispatch.
  ~EnumBuilder() = default;

  // Stages one canonical enumerator.
  EnumBuilder &Value(std::string_view Name, Enum Enumerator) {
    Staging.StageValue(Name, NumericValueOf(Enumerator));
    return *this;
  }

  // Stages one canonical enumerator from its numeric value. The value is
  // validated against the declared underlying type and the canonical integer
  // domain, so an out-of-range value is refused instead of narrowed.
  EnumBuilder &Value(std::string_view Name, std::int64_t Numeric) {
    Staging.StageValue(Name, Numeric);
    return *this;
  }

  // Declares one additional name for an existing canonical enumerator. This is
  // the only way a duplicate numeric value is accepted.
  EnumBuilder &Alias(std::string_view AliasName,
                     std::string_view CanonicalName) {
    Staging.StageAlias(AliasName, CanonicalName);
    return *this;
  }

  // Declares bitflag behavior with the supported-bit mask computed from the
  // declared canonical enumerators.
  EnumBuilder &Bitflags() {
    Staging.StageBitflags(false, 0);
    return *this;
  }

  // Declares bitflag behavior with an explicit supported-bit mask. Every
  // declared enumerator must be a subset of it, and every converted value
  // carrying any other bit is rejected whole.
  EnumBuilder &Bitflags(Enum SupportedBits) {
    Staging.StageBitflags(true, NumericValueOf(SupportedBits));
    return *this;
  }

  EnumBuilder &Bitflags(std::int64_t SupportedBits) {
    Staging.StageBitflags(true, SupportedBits);
    return *this;
  }

  // Permits exposing an unscoped enumeration. Without it, an unscoped
  // enumeration is refused deterministically.
  EnumBuilder &AllowUnscoped() {
    Staging.StageUnscopedOptIn();
    return *this;
  }

  // Documents the enumeration itself.
  EnumBuilder &Documentation(std::string_view Text) {
    Staging.StageDocumentation(std::string_view(), Text);
    return *this;
  }

  // Documents one already declared enumerator or alias.
  EnumBuilder &Documentation(std::string_view Member, std::string_view Text) {
    Staging.StageDocumentation(Member, Text);
    return *this;
  }

  // Annotates the enumeration itself.
  EnumBuilder &Attribute(std::string_view Name,
                         std::string_view AttributeValue) {
    Staging.StageAttribute(std::string_view(), Name, AttributeValue);
    return *this;
  }

  // Annotates one already declared enumerator or alias.
  EnumBuilder &Attribute(std::string_view Member, std::string_view Name,
                         std::string_view AttributeValue) {
    Staging.StageAttribute(Member, Name, AttributeValue);
    return *this;
  }

  // Adds one usage example to the enumeration itself. Examples are reflected in
  // declaration order, so generated material repeats them exactly as declared.
  EnumBuilder &Example(std::string_view Text) {
    Staging.StageExample(std::string_view(), Text);
    return *this;
  }

  // Adds one usage example to one already declared enumerator or alias.
  EnumBuilder &Example(std::string_view Member, std::string_view Text) {
    Staging.StageExample(Member, Text);
    return *this;
  }

  // The canonical `.`-separated qualified name of this enumeration.
  [[nodiscard]] std::string_view QualifiedName() const noexcept {
    return Staging.QualifiedName();
  }

  // Submits the whole pending plan of this builder chain as one outermost
  // registration transaction.
  [[nodiscard]] RegistrationResult Commit() { return Staging.Commit(); }

private:
  friend class BindingRegistry;
  friend class NamespaceBuilder;

  explicit EnumBuilder(Detail::EnumStaging Staged) noexcept
      : Staging(std::move(Staged)) {}

  // The numeric value of one enumerator, widened without narrowing. A value an
  // out-of-contract cast produced is still validated against the declared
  // underlying range before it is staged.
  [[nodiscard]] static std::int64_t NumericValueOf(Enum Enumerator) noexcept {
    return static_cast<std::int64_t>(static_cast<Underlying>(Enumerator));
  }

  Detail::EnumStaging Staging;
};

} // namespace Luna
