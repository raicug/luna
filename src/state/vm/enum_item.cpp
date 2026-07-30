// clang-format off
#include "state/vm/enum_item.hpp"

#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>

#include <cstddef>
#include <cstring>
#include <string>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr const char *EnumItemRegistrySlot = "Luna.EnumItems";

constexpr const char *ProtectedMetatableMarker = "Luna.ProtectedMetatable";

[[nodiscard]] int RejectEnumItemWrite(lua_State *State) {
  const char *Named = lua_tostring(State, lua_upvalueindex(1));
  std::string Refusal = "Luna: '";
  Refusal += Named != nullptr ? Named : "an enumerator";
  Refusal += "' is an immutable enumerator object.";
  lua_pushlstring(State, Refusal.data(), Refusal.size());
  lua_error(State);
  return 0;
}

[[nodiscard]] int DescribeEnumItem(lua_State *State) {
  lua_pushvalue(State, lua_upvalueindex(1));
  return 1;
}

[[nodiscard]] bool PushEnumItem(lua_State *State, const TypeId &Enumeration,
                                std::int64_t Numeric,
                                std::string_view EnumerationName,
                                std::string_view Name) {
  if (!lua_checkstack(State, 8))
    return false;

  void *Block = lua_newuserdata(State, sizeof(EnumItemPayload));
  if (Block == nullptr)
    return false;

  EnumItemPayload Payload;
  Payload.Enumeration = Enumeration;
  Payload.Numeric = Numeric;
  std::memcpy(Block, &Payload, sizeof(Payload));

  const int Item = lua_gettop(State);

  std::string Qualified(EnumerationName);
  Qualified += ".";
  Qualified += std::string(Name);

  lua_newtable(State);
  const int Fields = lua_gettop(State);
  lua_pushlstring(State, Name.data(), Name.size());
  lua_rawsetfield(State, Fields, "Name");
  lua_pushinteger(State, static_cast<int>(Numeric));
  lua_rawsetfield(State, Fields, "Value");
  lua_pushlstring(State, EnumerationName.data(), EnumerationName.size());
  lua_rawsetfield(State, Fields, "EnumName");
  lua_setreadonly(State, Fields, 1);

  lua_newtable(State);
  const int Meta = lua_gettop(State);
  lua_pushvalue(State, Fields);
  lua_rawsetfield(State, Meta, "__index");
  lua_pushlstring(State, Qualified.data(), Qualified.size());
  lua_pushcclosure(State, RejectEnumItemWrite, "Luna.EnumItemAssign", 1);
  lua_rawsetfield(State, Meta, "__newindex");
  lua_pushlstring(State, Qualified.data(), Qualified.size());
  lua_pushcclosure(State, DescribeEnumItem, "Luna.EnumItemToText", 1);
  lua_rawsetfield(State, Meta, "__tostring");
  lua_pushlstring(State, EnumItemTypeName.data(), EnumItemTypeName.size());
  lua_rawsetfield(State, Meta, "__type");
  lua_pushstring(State, ProtectedMetatableMarker);
  lua_rawsetfield(State, Meta, "__metatable");
  lua_setreadonly(State, Meta, 1);

  lua_setmetatable(State, Item);
  lua_remove(State, Fields);
  return true;
}

} // namespace

const EnumItemPayload *InspectEnumItem(const void *Block,
                                       std::size_t ByteCount) noexcept {
  if (Block == nullptr || ByteCount != sizeof(EnumItemPayload))
    return nullptr;
  const auto *Payload = static_cast<const EnumItemPayload *>(Block);
  if (!Payload->HasCanonicalLayout())
    return nullptr;
  return Payload;
}

EnumItemRegistry::~EnumItemRegistry() { Retire(); }

void EnumItemRegistry::Bind(lua_State *Root) noexcept { Thread = Root; }

const EnumItemRegistry::Interned *
EnumItemRegistry::Find(const TypeId &Enumeration,
                       std::int64_t Numeric) const noexcept {
  for (const Interned &Held : Items) {
    if (Held.Enumeration == Enumeration && Held.Numeric == Numeric)
      return &Held;
  }
  return nullptr;
}

bool EnumItemRegistry::Publish(lua_State *State, const TypeId &Enumeration,
                               std::int64_t Numeric,
                               std::string_view EnumerationName,
                               std::string_view Name) {
  if (State == nullptr || !Enumeration.IsValid())
    return false;

  if (const Interned *Held = Find(Enumeration, Numeric)) {
    if (!lua_checkstack(State, 2))
      return false;
    lua_getref(State, Held->Reference);
    if (lua_type(State, -1) == LUA_TUSERDATA)
      return true;
    lua_pop(State, 1);
    return false;
  }

  if (Name.empty())
    return false;

  const int EntryDepth = lua_gettop(State);
  if (!PushEnumItem(State, Enumeration, Numeric, EnumerationName, Name)) {
    lua_settop(State, EntryDepth);
    return false;
  }

  lua_pushvalue(State, -1);
  const int Reference = lua_ref(State, -1);
  lua_pop(State, 1);
  if (Reference <= 0) {
    lua_settop(State, EntryDepth);
    return false;
  }

  Interned Held;
  Held.Enumeration = Enumeration;
  Held.Numeric = Numeric;
  Held.Reference = Reference;
  Items.push_back(Held);
  return true;
}

void EnumItemRegistry::Retire() noexcept {
  if (Thread != nullptr) {
    for (const Interned &Held : Items)
      lua_unref(Thread, Held.Reference);
  }
  Items.clear();
  Thread = nullptr;
}

bool PublishEnumItemRegistry(lua_State *State,
                             EnumItemRegistry *Registry) noexcept {
  if (State == nullptr || Registry == nullptr)
    return false;
  if (!lua_checkstack(State, 4))
    return false;

  StackCheckpoint Checkpoint(State);
  lua_pushlightuserdata(State, Registry);
  lua_rawsetfield(State, LUA_REGISTRYINDEX, EnumItemRegistrySlot);
  return true;
}

EnumItemRegistry *ObserveEnumItemRegistry(lua_State *State) noexcept {
  if (State == nullptr || !lua_checkstack(State, 2))
    return nullptr;

  StackCheckpoint Checkpoint(State);
  lua_rawgetfield(State, LUA_REGISTRYINDEX, EnumItemRegistrySlot);
  return static_cast<EnumItemRegistry *>(lua_tolightuserdata(State, -1));
}

} // namespace Luna::Detail
