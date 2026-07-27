#pragma once

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

struct ConstantRequest final {
  ConstantValueStatus Status = ConstantValueStatus::UnsupportedType;
  TypeDescriptor Type;
  Value Constant;

  std::int64_t ReceivedInteger = 0;

  [[nodiscard]] bool IsSupported() const noexcept {
    return Status == ConstantValueStatus::Supported;
  }
};

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

[[nodiscard]] constexpr bool
FitsCanonicalInteger(std::int64_t Candidate) noexcept {
  constexpr std::int64_t Lowest =
      static_cast<std::int64_t>(std::numeric_limits<int>::min());
  constexpr std::int64_t Highest =
      static_cast<std::int64_t>(std::numeric_limits<int>::max());
  return Candidate >= Lowest && Candidate <= Highest;
}

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
