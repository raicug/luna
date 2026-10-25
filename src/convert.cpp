#include "runtime.hpp"

#include <luna/convert.hpp>
#include <luna/value.hpp>

#include <lua.h>
#include <lualib.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace luna {
namespace detail {
namespace {
// Tables nest, and a cycle is bounded by this rather than followed.
constexpr int kMaxDepth = 32;

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

  case LUA_TUSERDATA: {
    // Described as it was when read. Only an object the host shares is kept
    // alive, because only that one can go back into Lua.
    auto *Instance = static_cast<Object *>(lua_touserdata(L, Index));
    if (!Instance || Instance->Magic != kObjectMagic)
      return OwnedValue::FromOpaque(ValueCategory::Userdata);

    Model *World = ModelFromLua(L);
    const ClassInfo *Info = World ? World->Class(Instance->ClassId) : nullptr;
    OwnedValue::InstanceRef Described;
    Described.Live = IsObjectUsable(Instance);
    Described.Pointer = Instance->Pointer;
    if (Info) {
      Described.ClassName = Info->Name;
      Described.TypeToken = Info->Token;
    }
    if (Described.Live && luaL_callmeta(L, Index, "__tostring")) {
      if (const char *Text = lua_tostring(L, -1))
        Described.Text = Text;
      lua_pop(L, 1);
    }
    if (Described.Live && Info &&
        static_cast<ObjectTag>(Instance->Tag) == ObjectTag::Shared &&
        Instance->Shared)
      Described.Object = *Instance->Shared;
    return OwnedValue::FromUserdata(std::move(Described));
  }

  case LUA_TFUNCTION:
    return OwnedValue::FromOpaque(ValueCategory::Function);

  case LUA_TTHREAD:
    return OwnedValue::FromOpaque(ValueCategory::Thread);

  case LUA_TTABLE: {
    OwnedValue Result = OwnedValue::Table();
    if (Depth >= kMaxDepth)
      return Result;

    const int Absolute = Index < 0 && Index > LUA_REGISTRYINDEX
                             ? lua_gettop(L) + Index + 1
                             : Index;

    // The sequence keeps its positions, holes included.
    const int Length = static_cast<int>(lua_objlen(L, Absolute));
    for (int Position = 1; Position <= Length; ++Position) {
      lua_rawgeti(L, Absolute, Position);
      Result.Append(ReadOwned(L, lua_gettop(L), Depth + 1));
      lua_pop(L, 1);
    }

    // Everything else is a named field. A number outside the sequence keeps its
    // value as text, rather than being appended in whatever order lua_next
    // gives.
    lua_pushnil(L);
    while (lua_next(L, Absolute) != 0) {
      const int ValueIndex = lua_gettop(L);
      const int KeyIndex = ValueIndex - 1;
      if (lua_type(L, KeyIndex) == LUA_TSTRING) {
        std::size_t KeyLength = 0;
        const char *Text = lua_tolstring(L, KeyIndex, &KeyLength);
        Result.SetField(std::string_view(Text, KeyLength),
                        ReadOwned(L, ValueIndex, Depth + 1));
      } else if (lua_type(L, KeyIndex) == LUA_TNUMBER) {
        const double Key = lua_tonumber(L, KeyIndex);
        const bool InSequence =
            Key >= 1.0 && Key <= Length && std::floor(Key) == Key;
        if (!InSequence) {
          char Buffer[40];
          std::snprintf(Buffer, sizeof(Buffer), "%.14g", Key);
          Result.SetField(Buffer, ReadOwned(L, ValueIndex, Depth + 1));
        }
      }

      lua_pop(L, 1);
    }
    return Result;
  }

  default:
    return OwnedValue();
  }
}
} // namespace
} // namespace detail

OwnedValue OwnedValue::Table() {
  OwnedValue Result;
  auto Table = std::make_shared<OwnedTable>();
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

namespace detail {
int PushOwnedValue(lua_State *L, const OwnedValue &Value) {
  if (const auto *Instance = Value.AsInstance()) {
    // A userdata the host does not share has no way back into Lua.
    if (!Instance->Object || !Instance->TypeToken) {
      lua_pushnil(L);
      return 1;
    }
    // Shares the object with the VM, so the userdata cannot outlive its owner.
    if (PushSharedInstance(L, Instance->TypeToken, Instance->Object))
      return 1;
    throw MarshalError("an instance of a class that is not registered cannot "
                       "be passed to Lua");
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
bool SetTableField(lua_State *L, int TableIndex, const char *Name) {
  const int Absolute =
      TableIndex < 0 ? lua_gettop(L) + TableIndex + 1 : TableIndex;
  if (!lua_istable(L, Absolute) || !Name)
    return false;
  lua_setfield(L, Absolute, Name);
  return true;
}
} // namespace detail
} // namespace luna

namespace luna {
std::optional<std::string> OwnedValue::ToText() const {
  if (IsString())
    return std::string(AsString());
  if (IsBoolean())
    return std::string(AsBoolean() ? "true" : "false");
  if (IsNumber()) {
    char Buffer[40];
    std::snprintf(Buffer, sizeof(Buffer), "%.14g", AsNumber());
    return std::string(Buffer);
  }
  return std::nullopt;
}

std::size_t OwnedValue::Size() const noexcept {
  const OwnedTable *Table = AsTable();
  return Table ? Table->Items.size() : 0;
}

std::size_t OwnedValue::FieldCount() const noexcept {
  const OwnedTable *Table = AsTable();
  return Table ? Table->Fields.size() : 0;
}

const OwnedValue &OwnedValue::Element(std::size_t Index) const noexcept {
  static const OwnedValue Nil;
  const OwnedTable *Table = AsTable();
  return Table && Index < Table->Items.size() ? Table->Items[Index] : Nil;
}

std::string_view OwnedValue::FieldName(std::size_t Index) const noexcept {
  const OwnedTable *Table = AsTable();
  return Table && Index < Table->Fields.size()
             ? std::string_view(Table->Fields[Index].Key)
             : std::string_view();
}

const OwnedValue &OwnedValue::Field(std::string_view Name) const noexcept {
  static const OwnedValue Nil;
  if (const OwnedTable *Table = AsTable()) {
    for (const OwnedTable::Entry &Entry : Table->Fields) {
      if (Entry.Key == Name)
        return Entry.Value;
    }
  }
  return Nil;
}
} // namespace luna
