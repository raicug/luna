#pragma once

// clang-format off
#include <luna/binding/value.hpp>

#include <cstddef>
#include <optional>
#include <string>
// clang-format on

struct lua_State;

namespace Luna::Detail {

inline constexpr std::size_t MaximumInvocationStringBytes = 1'048'576;

enum class ArgumentReadStatus {
  Success,
  TypeMismatch,
  IntegerNonFinite,
  IntegerOutOfRange,
  IntegerFractional,
  StringTooLong,
  InternalFailure
};

struct ArgumentReadResult final {
  ArgumentReadStatus Status = ArgumentReadStatus::InternalFailure;
  std::optional<Value> ConvertedValue;
  std::string ReceivedType;
  double ReceivedNumber = 0.0;
  std::size_t ReceivedByteCount = 0;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Status == ArgumentReadStatus::Success && ConvertedValue.has_value();
  }
};

[[nodiscard]] ArgumentReadResult
ReadArgument(lua_State *State, int StackIndex, ValueKind ExpectedKind,
             bool InjectInspectionFailure = false) noexcept;

[[nodiscard]] const char *ValueKindName(ValueKind Kind) noexcept;

} // namespace Luna::Detail
