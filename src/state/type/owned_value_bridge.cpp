// clang-format off
#include "state/type/owned_value_bridge.hpp"

#include <luna/binding/class_construction.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/invocation/parameters/vm_userdata_capture.hpp"
#include "state/type/type_generation.hpp"
#include "state/userdata/access.hpp"
#include "state/userdata/class_registry.hpp"
#include "state/userdata/construction.hpp"
#include "state/userdata/exposure.hpp"
#include "state/userdata/header.hpp"
#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>

#include <cstddef>
#include <memory>
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

[[nodiscard]] CapturedUserdataIdentity
CapturedUserdataIdentityOf(lua_State *State, int StackIndex) {
  const void *Block = lua_touserdata(State, StackIndex);
  const auto ByteCount =
      static_cast<std::size_t>(lua_objlen(State, StackIndex));
  const UserdataHeader *Header = InspectUserdataHeader(Block, ByteCount);
  if (Header == nullptr)
    return CapturedUserdataIdentity();

  CapturedUserdataIdentity Described;
  Described.CapturedType = Header->DynamicType;

  const UserdataAccessContext *Context = ObserveUserdataAccessContext(State);
  if (Context) {
    Described.Origin = Context->Origin;
    Described.HandleProbe = Context->HandleProbe;
  }

  const RegisteredClass *Registered =
      Context && Context->Classes ? Context->Classes->Find(Header->DynamicType)
                                  : nullptr;
  if (Registered)
    Described.ClassName = Registered->QualifiedName;
  return Described;
}

[[nodiscard]] std::string CapturedUserdataDisplayText(lua_State *State,
                                                      int StackIndex) {
  if (!lua_checkstack(State, 4))
    return std::string();

  const int ValueIndex = AbsoluteIndex(State, StackIndex);
  StackCheckpoint Checkpoint(State);

  if (lua_getmetatable(State, ValueIndex) == 0)
    return std::string();

  const int MetaIndex = lua_gettop(State);
  lua_rawgetfield(State, MetaIndex, "__tostring");
  if (!lua_isfunction(State, -1))
    return std::string();

  lua_pushvalue(State, ValueIndex);
  if (lua_pcall(State, 1, 1, 0) != LUA_OK)
    return std::string();

  std::size_t Length = 0;
  const char *Bytes = lua_tolstring(State, -1, &Length);
  if (Bytes == nullptr)
    return std::string();
  return std::string(Bytes, Length);
}

[[nodiscard]] OwnedValue CaptureUserdata(lua_State *State, int StackIndex) {
  const int ValueIndex = AbsoluteIndex(State, StackIndex);
  CapturedUserdataIdentity Described =
      CapturedUserdataIdentityOf(State, ValueIndex);
  VmUserdataCaptureRegistry *Captures = ObserveUserdataCaptureRegistry(State);
  if (!Captures)
    return OwnedValue();

  std::string DisplayText = CapturedUserdataDisplayText(State, ValueIndex);
  std::string ClassName = Described.ClassName;

  std::shared_ptr<CapturedUserdataTarget> Target =
      Captures->Adopt(State, ValueIndex, std::move(Described));
  if (!Target)
    return OwnedValue();
  return OwnedValue::Userdata(std::move(Target), std::move(ClassName),
                              std::move(DisplayText));
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
      } else if (lua_type(State, KeyIndex) == LUA_TSTRING) {
        std::size_t Length = 0;
        const char *Bytes = lua_tolstring(State, KeyIndex, &Length);
        Table.SetField(std::string_view(Bytes ? Bytes : "", Length),
                       BuildFrom(State, ValueIndex, Depth + 1));
      }
      lua_settop(State, ValueIndex - 2);
    }
    return Table;
  }
  case LUA_TUSERDATA:
    return CaptureUserdata(State, StackIndex);
  default:
    break;
  }
  return OwnedValue();
}

