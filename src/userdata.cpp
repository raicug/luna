#include "runtime.hpp"

#include <luna/convert.hpp>

#include <lua.h>
#include <lualib.h>

#include <memory>
#include <new>
#include <string>
#include <utility>

namespace luna {
namespace detail {
namespace {
void DestroyObject(void *Memory) {
  auto *Instance = static_cast<Object *>(Memory);
  if (!Instance || Instance->Magic != kObjectMagic)
    return;

  Instance->Magic = 0;
  switch (static_cast<ObjectTag>(Instance->Tag)) {
  case ObjectTag::Owned:
    if (Instance->Destroy)
      Instance->Destroy(Instance->Pointer);
    break;
  case ObjectTag::Shared:
    delete Instance->Shared;
    break;
  case ObjectTag::Borrowed:
    break;
  }
}

Object *NewObject(lua_State *L, TypeId ClassId, ObjectTag Tag) {
  void *Memory = lua_newuserdatadtor(L, sizeof(Object), &DestroyObject);
  auto *Instance = new (Memory) Object{};
  Instance->Magic = kObjectMagic;
  Instance->ClassId = ClassId;
  Instance->Tag = static_cast<std::uint8_t>(Tag);
  Model *World = ModelFromLua(L);
  const ClassInfo *Info = World ? World->Class(ClassId) : nullptr;
  if (Info && Info->MetatableRef != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, Info->MetatableRef);
    lua_setmetatable(L, -2);
  }
  return Instance;
}

const char *NameOfClass(lua_State *L, TypeId ClassId) {
  Model *World = ModelFromLua(L);
  const ClassInfo *Info = World ? World->Class(ClassId) : nullptr;
  return Info ? Info->Name.c_str() : "instance";
}

bool IsUsable(const Object *Instance) {
  if (static_cast<ObjectTag>(Instance->Tag) != ObjectTag::Borrowed ||
      !Instance->Control)
    return true;
  auto *Control = static_cast<LifetimeHandle::Control *>(Instance->Control);
  return Control->Alive.load(std::memory_order_acquire);
}
} // namespace

bool IsObjectUsable(const Object *Instance) noexcept {
  return Instance && IsUsable(Instance);
}

std::shared_ptr<Liveness> LivenessOf(void *VM) noexcept {
  Runtime *R = RuntimeFromLua(static_cast<lua_State *>(VM));
  return R ? R->Token : nullptr;
}

TypeId ClassIdForToken(lua_State *L, const void *Token) noexcept {
  Model *World = ModelFromLua(L);
  if (!World || !Token)
    return NoType;
  return World->ClassIdForToken(Token);
}

bool ReadUserdataPointer(lua_State *L, int Index, TypeId ClassId, void **Out,
                         std::string &Why) {
  if (lua_type(L, Index) != LUA_TUSERDATA) {
    Why = "expected ";
    Why += ClassId == NoType ? "an instance" : NameOfClass(L, ClassId);
    Why += ", got ";
    Why += LuaTypeName(L, Index);
    return false;
  }

  auto *Instance = static_cast<Object *>(lua_touserdata(L, Index));
  if (!Instance || Instance->Magic != kObjectMagic) {
    Why = "expected an instance, got a foreign userdata";
    return false;
  }

  if (!IsUsable(Instance)) {
    Why = "the instance is no longer available";
    return false;
  }

  if (ClassId == NoType) {
    Why = "the expected class is not registered";
    return false;
  }

  if (Instance->ClassId != ClassId) {
    Why = "expected ";
    Why += NameOfClass(L, ClassId);
    Why += ", got ";
    Why += NameOfClass(L, Instance->ClassId);
    return false;
  }

  *Out = Instance->Pointer;
  return true;
}

bool ReadBasePointer(lua_State *L, int Index, TypeId ClassId,
                     void **Out) noexcept {
  if (lua_type(L, Index) != LUA_TUSERDATA)
    return false;

  auto *Instance = static_cast<Object *>(lua_touserdata(L, Index));
  if (!Instance || Instance->Magic != kObjectMagic || !IsUsable(Instance))
    return false;

  Model *World = ModelFromLua(L);
  if (!World)
    return false;

  void *Pointer = Instance->Pointer;
  if (!World->RebaseTo(Instance->ClassId, ClassId, Pointer))
    return false;

  *Out = Pointer;
  return true;
}

bool StoreSharedPointer(lua_State *L, int Index, TypeId ClassId,
                        std::shared_ptr<void> &Out, std::string &Why) {
  if (lua_type(L, Index) != LUA_TUSERDATA) {
    Why = "expected ";
    Why += ClassId == NoType ? "an instance" : NameOfClass(L, ClassId);
    Why += ", got ";
    Why += LuaTypeName(L, Index);
    return false;
  }

  auto *Instance = static_cast<Object *>(lua_touserdata(L, Index));
  if (!Instance || Instance->Magic != kObjectMagic) {
    Why = "expected an instance, got a foreign userdata";
    return false;
  }

  if (!IsUsable(Instance)) {
    Why = "the instance is no longer available";
    return false;
  }

  if (ClassId == NoType) {
    Why = "the expected class is not registered";
    return false;
  }

  void *Pointer = Instance->Pointer;
  Model *World = ModelFromLua(L);
  if (!World || !World->RebaseTo(Instance->ClassId, ClassId, Pointer)) {
    Why = "expected ";
    Why += NameOfClass(L, ClassId);
    Why += ", got ";
    Why += NameOfClass(L, Instance->ClassId);
    return false;
  }

  if (static_cast<ObjectTag>(Instance->Tag) != ObjectTag::Shared ||
      !Instance->Shared) {
    Why = "the instance is not shared";
    return false;
  }

  // Aliasing keeps the control block while pointing at the requested subobject.
  Out = std::shared_ptr<void>(*Instance->Shared, Pointer);
  return true;
}

bool PushBorrowedInstance(lua_State *L, const void *TypeToken, void *Subject,
                          const void *LifetimeControl) {
  Model *World = ModelFromLua(L);
  if (!World || !Subject)
    return false;

  const TypeId ClassId = World->ClassIdForToken(TypeToken);
  if (ClassId == NoType)
    return false;

  // A pointer that arrives with no handle of its own takes its class's, if the
  // class was registered with one.
  const ClassInfo *Info = World->Class(ClassId);
  void *Control = const_cast<void *>(LifetimeControl);
  if (!Control && Info && Info->Lifetime)
    Control = Info->Lifetime.get();

  Object *Instance = NewObject(L, ClassId, ObjectTag::Borrowed);
  Instance->Pointer = Subject;
  Instance->Control = Control;
  return true;
}

bool PushSharedInstance(lua_State *L, const void *TypeToken,
                        std::shared_ptr<void> Subject) {
  Model *World = ModelFromLua(L);
  if (!World || !Subject)
    return false;

  const TypeId ClassId = World->ClassIdForToken(TypeToken);
  if (ClassId == NoType)
    return false;

  // Owned until the userdata holds it, because making the userdata can throw.
  std::unique_ptr<std::shared_ptr<void>> Storage(
      new std::shared_ptr<void>(std::move(Subject)));
  Object *Instance = NewObject(L, ClassId, ObjectTag::Shared);
  Instance->Pointer = Storage->get();
  Instance->Shared = Storage.release();
  return true;
}

bool PushOwnedInstance(lua_State *L, const void *TypeToken, void *Subject,
                       void (*Destroy)(void *)) {
  Model *World = ModelFromLua(L);
  if (!World || !Subject)
    return false;

  const TypeId ClassId = World->ClassIdForToken(TypeToken);
  if (ClassId == NoType)
    return false;

  Object *Instance = NewObject(L, ClassId, ObjectTag::Owned);
  Instance->Pointer = Subject;
  Instance->Destroy = Destroy;
  return true;
}

bool PushInstanceByToken(lua_State *L, const void *TypeToken,
                         std::shared_ptr<void> Subject) {
  return PushSharedInstance(L, TypeToken, std::move(Subject));
}

namespace {
InvokeThunk DirectThunk(lua_State *L, int Index) {
  if (lua_type(L, Index) != LUA_TUSERDATA)
    return nullptr;
  const auto *Header =
      static_cast<const CallableHeader *>(lua_touserdata(L, Index));
  return Header ? Header->Thunk : nullptr;
}

int IndexFallback(lua_State *L) {
  if (!lua_isnil(L, lua_upvalueindex(3))) {
    lua_pushvalue(L, lua_upvalueindex(3));
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_call(L, 2, 1);
    return 1;
  }

  if (lua_type(L, 2) == LUA_TSTRING) {
    const Runtime *R = RuntimeFromLua(L);
    if (R && R->Settings.StrictMembers)
      luaL_error(L, "%s is not a valid member of %s", lua_tostring(L, 2),
                 lua_tostring(L, lua_upvalueindex(2)));
  }
  lua_pushnil(L);
  return 1;
}

bool NewIndexFallback(lua_State *L) {
  if (lua_isnil(L, lua_upvalueindex(3)))
    return false;

  lua_pushvalue(L, lua_upvalueindex(3));
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_call(L, 3, 0);
  return true;
}
} // namespace

int ClassIndex(lua_State *L) {
  if (lua_type(L, 2) != LUA_TSTRING)
    return IndexFallback(L);

  lua_pushvalue(L, 2);
  lua_rawget(L, lua_upvalueindex(1));
  if (lua_isfunction(L, -1))
    return 1;

  if (const InvokeThunk Thunk = DirectThunk(L, -1)) {
    void *Context = lua_touserdata(L, -1);
    lua_pop(L, 1);
    return Thunk(L, Context);
  }

  lua_pop(L, 1);
  return IndexFallback(L);
}

int ClassNewIndex(lua_State *L) {
  if (lua_type(L, 2) != LUA_TSTRING) {
    if (NewIndexFallback(L))
      return 0;

    luaL_error(L, "%s is not a writable member of %s", luaL_typename(L, 2),
               lua_tostring(L, lua_upvalueindex(2)));
  }

  const char *Key = lua_tostring(L, 2);
  lua_pushvalue(L, 2);
  lua_rawget(L, lua_upvalueindex(1));
  if (const InvokeThunk Thunk = DirectThunk(L, -1)) {
    void *Context = lua_touserdata(L, -1);
    lua_pop(L, 1);
    lua_remove(L, 2);
    return Thunk(L, Context);
  }

  if (lua_isfunction(L, -1)) {
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 3);
    lua_call(L, 2, 0);
    return 0;
  }
  lua_pop(L, 1);
  if (NewIndexFallback(L))
    return 0;

