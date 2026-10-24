#include "runtime.hpp"

#include <luna/convert.hpp>
#include <luna/value.hpp>

#include <lua.h>
#include <lualib.h>

#include <string>
#include <utility>

namespace luna {
namespace detail {
namespace {

// Reads a value off the stack into an OwnedValue. Tables are copied, bounded so
// a self-referential table cannot spin.
OwnedValue ReadOwned(lua_State *L, int Index, int Depth) {
  switch (lua_type(L, Index)) {
  case LUA_TNIL:
    return OwnedValue();

  case LUA_TBOOLEAN:
    return OwnedValue::Boolean(lua_toboolean(L, Index) != 0);

  case LUA_TNUMBER:
    return OwnedValue::Number(lua_tonumber(L, Index));

  case LUA_TSTRING: {
    std::size_t Length = 0;
    const char *Text = lua_tolstring(L, Index, &Length);
    return OwnedValue::Text(std::string_view(Text, Length));
  }

  case LUA_TTABLE: {
    OwnedValue Result = OwnedValue::Table();
    if (Depth >= 8)
      return Result;

    const int Absolute =
        Index < 0 && Index > LUA_REGISTRYINDEX ? lua_gettop(L) + Index + 1
                                               : Index;

    lua_pushnil(L);
    while (lua_next(L, Absolute) != 0) {
      // Key is at -2, value at -1.
      const int ValueIndex = lua_gettop(L);
      const int KeyIndex = ValueIndex - 1;

      if (lua_type(L, KeyIndex) == LUA_TNUMBER) {
        Result.Append(ReadOwned(L, ValueIndex, Depth + 1));
      } else if (lua_type(L, KeyIndex) == LUA_TSTRING) {
        std::size_t Length = 0;
        const char *Text = lua_tolstring(L, KeyIndex, &Length);
        Result.SetField(std::string_view(Text, Length),
                        ReadOwned(L, ValueIndex, Depth + 1));
      }

      lua_pop(L, 1); // pop the value, keep the key for the next iteration
    }
    return Result;
  }

  default:
    return OwnedValue();
  }
}

} // namespace

} // namespace detail

// ---------------------------------------------------------------------------
// OwnedValue
// ---------------------------------------------------------------------------

OwnedValue OwnedValue::Table() {
  OwnedValue Result;
  auto Table = std::make_shared<OwnedTable>();
  // A record like a Drawing entry is eight fields and a couple of instances;
  // reserving that up front turns eight growth reallocations into none.
  Table->Fields.reserve(16);
  Table->Items.reserve(8);
  Result.Data = std::move(Table);
  return Result;
}

OwnedValue &OwnedValue::SetField(std::string_view Name, OwnedValue FieldValue) {
  auto *Slot = std::get_if<std::shared_ptr<OwnedTable>>(&Data);
  if (!Slot) {
    Data = std::make_shared<OwnedTable>();
    Slot = std::get_if<std::shared_ptr<OwnedTable>>(&Data);
  }

  // Appending to a copy must not mutate a table someone else holds, so a table
  // that is shared gets cloned on first write.
  if (Slot->use_count() > 1)
    *Slot = std::make_shared<OwnedTable>(**Slot);

  (*Slot)->Fields.push_back(
      OwnedTable::Entry{std::string(Name), std::move(FieldValue)});
  return *this;
}

OwnedValue &OwnedValue::Append(OwnedValue Item) {
  auto *Slot = std::get_if<std::shared_ptr<OwnedTable>>(&Data);
  if (!Slot) {
    Data = std::make_shared<OwnedTable>();
    Slot = std::get_if<std::shared_ptr<OwnedTable>>(&Data);
  } else if (Slot->use_count() > 1) {
    *Slot = std::make_shared<OwnedTable>(**Slot);
  }

  (*Slot)->Items.push_back(std::move(Item));
  return *this;
}

// ---------------------------------------------------------------------------
// Reflection into Lua
// ---------------------------------------------------------------------------

namespace detail {

int PushOwnedValue(lua_State *L, const OwnedValue &Value) {
  if (const auto *Instance = Value.AsInstance()) {
    if (!Instance->Object)
      return 0;
    const bool Ok = PushBorrowedInstance(L, Instance->TypeToken,
                                         Instance->Object.get(), nullptr);
    if (Ok)
      return 1;
    lua_pushnil(L);
    return 1;
  }

  if (const OwnedTable *Table = Value.AsTable()) {
    const auto FieldCount = static_cast<int>(Table->Fields.size());
    const auto ItemCount = static_cast<int>(Table->Items.size());
    lua_createtable(L, ItemCount, FieldCount);

    for (const OwnedTable::Entry &Field : Table->Fields) {
      PushOwnedValue(L, Field.Value);
      lua_setfield(L, -2, Field.Key.c_str());
    }

    int Position = 1;
    for (const OwnedValue &Item : Table->Items) {
      PushOwnedValue(L, Item);
      lua_rawseti(L, -2, Position++);
    }
    return 1;
  }

  if (std::holds_alternative<std::monostate>(Value.Get())) {
    lua_pushnil(L);
    return 1;
  }
  if (Value.IsBoolean()) {
    lua_pushboolean(L, Value.AsBoolean() ? 1 : 0);
    return 1;
  }
  if (Value.IsNumber()) {
    lua_pushnumber(L, Value.AsNumber());
    return 1;
  }
  if (Value.IsString()) {
    const std::string_view Text = Value.AsString();
    lua_pushlstring(L, Text.data(), Text.size());
    return 1;
  }

  lua_pushnil(L);
  return 1;
}

int PushValuePack(lua_State *L, const ValuePack &Pack) {
  int Pushed = 0;
  for (const OwnedValue &Value : Pack.Values())
    Pushed += PushOwnedValue(L, Value);
  return Pushed;
}

} // namespace detail

// ---------------------------------------------------------------------------
// ArgumentPack
// ---------------------------------------------------------------------------

OwnedValue ArgumentPack::At(std::size_t Index) const {
  if (!VM_ || Index >= Size())
    return OwnedValue();

  auto *L = static_cast<lua_State *>(VM_);
  return detail::ReadOwned(L, First_ + static_cast<int>(Index), 0);
}

Value ArgumentPack::View(std::size_t Index) const {
  Value Result;
  if (!VM_ || Index >= Size())
    return Result;

  auto *L = static_cast<lua_State *>(VM_);
  const int Absolute = First_ + static_cast<int>(Index);

  switch (lua_type(L, Absolute)) {
  case LUA_TNIL:
    Result.Kind = ValueCategory::Nil;
    break;
  case LUA_TBOOLEAN:
    Result.Kind = ValueCategory::Boolean;
    Result.Flag = lua_toboolean(L, Absolute) != 0;
    break;
  case LUA_TNUMBER:
    Result.Kind = ValueCategory::Number;
    Result.Number_ = lua_tonumber(L, Absolute);
    break;
  case LUA_TSTRING: {
    std::size_t Length = 0;
    const char *Text = lua_tolstring(L, Absolute, &Length);
    Result.Kind = ValueCategory::String;
    Result.Text = std::string_view(Text, Length);
    break;
  }
  case LUA_TTABLE:
    Result.Kind = ValueCategory::Table;
    break;
  case LUA_TFUNCTION:
    Result.Kind = ValueCategory::Function;
    break;
  case LUA_TUSERDATA:
    Result.Kind = ValueCategory::Userdata;
    break;
  case LUA_TTHREAD:
    Result.Kind = ValueCategory::Thread;
    break;
  default:
    break;
  }
  return Result;
}

namespace detail {

// Stores the value on top of the stack at Table[Name]. lua_setfield pops it, so
// the table index is resolved first.
bool SetTableField(lua_State *L, int TableIndex, const char *Name) {
  const int Absolute = TableIndex < 0 ? lua_gettop(L) + TableIndex + 1
                                      : TableIndex;
  if (!lua_istable(L, Absolute) || !Name)
    return false;
  lua_setfield(L, Absolute, Name);
  return true;
}

} // namespace detail
} // namespace luna
