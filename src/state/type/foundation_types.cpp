// clang-format off
#include "state/type/foundation_types.hpp"

#include <luna/binding/value.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/type/conversion_outcome.hpp"
#include "state/type/type_record.hpp"

#include <lua.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>
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

[[nodiscard]] bool Accepts(lua_State *State, int StackIndex, int ExpectedType,
                           ArgumentReadResult &Rejection) {
  if (!State) {
    Rejection = InternalFailure();
    return false;
  }
  const int ActualType = lua_type(State, StackIndex);
  if (ActualType != ExpectedType) {
    Rejection = TypeMismatch(State, ActualType);
    return false;
  }
  return true;
}

[[nodiscard]] ArgumentReadResult ReadBoolean(lua_State *State, int StackIndex) {
  ArgumentReadResult Rejection;
  if (!Accepts(State, StackIndex, LUA_TBOOLEAN, Rejection))
    return Rejection;
  return {.Status = ArgumentReadStatus::Success,
          .ConvertedValue = Value(lua_toboolean(State, StackIndex) != 0),
          .ReceivedType = "boolean"};
}

[[nodiscard]] ArgumentReadResult ReadDouble(lua_State *State, int StackIndex) {
  ArgumentReadResult Rejection;
  if (!Accepts(State, StackIndex, LUA_TNUMBER, Rejection))
    return Rejection;
  return {.Status = ArgumentReadStatus::Success,
          .ConvertedValue = Value(lua_tonumberx(State, StackIndex, nullptr)),
          .ReceivedType = "number"};
}

[[nodiscard]] ArgumentReadResult ReadInt32(lua_State *State, int StackIndex) {
  ArgumentReadResult Rejection;
  if (!Accepts(State, StackIndex, LUA_TNUMBER, Rejection))
    return Rejection;

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

[[nodiscard]] ArgumentReadResult ReadString(lua_State *State, int StackIndex) {
  ArgumentReadResult Rejection;
  if (!Accepts(State, StackIndex, LUA_TSTRING, Rejection))
    return Rejection;

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

[[nodiscard]] bool WriteBoolean(lua_State *State, const Value &Source) {
  const auto *Typed = std::get_if<bool>(&Source);
  if (!State || !Typed)
    return false;
  lua_pushboolean(State, *Typed ? 1 : 0);
  return true;
}

[[nodiscard]] bool WriteInt32(lua_State *State, const Value &Source) {
  const auto *Typed = std::get_if<int>(&Source);
  if (!State || !Typed)
    return false;
  lua_pushinteger(State, *Typed);
  return true;
}

[[nodiscard]] bool WriteDouble(lua_State *State, const Value &Source) {
  const auto *Typed = std::get_if<double>(&Source);
  if (!State || !Typed)
    return false;
  lua_pushnumber(State, *Typed);
  return true;
}

[[nodiscard]] bool WriteString(lua_State *State, const Value &Source) {
  const auto *Typed = std::get_if<std::string>(&Source);
  if (!State || !Typed)
    return false;
  if (Typed->size() > MaximumInvocationStringBytes)
    return false;
  lua_pushlstring(State, Typed->data(), Typed->size());
  return true;
}

[[nodiscard]] TypeRecord Declare(FixedTypeKey Key, std::string PublicName,
                                 LuauRepresentation Representation,
                                 ValueKind Kind, TypeReadFunction Read,
                                 TypeWriteFunction Write) {
  TypeRecord Record;
  Record.Descriptor = TypeDescriptor::ForFixed(Key);
  if (const auto Identity =
          TypeIdentityRegistry::ComputeIdentity(Record.Descriptor))
    Record.Identity = *Identity;
  Record.PublicName = std::move(PublicName);
  Record.Representation = Representation;
  Record.IsNullable = false;
  Record.IsReadable = true;
  Record.IsWritable = true;
  Record.Rank = ConversionRankCategory::Exact;
  Record.ValueRepresentation = Kind;
  Record.Read = Read;
  Record.Write = Write;
  return Record;
}

[[nodiscard]] TypeRecord DeclareVoid() {
  TypeRecord Record;
  Record.Descriptor = TypeDescriptor::ForFixed(FixedTypeKey::Void);
  if (const auto Identity =
          TypeIdentityRegistry::ComputeIdentity(Record.Descriptor))
    Record.Identity = *Identity;
  Record.PublicName = "void";
  Record.Representation = LuauRepresentation::None;
  Record.IsReadable = false;
  Record.IsWritable = false;
  Record.Rank = ConversionRankCategory::Exact;
  return Record;
}

} // namespace

std::vector<TypeRecord> FoundationTypeRecords() {
  std::vector<TypeRecord> Records;
  Records.reserve(5);
  Records.push_back(DeclareVoid());
  Records.push_back(Declare(FixedTypeKey::Boolean, "boolean",
                            LuauRepresentation::Boolean, ValueKind::Boolean,
                            &ReadBoolean, &WriteBoolean));
  Records.push_back(Declare(FixedTypeKey::Int32, "signed 32-bit integer",
                            LuauRepresentation::Number, ValueKind::Integer,
                            &ReadInt32, &WriteInt32));
  Records.push_back(Declare(FixedTypeKey::Double, "number",
                            LuauRepresentation::Number, ValueKind::Number,
                            &ReadDouble, &WriteDouble));

  TypeRecord Text =
      Declare(FixedTypeKey::String, "string", LuauRepresentation::String,
              ValueKind::String, &ReadString, &WriteString);

  Text.MaximumByteCount = MaximumInvocationStringBytes;
  Records.push_back(std::move(Text));
  return Records;
}

} // namespace Luna::Detail
