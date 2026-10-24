#include "runtime.hpp"

#include <luna/convert.hpp>

#include <lua.h>
#include <lualib.h>

#include <new>
#include <string>
#include <utility>

namespace luna {
namespace detail {

// ---------------------------------------------------------------------------
// Instance objects
// ---------------------------------------------------------------------------

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

// Allocates the userdata and attaches the class metatable, leaving it on top.
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

} // namespace

TypeId ClassIdForToken(lua_State *L, const void *Token) noexcept {
  Model *World = ModelFromLua(L);
  if (!World || !Token)
    return NoType;
  return World->ClassIdForToken(Token);
}

bool ReadUserdataPointer(lua_State *L, int Index, TypeId ClassId, void **Out,
                         std::string &Why) {
  if (lua_type(L, Index) != LUA_TUSERDATA) {
    Why = "expected an instance, got ";
    Why += LuaTypeName(L, Index);
    return false;
  }

  auto *Instance = static_cast<Object *>(lua_touserdata(L, Index));
  if (!Instance || Instance->Magic != kObjectMagic) {
    Why = "expected an instance, got a foreign userdata";
    return false;
  }

  if (ClassId != NoType && Instance->ClassId != ClassId) {
    Why = "expected ";
    Why += NameOfClass(L, ClassId);
    Why += ", got ";
    Why += NameOfClass(L, Instance->ClassId);
    return false;
  }

  if (static_cast<ObjectTag>(Instance->Tag) == ObjectTag::Borrowed &&
      Instance->Control) {
    auto *Control = static_cast<LifetimeHandle::Control *>(Instance->Control);
    if (!Control->Alive.load(std::memory_order_acquire)) {
      Why = "the instance is no longer available";
      return false;
    }
  }

  *Out = Instance->Pointer;
  return true;
}

bool StoreSharedPointer(lua_State *L, int Index, TypeId ClassId,
                        std::shared_ptr<void> &Out, std::string &Why) {
  if (lua_type(L, Index) != LUA_TUSERDATA) {
    Why = "expected an instance, got ";
    Why += LuaTypeName(L, Index);
    return false;
  }

  auto *Instance = static_cast<Object *>(lua_touserdata(L, Index));
  if (!Instance || Instance->Magic != kObjectMagic) {
    Why = "expected an instance, got a foreign userdata";
    return false;
  }

  if (ClassId != NoType && Instance->ClassId != ClassId) {
    Why = "expected ";
    Why += NameOfClass(L, ClassId);
    return false;
  }

  // Shared and owned payloads own themselves; a borrowed one is kept alive by
  // the host's lifetime handle, so a shared_ptr would be a lie.
  if (static_cast<ObjectTag>(Instance->Tag) == ObjectTag::Shared &&
      Instance->Shared) {
    Out = *Instance->Shared;
    return true;
  }

  Why = "the instance is not shared";
  return false;
}

bool PushBorrowedInstance(lua_State *L, const void *TypeToken, void *Subject,
                          const void *LifetimeControl) {
  Model *World = ModelFromLua(L);
  if (!World || !Subject)
    return false;

  const TypeId ClassId = World->ClassIdForToken(TypeToken);
  if (ClassId == NoType)
    return false;

  Object *Instance = NewObject(L, ClassId, ObjectTag::Borrowed);
  Instance->Pointer = Subject;
  Instance->Control = const_cast<void *>(LifetimeControl);
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

  auto *Storage = new std::shared_ptr<void>(std::move(Subject));
  Object *Instance = NewObject(L, ClassId, ObjectTag::Shared);
  Instance->Shared = Storage;
  Instance->Pointer = Storage->get();
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

// Also used for OwnedValue::Instance, which carries a token rather than a type.
bool PushInstanceByToken(lua_State *L, const void *TypeToken,
                         std::shared_ptr<void> Subject) {
  return PushSharedInstance(L, TypeToken, std::move(Subject));
}

// ---------------------------------------------------------------------------
// Metamethods
// ---------------------------------------------------------------------------

// A resolve-table entry is either a closure (a method: returned as-is) or a
// callable context whose first word is its dispatch thunk (a property or field:
// invoked in place).
namespace {

// Returns the entry's thunk when it can be invoked directly, else nullptr.
InvokeThunk DirectThunk(lua_State *L, int Index) {
  if (lua_type(L, Index) != LUA_TUSERDATA)
    return nullptr;
  const auto *Header =
      static_cast<const CallableHeader *>(lua_touserdata(L, Index));
  return Header ? Header->Thunk : nullptr;
}

// __index upvalue 3: fn(receiver, key) -> value, or nil when the class declared
// no ClassOperator::Index.
int IndexFallback(lua_State *L) {
  if (!lua_isnil(L, lua_upvalueindex(3))) {
    lua_pushvalue(L, lua_upvalueindex(3));
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_call(L, 2, 1);
    return 1;
  }
  lua_pushnil(L);
  return 1;
}

// __newindex upvalue 3: fn(receiver, key, value). True when it ran.
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

// __index(obj, key)
//   upvalue 1: resolve table   upvalue 2: class name
//   upvalue 3: index fallback closure or nil
int ClassIndex(lua_State *L) {
  // Only a string can name a member, but a non-string key is still the class's
  // business: a container binds ClassOperator::Index to answer scene[1].
  if (lua_type(L, 2) != LUA_TSTRING)
    return IndexFallback(L);

  lua_pushvalue(L, 2);
  lua_rawget(L, lua_upvalueindex(1));

  if (lua_isfunction(L, -1))
    return 1; // a method is returned as-is: obj:M() compiles to obj.M(obj)

  if (const InvokeThunk Thunk = DirectThunk(L, -1)) {
    void *Context = lua_touserdata(L, -1);
    lua_pop(L, 1);
    // The receiver is already argument 1 of this call, so the getter runs in
    // place: no closure, no nested lua_call.
    return Thunk(L, Context);
  }

  lua_pop(L, 1);
  return IndexFallback(L);
}

// __newindex(obj, key, value)
//   upvalue 1: setters table   upvalue 2: class name
//   upvalue 3: newindex fallback closure or nil
int ClassNewIndex(lua_State *L) {
  // A non-string key cannot name a member, so only the class's own
  // ClassOperator::NewIndex can give the write a meaning.
  if (lua_type(L, 2) != LUA_TSTRING) {
    if (NewIndexFallback(L))
      return 0;
    // luaL_error is noreturn in this fork, so neither site needs a trailing
    // return and the compiler knows the function cannot fall through.
    luaL_error(L, "%s is not a writable member of %s", luaL_typename(L, 2),
               lua_tostring(L, lua_upvalueindex(2)));
  }

  const char *Key = lua_tostring(L, 2);

  lua_pushvalue(L, 2);
  lua_rawget(L, lua_upvalueindex(1));

  if (const InvokeThunk Thunk = DirectThunk(L, -1)) {
    void *Context = lua_touserdata(L, -1);
    lua_pop(L, 1);
    // Drop the key so the layout is (receiver, value), which is what a setter
    // reads as arguments 1 and 2.
    lua_remove(L, 2);
    return Thunk(L, Context);
  }

  if (lua_isfunction(L, -1)) {
    lua_pushvalue(L, 1); // receiver
    lua_pushvalue(L, 3); // value
    lua_call(L, 2, 0);
    return 0;
  }
  lua_pop(L, 1);

  // A ClassOperator::NewIndex fallback decides what a write means.
  if (NewIndexFallback(L))
    return 0;

  // Nothing is bound under this name and there is no fallback, so the write has
  // no meaning. Saying so beats silently dropping it.
  luaL_error(L, "%s is not a writable member of %s", Key,
             lua_tostring(L, lua_upvalueindex(2)));
}

// __iter(obj): returns the triple a generic for needs -- the stepping closure,
// the state it is called with, and the initial control.
//   upvalue 1: iterate target closure
int ClassIter(lua_State *L) {
  lua_pushvalue(L, lua_upvalueindex(1));
  luna::detail::PushClosure(L, &ClassIterStep, nullptr, 1);
  lua_pushvalue(L, 1);   // state: the receiver
  lua_pushinteger(L, 0); // control
  return 3;
}

// step(state, control) -> control, value   (or nothing when exhausted)
//   upvalue 1: iterate target
int ClassIterStep(lua_State *L) {
  const int Control =
      lua_type(L, 2) == LUA_TNUMBER ? static_cast<int>(lua_tointeger(L, 2)) : 0;

  lua_pushvalue(L, lua_upvalueindex(1)); // target
  lua_pushvalue(L, 1);                   // receiver
  lua_pushinteger(L, Control);
  lua_call(L, 2, 1); // target(receiver, control) -> value, or nothing

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
