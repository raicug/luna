#pragma once

// The canonical constant boundary. `RegisterConstant` accepts a C++ value and
// must describe it the way every other Luna declaration is described: as one
// canonical type plus one staged value. That normalization happens here, in the
// consumer's translation unit, so the registration backend never needs the
// consumer's type and no Luau type is ever involved.
//
// A request is deliberately explicit about refusal. An unsupported C++ type, a
// user-defined leaf without its validated stable key, and a value outside the
// canonical integer domain each produce a request that carries the reason
// instead of a guessed conversion, and registration turns that reason into one
// deterministic diagnostic.

// clang-format off
#include <luna/binding/value.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
// clang-format on

namespace Luna::Detail {

// Why one constant value is accepted or refused before it reaches Luna.
// `UnsupportedType` means the C++ type has no canonical Luna type today,
// `MissingStableKey` that a user-defined leaf such as an enumeration needs its
// explicit validated stable key, and `OutOfRange` that the value does not fit
// the canonical integer domain Luna converts a constant through.
enum class ConstantValueStatus {
  Supported,
  UnsupportedType,
  MissingStableKey,
  OutOfRange
};

[[nodiscard]] constexpr std::string_view
ConstantValueStatusText(ConstantValueStatus Status) noexcept {
  switch (Status) {
  case ConstantValueStatus::Supported:
    return "supported";
  case ConstantValueStatus::UnsupportedType:
    return "unsupported-type";
  case ConstantValueStatus::MissingStableKey:
    return "missing-stable-key";
  case ConstantValueStatus::OutOfRange:
    return "out-of-range";
  }
  return "unsupported-type";
}

// One normalized constant declaration: the canonical type the value is
// reflected and converted as, and the staged value itself. An enumeration
// constant keeps the enumeration's canonical type, so it never degrades into an
// untyped integer.
struct ConstantRequest final {
  ConstantValueStatus Status = ConstantValueStatus::UnsupportedType;
  TypeDescriptor Type;
  Value Constant;

  // The value an out-of-range refusal received, reported verbatim by the
  // diagnostic so nothing about the refusal is guessed.
  std::int64_t ReceivedInteger = 0;

  [[nodiscard]] bool IsSupported() const noexcept {
    return Status == ConstantValueStatus::Supported;
  }
};

// One refusal, carrying no type and no value.
[[nodiscard]] inline ConstantRequest
RefuseConstant(ConstantValueStatus Status, std::int64_t Received = 0) noexcept {
  ConstantRequest Request;
  Request.Status = Status;
  Request.Type = TypeDescriptor::Unsupported();
  Request.ReceivedInteger = Received;
  return Request;
}

[[nodiscard]] inline ConstantRequest AcceptConstant(TypeDescriptor Type,
                                                    Value Constant) {
  ConstantRequest Request;
  Request.Status = ConstantValueStatus::Supported;
  Request.Type = std::move(Type);
  Request.Constant = std::move(Constant);
  return Request;
}

// The canonical integer domain Luna converts a constant through. It is the
// intersection of the exact-integer range of the Luau number representation and
// the signed 32-bit domain the canonical integer type describes, so no value is
// ever narrowed, wrapped, or rounded on its way in.
[[nodiscard]] constexpr bool
FitsCanonicalInteger(std::int64_t Candidate) noexcept {
  constexpr std::int64_t Lowest =
      static_cast<std::int64_t>(std::numeric_limits<int>::min());
  constexpr std::int64_t Highest =
      static_cast<std::int64_t>(std::numeric_limits<int>::max());
  return Candidate >= Lowest && Candidate <= Highest;
}

// Normalizes one C++ constant into its canonical type and staged value. A
// user-defined leaf consumes the supplied stable key; every fixed leaf ignores
// it.
template <class ValueType>
[[nodiscard]] ConstantRequest MakeConstantRequest(ValueType &&Constant,
                                                  StableTypeKey Key = {}) {
  using Bare = std::remove_cvref_t<ValueType>;

  if constexpr (std::is_same_v<Bare, bool>) {
    return AcceptConstant(TypeDescriptor::ForFixed(FixedTypeKey::Boolean),
                          Value(static_cast<bool>(Constant)));
  } else if constexpr (std::is_enum_v<Bare>) {
    if (!Key.IsValid())
      return RefuseConstant(ConstantValueStatus::MissingStableKey);
    using Underlying = std::underlying_type_t<Bare>;
    const auto Numeric =
        static_cast<std::int64_t>(static_cast<Underlying>(Constant));
    if (!FitsCanonicalInteger(Numeric))
      return RefuseConstant(ConstantValueStatus::OutOfRange, Numeric);
    return AcceptConstant(TypeDescriptor::ForEnumeration(std::move(Key)),
                          Value(static_cast<int>(Numeric)));
  } else if constexpr (std::is_integral_v<Bare>) {
    const auto Numeric = static_cast<std::int64_t>(Constant);
    if (!FitsCanonicalInteger(Numeric))
      return RefuseConstant(ConstantValueStatus::OutOfRange, Numeric);
    return AcceptConstant(TypeDescriptor::ForFixed(FixedTypeKey::Int32),
                          Value(static_cast<int>(Numeric)));
  } else if constexpr (std::is_floating_point_v<Bare>) {
    return AcceptConstant(TypeDescriptor::ForFixed(FixedTypeKey::Double),
                          Value(static_cast<double>(Constant)));
  } else if constexpr (std::is_convertible_v<const Bare &, std::string_view>) {
    const std::string_view Text(Constant);
    return AcceptConstant(TypeDescriptor::ForFixed(FixedTypeKey::String),
                          Value(std::string(Text)));
  } else {
    return RefuseConstant(ConstantValueStatus::UnsupportedType);
  }
}

} // namespace Luna::Detail
