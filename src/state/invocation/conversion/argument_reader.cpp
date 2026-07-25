// clang-format off
#include "state/invocation/conversion/argument_reader.hpp"

#include <lua.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] ArgumentReadResult InternalFailure() noexcept {
  return {.Status = ArgumentReadStatus::InternalFailure};
}

[[nodiscard]] ArgumentReadResult TypeMismatch(lua_State *State,
                                              int ActualType) {
  const char *Name = lua_typename(State, ActualType);
  return {.Status = ArgumentReadStatus::TypeMismatch,
          .ReceivedType = Name ? Name : "unknown"};
}

[[nodiscard]] bool IsSupportedKind(ValueKind Kind) noexcept {
  switch (Kind) {
  case ValueKind::Boolean:
  case ValueKind::Integer:
  case ValueKind::Number:
  case ValueKind::String:
    return true;
  }
  return false;
}

} // namespace

const char *ValueKindName(ValueKind Kind) noexcept {
  switch (Kind) {
  case ValueKind::Boolean:
    return "boolean";
  case ValueKind::Integer:
    return "signed 32-bit integer";
  case ValueKind::Number:
    return "number";
  case ValueKind::String:
    return "string";
  }
  return "unknown";
}

ArgumentReadResult ReadArgument(lua_State *State, int StackIndex,
                                ValueKind ExpectedKind,
                                bool InjectInspectionFailure) noexcept {
  if (!State || InjectInspectionFailure || !IsSupportedKind(ExpectedKind))
    return InternalFailure();

  try {
    const int ActualType = lua_type(State, StackIndex);
    const int ExpectedType = [&]() -> int {
      switch (ExpectedKind) {
      case ValueKind::Boolean:
        return LUA_TBOOLEAN;
      case ValueKind::Integer:
      case ValueKind::Number:
        return LUA_TNUMBER;
      case ValueKind::String:
        return LUA_TSTRING;
      }
      return LUA_TNONE;
    }();

    if (ActualType != ExpectedType)
      return TypeMismatch(State, ActualType);

    switch (ExpectedKind) {
    case ValueKind::Boolean:
      return {.Status = ArgumentReadStatus::Success,
              .ConvertedValue = Value(lua_toboolean(State, StackIndex) != 0),
              .ReceivedType = "boolean"};

    case ValueKind::Number:
      return {.Status = ArgumentReadStatus::Success,
              .ConvertedValue =
                  Value(lua_tonumberx(State, StackIndex, nullptr)),
              .ReceivedType = "number"};

    case ValueKind::Integer: {
      const double Number = lua_tonumberx(State, StackIndex, nullptr);
      if (!std::isfinite(Number))
        return {.Status = ArgumentReadStatus::IntegerNonFinite,
                .ReceivedType = "number",
                .ReceivedNumber = Number};

      constexpr double Minimum =
          static_cast<double>(std::numeric_limits<std::int32_t>::min());
      constexpr double Maximum =
          static_cast<double>(std::numeric_limits<std::int32_t>::max());
      if (Number < Minimum || Number > Maximum)
        return {.Status = ArgumentReadStatus::IntegerOutOfRange,
                .ReceivedType = "number",
                .ReceivedNumber = Number};

      if (std::trunc(Number) != Number)
        return {.Status = ArgumentReadStatus::IntegerFractional,
                .ReceivedType = "number",
                .ReceivedNumber = Number};

      return {.Status = ArgumentReadStatus::Success,
              .ConvertedValue = Value(static_cast<int>(Number)),
              .ReceivedType = "number",
              .ReceivedNumber = Number};
    }

    case ValueKind::String: {
      std::size_t Length = 0;
      const char *Bytes = lua_tolstring(State, StackIndex, &Length);
      if (!Bytes && Length != 0)
        return InternalFailure();
      if (Length > MaximumInvocationStringBytes)
        return {.Status = ArgumentReadStatus::StringTooLong,
                .ReceivedType = "string",
                .ReceivedByteCount = Length};

      return {.Status = ArgumentReadStatus::Success,
              .ConvertedValue = Value(std::string(Bytes ? Bytes : "", Length)),
              .ReceivedType = "string",
              .ReceivedByteCount = Length};
    }
    }
  } catch (...) {
    return InternalFailure();
  }

  return InternalFailure();
}

} // namespace Luna::Detail