[[nodiscard]] std::string
ClassifyPendingObject(const StableTypeKey &Class,
                      const ConstructedInstance &Produced,
                      const TypeGeneration &Types) {
  if (Class.IsEmpty())
    return "a manufactured class instance names a class that never registered "
           "in this State.";
  if (!Types.IsAvailableForWrite(TypeDescriptor::ForClass(Class)))
    return "a manufactured instance of " + std::string(Class.Text()) +
           " names a class that is unavailable in the captured type registry.";

  switch (Produced.Ownership) {
  case ConstructionOwnership::Borrowed:
    if (Produced.Storage == nullptr)
      return "a borrowed instance of " + std::string(Class.Text()) +
             " carries no object.";
    if (!Produced.Lifetime.IsDeclared())
      return "a borrowed instance of " + std::string(Class.Text()) +
             " declares no lifetime, so Luna cannot state when the object "
             "stops being reachable.";
    return std::string();
  case ConstructionOwnership::Shared:
    if (Produced.Storage == nullptr || !Produced.SharedOwnership)
      return "a shared instance of " + std::string(Class.Text()) +
             " carries no object.";
    return std::string();
  case ConstructionOwnership::LuaOwned:
    if (!Produced.Allocator.DeclaresAllocation() ||
        (!Produced.Construct && !Produced.Allocator.DeclaresConstruction()))
      return "a Lua-owned instance of " + std::string(Class.Text()) +
             " carries no object.";
    return std::string();
  }
  return "a manufactured instance of " + std::string(Class.Text()) +
         " declares no ownership.";
}

[[nodiscard]] std::string ClassifyPending(const OwnedValue &Source,
                                          const TypeGeneration &Types,
                                          int Depth) {
  if (Depth > MaximumBridgeDepth)
    return "a returned value nests deeper than Luna publishes.";

  if (Source.IsPendingInstance()) {
    std::string Refusal = ClassifyPendingObject(
        Source.InstanceClass(), *Source.PendingInstanceObject(), Types);
    if (!Refusal.empty())
      return Refusal;
  }
  for (std::size_t Index = 0; Index < Source.Size(); ++Index) {
    std::string Refusal =
        ClassifyPending(Source.Element(Index), Types, Depth + 1);
    if (!Refusal.empty())
      return Refusal;
  }
  for (std::size_t Index = 0; Index < Source.FieldCount(); ++Index) {
    std::string Refusal = ClassifyPending(Source.Field(Source.FieldName(Index)),
                                          Types, Depth + 1);
    if (!Refusal.empty())
      return Refusal;
  }
  return std::string();
}

[[nodiscard]] std::shared_ptr<const TypeGeneration>
CapturedTypes(lua_State *State) {
  const UserdataAccessContext *Context = ObserveUserdataAccessContext(State);
  if (Context && Context->Types)
    return Context->Types->Capture();
  return TypeGeneration::Foundation();
}

bool PushTo(lua_State *State, const OwnedValue &Source,
            const TypeGeneration &Types, int Depth) {
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
      if (!PushTo(State, Source.Element(Index), Types, Depth + 1)) {
        lua_settop(State, TableIndex - 1);
        return false;
      }
      lua_rawseti(State, TableIndex, static_cast<int>(Index + 1));
    }
    for (std::size_t Index = 0; Index < Source.FieldCount(); ++Index) {
      const std::string_view Name = Source.FieldName(Index);
      if (!PushTo(State, Source.Field(Name), Types, Depth + 1)) {
        lua_settop(State, TableIndex - 1);
        return false;
      }
      lua_setfield(State, TableIndex, std::string(Name).c_str());
    }
    return true;
  }
  case ValueCategory::Userdata: {
    if (Source.IsPendingInstance()) {
      const InstancePublication Published =
          PublishConstructedInstance(State, Types, Source.InstanceClass(),
                                     *Source.PendingInstanceObject());
      return Published.IsSuccess() && Published.PublishedCount == 1;
    }
    const auto &Target = Source.UserdataTarget();
    if (!Target)
      return false;
    return PushCapturedUserdataValue(State, *Target);
  }
  case ValueCategory::Function:
    return false;
  }
  return false;
}

} // namespace

Luna::OwnedValue BuildOwnedValueFromStack(lua_State *State, int StackIndex) {
  return BuildFrom(State, StackIndex, 0);
}

std::string ClassifyPendingInstances(const Luna::OwnedValue &Source,
                                     const TypeGeneration &Types) {
  return ClassifyPending(Source, Types, 0);
}

bool PushOwnedValueToStack(lua_State *State, const Luna::OwnedValue &Source) {
  const std::shared_ptr<const TypeGeneration> Types = CapturedTypes(State);
  if (!Types)
    return false;
  return PushOwnedValueToStack(State, Source, *Types);
}

bool PushOwnedValueToStack(lua_State *State, const Luna::OwnedValue &Source,
                           const TypeGeneration &Types) {
  if (!ClassifyPending(Source, Types, 0).empty())
    return false;
  return PushTo(State, Source, Types, 0);
}

} // namespace Luna::Detail
