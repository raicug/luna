// clang-format off
#include "state/type/owned_value_bridge.hpp"

#include <lua.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr int MaximumBridgeDepth = 64;

[[nodiscard]] int AbsoluteIndex(lua_State *State, int StackIndex) {
  if (StackIndex > 0 || StackIndex <= LUA_REGISTRYINDEX)
    return StackIndex;
  return lua_gettop(State) + StackIndex + 1;
}

[[nodiscard]] OwnedValue BuildFrom(lua_State *State, int StackIndex,
                                   int Depth) {
  if (State == nullptr || Depth > MaximumBridgeDepth)
    return OwnedValue();

  switch (lua_type(State, StackIndex)) {
  case LUA_TNIL:
  case LUA_TNONE:
    return OwnedValue::Nil();
  case LUA_TBOOLEAN:
    return OwnedValue::Boolean(lua_toboolean(State, StackIndex) != 0);
  case LUA_TNUMBER:
    return OwnedValue::Number(lua_tonumberx(State, StackIndex, nullptr));
  case LUA_TSTRING: {
    std::size_t Length = 0;
    const char *Bytes = lua_tolstring(State, StackIndex, &Length);
    return OwnedValue::Text(std::string(Bytes ? Bytes : "", Length));
  }
  case LUA_TTABLE: {
    OwnedValue Table = OwnedValue::Table();
    const int TableIndex = AbsoluteIndex(State, StackIndex);
    if (!lua_checkstack(State, 3))
      return Table;

    int Iterator = 0;
    while ((Iterator = lua_rawiter(State, TableIndex, Iterator)) >= 0) {
      const int ValueIndex = lua_gettop(State);
      const int KeyIndex = ValueIndex - 1;

      if (lua_type(State, KeyIndex) == LUA_TNUMBER) {
        const double Position = lua_tonumberx(State, KeyIndex, nullptr);
        if (Position > 0 &&
            Position == static_cast<double>(static_cast<long long>(Position)))
          Table.Append(BuildFrom(State, ValueIndex, Depth + 1));
        else
          lua_pop(State, 2);
        continue;
      }
      if (lua_type(State, KeyIndex) == LUA_TSTRING) {
        std::size_t Length = 0;
        const char *Bytes = lua_tolstring(State, KeyIndex, &Length);
        Table.SetField(std::string_view(Bytes ? Bytes : "", Length),
                       BuildFrom(State, ValueIndex, Depth + 1));
        continue;
      }
      lua_pop(State, 2);
    }
    return Table;
  }
  default:
    break;
  }
  return OwnedValue();
}

bool PushTo(lua_State *State, const OwnedValue &Source, int Depth) {
  if (State == nullptr || Depth > MaximumBridgeDepth ||
      !lua_checkstack(State, 3))
    return false;

  switch (Source.Kind()) {
  case ValueCategory::None:
  case ValueCategory::Nil:
    lua_pushnil(State);
    return true;
  case ValueCategory::Boolean:
    lua_pushboolean(State, *Source.ToBoolean() ? 1 : 0);
    return true;
  case ValueCategory::Number:
    lua_pushnumber(State, *Source.ToNumber());
    return true;
  case ValueCategory::String: {
    const std::optional<std::string> Text = Source.ToText();
    lua_pushlstring(State, Text->data(), Text->size());
    return true;
  }
  case ValueCategory::Table: {
    lua_createtable(State, static_cast<int>(Source.Size()), 0);
    const int TableIndex = lua_gettop(State);
    for (std::size_t Index = 0; Index < Source.Size(); ++Index) {
      if (!PushTo(State, Source.Element(Index), Depth + 1)) {
        lua_settop(State, TableIndex - 1);
        return false;
      }
      lua_rawseti(State, TableIndex, static_cast<int>(Index + 1));
    }
    for (std::size_t Index = 0; Index < Source.FieldCount(); ++Index) {
      const std::string_view Name = Source.FieldName(Index);
      if (!PushTo(State, Source.Field(Name), Depth + 1)) {
        lua_settop(State, TableIndex - 1);
        return false;
      }
      lua_setfield(State, TableIndex, std::string(Name).c_str());
    }
    return true;
  }
  case ValueCategory::Userdata:
  case ValueCategory::Function:
    return false;
  }
  return false;
}

} // namespace

Luna::OwnedValue BuildOwnedValueFromStack(lua_State *State, int StackIndex) {
  return BuildFrom(State, StackIndex, 0);
}

bool PushOwnedValueToStack(lua_State *State, const Luna::OwnedValue &Source) {
  return PushTo(State, Source, 0);
}

} // namespace Luna::Detail