  luaL_error(L, "%s is not a writable member of %s", Key,
             lua_tostring(L, lua_upvalueindex(2)));
}

// instance:IsA("Name") walks the base chain of the instance's own class.
int ClassIsA(lua_State *L) {
  if (lua_type(L, 1) != LUA_TUSERDATA || lua_type(L, 2) != LUA_TSTRING) {
    luaL_error(L, "IsA expects an instance and a class name");
  }

  auto *Instance = static_cast<Object *>(lua_touserdata(L, 1));
  if (!Instance || Instance->Magic != kObjectMagic) {
    luaL_error(L, "IsA expects an instance");
  }

  Model *World = ModelFromLua(L);
  if (!World) {
    lua_pushboolean(L, 0);
    return 1;
  }

  std::size_t Length = 0;
  const char *Name = lua_tolstring(L, 2, &Length);
  const std::string_view Wanted(Name ? Name : "", Length);
  lua_pushboolean(L, World->DerivesFrom(Instance->ClassId, Wanted));
  return 1;
}

int ClassIter(lua_State *L) {
  lua_pushvalue(L, lua_upvalueindex(1));
  luna::detail::PushClosure(L, &ClassIterStep, nullptr, 1);
  lua_pushvalue(L, 1);
  lua_pushinteger(L, 0);
  return 3;
}

int ClassIterStep(lua_State *L) {
  const int Control =
      lua_type(L, 2) == LUA_TNUMBER ? static_cast<int>(lua_tointeger(L, 2)) : 0;

  lua_pushvalue(L, lua_upvalueindex(1));
  lua_pushvalue(L, 1);
  lua_pushinteger(L, Control);
  lua_call(L, 2, 1);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return 0;
  }

  lua_pushinteger(L, Control + 1);
  lua_insert(L, -2);
  return 2;
}
} // namespace detail
} // namespace luna
