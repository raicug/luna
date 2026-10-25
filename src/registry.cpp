#include "runtime.hpp"

#include <luna/bound.hpp>
#include <luna/state.hpp>

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace luna {
namespace detail {
namespace {
std::size_t LastDot(std::string_view Name) {
  return Name.rfind('.');
}

std::string_view ParentOf(std::string_view Qualified) {
  const std::size_t Dot = LastDot(Qualified);
  return Dot == std::string_view::npos ? std::string_view()
                                       : Qualified.substr(0, Dot);
}

std::string_view LeafOf(std::string_view Qualified) {
  const std::size_t Dot = LastDot(Qualified);
  return Dot == std::string_view::npos ? Qualified : Qualified.substr(Dot + 1);
}

bool ValidName(std::string_view Name) {
  if (Name.empty())
    return false;
  if (Name.front() == '.' || Name.back() == '.')
    return false;
  return Name.find("..") == std::string_view::npos;
}

const char *MetamethodName(OperatorSlot Slot) {
  switch (Slot) {
  case OperatorSlot::Add:
    return "__add";
  case OperatorSlot::Subtract:
    return "__sub";
  case OperatorSlot::Multiply:
    return "__mul";
  case OperatorSlot::Divide:
    return "__div";
  case OperatorSlot::Modulo:
    return "__mod";
  case OperatorSlot::Power:
    return "__pow";
  case OperatorSlot::Negate:
    return "__unm";
  case OperatorSlot::Equal:
    return "__eq";
  case OperatorSlot::Less:
    return "__lt";
  case OperatorSlot::LessOrEqual:
    return "__le";
  case OperatorSlot::Concat:
    return "__concat";
  case OperatorSlot::Len:
    return "__len";
  case OperatorSlot::ToString:
    return "__tostring";
  case OperatorSlot::Call:
    return "__call";
  case OperatorSlot::Iterate:
    return "__iter";
  case OperatorSlot::Index:
    return "__index";
  case OperatorSlot::NewIndex:
    return "__newindex";
  }
  return "__index";
}

void PushRef(lua_State *L, int Ref) {
  lua_rawgeti(L, LUA_REGISTRYINDEX, Ref);
}

// Sets TableRef[Name] to the value on top. What was there is remembered first,
// so a commit that fails later can put it back.
void SetFieldFromTop(Model &World, lua_State *L, int TableRef,
                     std::string_view Name) {
  const std::string Key(Name);
  PushRef(L, TableRef);
  World.RememberSlot(lua_gettop(L), Key);
  lua_insert(L, -2);
  lua_setfield(L, -2, Key.c_str());
  lua_pop(L, 1);
}

void SetResolvable(Model &World, lua_State *L, int TableRef,
                   std::string_view Name, Target &Call) {
  if (Call.Direct())
    Call.Context(L, Call.Ctx.get());
  else
    Call.Push(L, Call.Ctx.get());

  SetFieldFromTop(World, L, TableRef, Name);
}

// One candidate of an overload set, holding a thunk rather than a Target so the
// dispatcher never touches a shared_ptr.
struct OverloadSlot {
  InvokeThunk Thunk = nullptr;
  AcceptThunk Accept = nullptr;
};

struct OverloadSet {
  std::string Name;
  std::vector<OverloadSlot> Slots;
  int ArityOffset = 0;
};

void DestroyOverloadSet(void *Memory) {
  static_cast<OverloadSet *>(Memory)->~OverloadSet();
}

int OverloadDispatch(lua_State *L) {
  auto *Set =
      static_cast<OverloadSet *>(lua_touserdata(L, lua_upvalueindex(1)));
  const int Count = static_cast<int>(Set->Slots.size());
  for (int Index = 0; Index < Count; ++Index) {
    void *Context = lua_touserdata(L, lua_upvalueindex(Index + 2));
    const OverloadSlot &Slot = Set->Slots[static_cast<std::size_t>(Index)];
    if (Slot.Accept && !Slot.Accept(L, Context))
      continue;
    return Slot.Thunk(L, Context);
  }

  // A method's receiver is not one of its arguments, so it is not counted here.
  const int Given = lua_gettop(L) - Set->ArityOffset;
  luaL_error(L, "no overload of '%s' accepts %d argument%s", Set->Name.c_str(),
             Given, Given == 1 ? "" : "s");
}

// Pushes the value for a callable and reports whether it now heads an overload
// set. The first registration pushes the callable itself, so a name that is not
// overloaded costs nothing at call time.
bool PushCallableValue(lua_State *L, Model &World, std::string_view Qualified,
                       Target &Call, std::string &Error, int ArityOffset = 0) {
  std::vector<Target> &List = World.CallablesAt(Qualified);
  if (List.empty()) {
    List.push_back(Call);
    Call.Push(L, Call.Ctx.get());
    return false;
  }

  if (!Call.Direct() || !Call.Accept) {
    Error = "cannot overload '";
    Error += std::string(Qualified);
    Error += "': the callable cannot be dispatched in place";
    return false;
  }

  List.push_back(Call);

  void *Memory =
      lua_newuserdatadtor(L, sizeof(OverloadSet), &DestroyOverloadSet);
  auto *Set = new (Memory) OverloadSet();
  Set->Name.assign(Qualified);
  Set->ArityOffset = ArityOffset;
  Set->Slots.reserve(List.size());
  for (Target &Candidate : List)
    Set->Slots.push_back(OverloadSlot{Candidate.Thunk, Candidate.Accept});

  for (Target &Candidate : List)
    Candidate.Context(L, Candidate.Ctx.get());

  luna::detail::PushClosure(L, &OverloadDispatch, nullptr,
                            static_cast<int>(List.size()) + 1);
  return true;
}

void LinkInto(lua_State *L, Model &World, std::string_view Qualified,
              int ValueRef, std::string &Error) {
  const std::string_view Parent = ParentOf(Qualified);
  const std::string_view Leaf = LeafOf(Qualified);
  if (Leaf.empty()) {
    Error = "name has an empty final segment";
    return;
  }

  const int ParentRef = World.EnsureNamespace(Parent, Error);
  if (ParentRef == LUA_NOREF)
    return;

  PushRef(L, ParentRef);
  World.RememberSlot(lua_gettop(L), Leaf);
  PushRef(L, ValueRef);
  lua_setfield(L, -2, std::string(Leaf).c_str());
  lua_pop(L, 1);
}

enum class CopyFilter : std::uint8_t { All, Metamethods };

// Copies entries the destination does not already have, so a derived entry
// wins.
void CopyEntries(Model &World, lua_State *L, int FromRef, int ToRef,
                 CopyFilter Filter) {
  if (FromRef == LUA_NOREF || ToRef == LUA_NOREF)
    return;

  const int Dst = lua_gettop(L) + 1;
  PushRef(L, ToRef);
  const int Src = Dst + 1;
  PushRef(L, FromRef);
  lua_pushnil(L);
  while (lua_next(L, Src) != 0) {
    bool Copy = true;
    if (Filter == CopyFilter::Metamethods) {
      std::size_t Length = 0;
      const char *Key = lua_type(L, -2) == LUA_TSTRING
                            ? lua_tolstring(L, -2, &Length)
                            : nullptr;
      const std::string_view Text(Key ? Key : "", Length);
      // The class's own indexing and identity stay with the derived class.
      Copy = Key != nullptr && Text.rfind("__", 0) == 0 && Text != "__index" &&
             Text != "__newindex" && Text != "__type";
    }

    if (Copy) {
      lua_pushvalue(L, -2);
      lua_rawget(L, Dst);
      const bool Present = !lua_isnil(L, -1);
      lua_pop(L, 1);
      if (!Present) {
        if (lua_type(L, -2) == LUA_TSTRING) {
          std::size_t KeyLength = 0;
          const char *KeyText = lua_tolstring(L, -2, &KeyLength);
          World.RememberSlot(Dst, std::string_view(KeyText, KeyLength));
        }
        lua_pushvalue(L, -2);
        lua_pushvalue(L, -2);
        lua_rawset(L, Dst);
      }
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 2);
}

bool HasField(lua_State *L, int TableRef, std::string_view Name) {
  PushRef(L, TableRef);
  lua_pushlstring(L, Name.data(), Name.size());
  lua_rawget(L, -2);
  const bool Present = !lua_isnil(L, -1);
  lua_pop(L, 2);
  return Present;
}
} // namespace

// Everything that changes the VM and the model. It runs under lua_pcall, so a
// Lua error (a read-only table, running out of memory) comes back as an error
// string, and every write is logged so the caller can undo a partial link.
void LinkBatch(Runtime &R, Batch &Pending, std::string &Error) {
  Model &World = R.World;
  lua_State *L = R.L;

  std::vector<TypeId> ClassIds;
  ClassIds.reserve(Pending.Classes.size());
  for (StagedClass &Entry : Pending.Classes) {
    const std::string Key =
        Entry.StableKey.empty() ? Entry.Name : Entry.StableKey;
    ClassIds.push_back(World.AddClass(Entry.Name, Key, Entry.Token));
  }

  for (std::size_t Index = 0; Index < Pending.Classes.size(); ++Index) {
    StagedClass &Entry = Pending.Classes[Index];
    const TypeId ClassId = ClassIds[Index];
    ClassInfo *Info = World.Class(ClassId);
    if (!Info) {
      Error = "class '";
      Error += Entry.Name;
      Error += "' could not be allocated";
      break;
    }

    if (Entry.Lifetime && !Info->Lifetime) {
      World.RememberLifetimeSet(*Info);
      Info->Lifetime = Entry.Lifetime;
    }

    if (!Entry.BaseName.empty()) {
      ClassInfo *Base = World.FindOrNull(Entry.BaseName);
      if (!Base) {
        Error = "class '";
        Error += Entry.Name;
        Error += "' declares base '";
        Error += Entry.BaseName;
        Error += "', which is not registered before it";
        break;
      }
      if (Entry.BaseToken && Base->Token != Entry.BaseToken) {
        Error = "class '";
        Error += Entry.Name;
        Error += "' declares '";
        Error += Entry.BaseName;
        Error += "' as its base, but that class is a different type";
        break;
      }
      if (Base->MembersRef == LUA_NOREF) {
        Error = "class '";
        Error += Entry.Name;
        Error += "' declares base '";
        Error += Entry.BaseName;
        Error += "', which is declared later in the same commit";
        break;
      }
      World.RememberBase(*Info);
      Info->BaseId = Base->Id;
      Info->BaseOffset = Entry.BaseOffset;
    }

    if (Info->MembersRef == LUA_NOREF) {
      lua_createtable(L, 0,
                      static_cast<int>(Entry.Calls.size() +
                                       Entry.Properties.size() +
                                       Entry.Fields.size()));
      Info->MembersRef = luna::detail::PinTop(L);
      lua_createtable(
          L, 0,
          static_cast<int>(Entry.Properties.size() + Entry.Fields.size()));
      Info->SettersRef = luna::detail::PinTop(L);
    }

    if (Info->BaseId != NoType) {
      ClassInfo *Base = World.Class(Info->BaseId);
      CopyEntries(World, L, Base->MembersRef, Info->MembersRef,
                  CopyFilter::All);
      CopyEntries(World, L, Base->SettersRef, Info->SettersRef,
                  CopyFilter::All);
    }

    // The public table survives re-registration, so a later commit extends a
    // class.
    if (Info->PublicRef == LUA_NOREF) {
      lua_createtable(L, 0, static_cast<int>(Entry.Calls.size() + 4));
      Info->PublicRef = luna::detail::PinTop(L);
    }
    const int PublicRef = Info->PublicRef;
    Target *IndexFallback = nullptr;
    Target *NewIndexFallback = nullptr;
    for (StagedOperator &Operation : Entry.Operators) {
      if (Operation.Slot == OperatorSlot::Index)
        IndexFallback = &Operation.Call;
      else if (Operation.Slot == OperatorSlot::NewIndex)
        NewIndexFallback = &Operation.Call;
    }

    // A second name for the same type is an alias, so the class keeps its own
    // name.
    const std::string_view Leaf = LeafOf(Info->Name);
    const bool Fresh = Info->MetatableRef == LUA_NOREF;
    if (Fresh) {
      lua_createtable(L, 0, 8);
      lua_pushlstring(L, Leaf.data(), Leaf.size());
      lua_setfield(L, -2, "__type");
      // getmetatable hands a script this string, not the metatable to edit.
      lua_pushstring(L, "The metatable is locked");
      lua_setfield(L, -2, "__metatable");
    } else {
      // Re-registration keeps the metatable, so earlier operators are not lost.
      PushRef(L, Info->MetatableRef);
    }

    if (Fresh || IndexFallback || NewIndexFallback) {
      const int MetaIndex = lua_gettop(L);
      PushRef(L, Info->MembersRef);
      lua_pushlstring(L, Info->Name.c_str(), Info->Name.size());
      if (IndexFallback && IndexFallback->Valid())
        IndexFallback->Push(L, IndexFallback->Ctx.get());
      else
        lua_pushnil(L);
      luna::detail::PushClosure(L, &ClassIndex, nullptr, 3);
      World.RememberSlot(MetaIndex, "__index");
      lua_setfield(L, MetaIndex, "__index");
      PushRef(L, Info->SettersRef);
      lua_pushlstring(L, Info->Name.c_str(), Info->Name.size());
      if (NewIndexFallback && NewIndexFallback->Valid())
        NewIndexFallback->Push(L, NewIndexFallback->Ctx.get());
      else
        lua_pushnil(L);
      luna::detail::PushClosure(L, &ClassNewIndex, nullptr, 3);
      World.RememberSlot(MetaIndex, "__newindex");
      lua_setfield(L, MetaIndex, "__newindex");
    }

    int MetatableRef = Info->MetatableRef;
    if (Fresh) {
      MetatableRef = luna::detail::PinTop(L);
      Info->MetatableRef = MetatableRef;
    } else {
      lua_pop(L, 1);
    }

    for (StagedOperator &Operation : Entry.Operators) {
      if (Operation.Slot == OperatorSlot::Index ||
          Operation.Slot == OperatorSlot::NewIndex)
        continue;

      Operation.Call.Push(L, Operation.Call.Ctx.get());
      if (Operation.Slot == OperatorSlot::Iterate)
        luna::detail::PushClosure(L, &ClassIter, nullptr, 1);

      SetFieldFromTop(World, L, MetatableRef, MetamethodName(Operation.Slot));
    }

    if (Info->BaseId != NoType) {
      if (const ClassInfo *Base = World.Class(Info->BaseId))
        CopyEntries(World, L, Base->MetatableRef, MetatableRef,
                    CopyFilter::Metamethods);
    }

    for (StagedCall &Call : Entry.Calls) {
      // A method's first parameter is the receiver, so scripts see one fewer.
      const int Offset = Call.Kind == Category::Method ? 1 : 0;
      const bool Overloaded = PushCallableValue(
          L, World, Entry.Name + "." + Call.Name, Call.Call, Error, Offset);
      if (!Error.empty())
        break;

      if (Call.Kind == Category::Method) {
        SetFieldFromTop(World, L, Info->MembersRef, Call.Name);
      } else {
        SetFieldFromTop(World, L, PublicRef, Call.Name);
      }

      const std::string Qualified = Entry.Name + "." + Call.Name;
      World.AddSymbol(
          Qualified,
          Overloaded                       ? SymbolKind::OverloadSet
          : Call.Kind == Category::Method  ? SymbolKind::Method
          : Call.Kind == Category::Factory ? SymbolKind::Factory
                                           : SymbolKind::StaticMethod,
          Call.Call.Arity < 0 ? -1 : Call.Call.Arity - Offset,
          Call.Call.Required < 0 ? -1 : Call.Call.Required - Offset);
    }

    for (StagedProperty &Property : Entry.Properties) {
      SetResolvable(World, L, Info->MembersRef, Property.Name, Property.Getter);
      if (Property.Writable)
        SetResolvable(World, L, Info->SettersRef, Property.Name,
                      Property.Setter);

      World.AddSymbol(Entry.Name + "." + Property.Name, SymbolKind::Property);
    }

    for (StagedField &Field : Entry.Fields) {
      SetResolvable(World, L, Info->MembersRef, Field.Name, Field.Getter);
      SetResolvable(World, L, Info->SettersRef, Field.Name, Field.Setter);
      World.AddSymbol(Entry.Name + "." + Field.Name, SymbolKind::Field);
    }

    if (!HasField(L, Info->MembersRef, "IsA")) {
      luna::detail::PushClosure(L, &ClassIsA, nullptr, 0);
      SetFieldFromTop(World, L, Info->MembersRef, "IsA");
    }

    for (StagedConstant &Constant : Entry.Constants) {
      PushOwnedValue(L, Constant.Value);
      SetFieldFromTop(World, L, PublicRef, Constant.Name);
      World.AddSymbol(Entry.Name + "." + Constant.Name, SymbolKind::Constant);
    }

    LinkInto(L, World, Entry.Name, PublicRef, Error);
    if (!Error.empty())
      break;

    World.AddSymbol(Entry.Name, SymbolKind::Class);
    if (Info->BaseId != NoType) {
      if (const ClassInfo *Base = World.Class(Info->BaseId))
        World.InheritSymbols(Base->Name, Entry.Name);
    }
  }

  if (!Error.empty())
    return;

  for (StagedCall &Entry : Pending.Calls) {
    const bool Overloaded =
        PushCallableValue(L, World, Entry.Name, Entry.Call, Error);
    if (!Error.empty())
      break;

    const int Ref = luna::detail::PinTop(L);
    LinkInto(L, World, Entry.Name, Ref, Error);
    luna::detail::Unpin(L, Ref);
    if (!Error.empty())
      break;

    World.AddSymbol(Entry.Name,
                    Overloaded ? SymbolKind::OverloadSet : SymbolKind::Function,
                    Entry.Call.Arity, Entry.Call.Required);
  }

  if (!Error.empty())
    return;

  for (StagedConstant &Entry : Pending.Constants) {
    PushOwnedValue(L, Entry.Value);
    const int Ref = luna::detail::PinTop(L);
    LinkInto(L, World, Entry.Name, Ref, Error);
    luna::detail::Unpin(L, Ref);
    if (!Error.empty())
      break;

    World.AddSymbol(Entry.Name, SymbolKind::Constant);
  }

  if (!Error.empty())
    return;

  for (StagedEnum &Entry : Pending.Enums) {
    const int Count = static_cast<int>(Entry.Values.size());
    lua_createtable(L, 0, Count);
    for (const StagedEnumValue &Value : Entry.Values) {
      if (Entry.AsObjects) {
        lua_createtable(L, 0, 3);
        lua_pushlstring(L, Value.Name.c_str(), Value.Name.size());
        lua_setfield(L, -2, "Name");
        lua_pushnumber(L, static_cast<double>(Value.Value));
        lua_setfield(L, -2, "Value");
        lua_pushlstring(L, Entry.Name.c_str(), Entry.Name.size());
        lua_setfield(L, -2, "EnumName");
        // An enumerator is a value; a script cannot rewrite it.
        lua_setreadonly(L, -1, 1);
      } else {
        lua_pushnumber(L, static_cast<double>(Value.Value));
      }

      lua_setfield(L, -2, Value.Name.c_str());
      World.AddSymbol(Entry.Name + "." + Value.Name, SymbolKind::Enumerator);
    }

    for (const std::pair<std::string, std::string> &Alias : Entry.Aliases) {
      lua_getfield(L, -1, Alias.second.c_str());
      if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        continue;
      }
      lua_setfield(L, -2, Alias.first.c_str());
      World.AddSymbol(Entry.Name + "." + Alias.first,
                      SymbolKind::EnumeratorAlias);
    }
    lua_setreadonly(L, -1, 1);

    const int Ref = luna::detail::PinTop(L);
    LinkInto(L, World, Entry.Name, Ref, Error);
    luna::detail::Unpin(L, Ref);
    if (!Error.empty())
      break;

    World.AddSymbol(Entry.Name, SymbolKind::Enumeration);
  }

  if (!Error.empty())
    return;

  for (StagedValue &Entry : Pending.Values) {
    Entry.Factory.Push(L, Entry.Factory.Ctx.get());
    if (lua_pcall(L, 0, 1, 0) != 0) {
      Error = "publishing '";
      Error += Entry.Name;
      Error += "' failed: ";
      const char *Message = lua_tostring(L, -1);
      Error += Message ? Message : "unknown error";
      lua_pop(L, 1);
      break;
    }

    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      Error = "publishing '";
      Error += Entry.Name;
      Error += "' produced no value";
      break;
    }

    if (Entry.Policy.GetKind() == OwnershipPolicy::Kind::Borrowed &&
        lua_type(L, -1) == LUA_TUSERDATA) {
      auto *Instance = static_cast<Object *>(lua_touserdata(L, -1));
      if (Instance && Instance->Magic == kObjectMagic &&
          static_cast<ObjectTag>(Instance->Tag) == ObjectTag::Borrowed) {
        std::shared_ptr<LifetimeHandle::Control> Control =
            Entry.Policy.GetLifetime().ControlBlock();
        if (Control) {
          World.RememberLifetime(Control);
          Instance->Control = Control.get();
        }
      }
    }

    const int Ref = luna::detail::PinTop(L);
    LinkInto(L, World, Entry.Name, Ref, Error);
    luna::detail::Unpin(L, Ref);
    if (!Error.empty())
      break;

    World.AddSymbol(Entry.Name, SymbolKind::Constant);
  }

  if (!Error.empty())
    return;
}

// What a Lua error said, as a registration diagnostic.
std::string Describe(const std::string &Text) {
  std::string Message = "registration failed: " + Text;
  if (Text.find("readonly") != std::string::npos)
    Message += " (the globals were made read-only by State::Sandbox; register "
               "before sandboxing)";
  return Message;
}

// True when a segment of Parent is already bound to something that is not a
// table, so a namespace could not be created there.
bool PathBlocked(lua_State *L, std::string_view Parent, std::string &Culprit) {
  if (Parent.empty())
    return false;

  const int Base = lua_gettop(L);
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  bool Blocked = false;
  std::size_t Start = 0;
  while (Start <= Parent.size()) {
    const std::size_t Dot = Parent.find('.', Start);
    const std::size_t End = Dot == std::string_view::npos ? Parent.size() : Dot;
    const std::string_view Segment = Parent.substr(Start, End - Start);
    lua_pushlstring(L, Segment.data(), Segment.size());
    lua_rawget(L, -2);
    if (lua_isnil(L, -1))
      break;
    if (!lua_istable(L, -1)) {
      Blocked = true;
      Culprit = std::string(Parent.substr(0, End));
      break;
    }
    lua_remove(L, -2);
    if (Dot == std::string_view::npos)
      break;
    Start = Dot + 1;
  }
  lua_settop(L, Base);
  return Blocked;
}

RegistrationResult Commit(Runtime &Core, Batch &&Pending) {
  Runtime *R = &Core;
  if (!R->L)
    return RegistrationResult(
        ErrorDiagnostic(ErrorCategory::Registration, "the state is not ready"));

  Model &World = R->World;
  lua_State *L = R->L;
  if (Pending.Empty())
    return RegistrationResult();

  std::string Error;
  std::unordered_set<std::string> Seen;
  auto ReserveGlobal = [&](std::string_view Name) {
    if (!ValidName(Name)) {
      Error = "invalid name '";
      Error += std::string(Name);
      Error += "'";
      return false;
    }
    if (!Seen.insert(std::string(Name)).second) {
      Error = "duplicate registration for '";
      Error += std::string(Name);
      Error += "' in one commit";
      return false;
    }
    return true;
  };
  for (const StagedClass &Entry : Pending.Classes) {
    if (!ReserveGlobal(Entry.Name))
      break;
    if (!Entry.Token) {
      Error = "class '";
      Error += Entry.Name;
      Error += "' has no type token";
      break;
    }

    for (const StagedCall &Call : Entry.Calls) {
      if (!Call.Call.Valid()) {
        Error = "member '";
        Error += Call.Name;
        Error += "' has an unsupported callable signature";
        break;
      }
    }
    if (!Error.empty())
      break;
  }

  for (const StagedCall &Entry : Pending.Calls) {
    if (!ReserveGlobal(Entry.Name))
      break;
    if (!Entry.Call.Valid()) {
      Error = "callable for '";
      Error += Entry.Name;
      Error += "' is unsupported";
      break;
    }
  }

  for (const StagedEnum &Entry : Pending.Enums) {
    if (!ReserveGlobal(Entry.Name))
      break;
    if (Entry.Values.empty()) {
      Error = "enum '";
      Error += Entry.Name;
      Error += "' has no values";
      break;
    }
    for (const std::pair<std::string, std::string> &Alias : Entry.Aliases) {
      bool Known = false;
      for (const StagedEnumValue &Value : Entry.Values)
        Known = Known || Value.Name == Alias.second;
      for (const std::pair<std::string, std::string> &Other : Entry.Aliases)
        Known = Known || Other.first == Alias.second;
      if (!Known) {
        Error = "enum '";
        Error += Entry.Name;
        Error += "' aliases '";
        Error += Alias.second;
        Error += "', which is not one of its values";
        break;
      }
    }
    if (!Error.empty())
      break;
  }

  for (const StagedConstant &Entry : Pending.Constants) {
    if (!ReserveGlobal(Entry.Name))
      break;
  }

  for (const StagedValue &Entry : Pending.Values) {
    if (!ReserveGlobal(Entry.Name))
      break;
    if (!Entry.Factory.Valid()) {
      Error = "value factory for '";
      Error += Entry.Name;
      Error += "' is unsupported";
      break;
    }
  }

  // A name can only mean one C++ type: a different type under a registered name
  // would be merged into it, and its instances could never be pushed.
  for (const StagedClass &Entry : Pending.Classes) {
    if (!Error.empty())
      break;
    const ClassInfo *Existing = World.FindOrNull(Entry.Name);
    if (Existing && Existing->Token != Entry.Token) {
      Error = "class '";
      Error += Entry.Name;
      Error += "' is already registered for a different type";
    }
  }

  // Members of a class share one table per side, so one name cannot be both a
  // method and a property. Methods may repeat: they become an overload set.
  for (const StagedClass &Entry : Pending.Classes) {
    if (!Error.empty())
      break;
    std::unordered_map<std::string, std::string> Instance;
    std::unordered_map<std::string, std::string> Static;
    auto Claim = [&](std::unordered_map<std::string, std::string> &Side,
                     const std::string &Name, const char *Kind,
                     bool Repeatable) {
      if (!Error.empty())
        return;
      const auto Added = Side.emplace(Name, Kind);
      if (Added.second || (Repeatable && Added.first->second == Kind))
        return;
      Error = "member '" + Entry.Name + "." + Name + "' is declared as both a ";
      Error += Added.first->second;
      Error += " and a ";
      Error += Kind;
      if (Added.first->second == Kind)
        Error = "member '" + Entry.Name + "." + Name +
                "' is declared more than once as a " + Kind;
    };
    for (const StagedCall &Call : Entry.Calls) {
      if (Call.Kind == Category::Method)
        Claim(Instance, Call.Name, "method", true);
      else
        Claim(Static, Call.Name, "static function", true);
    }
    for (const StagedProperty &Property : Entry.Properties)
      Claim(Instance, Property.Name, "property", false);
    for (const StagedField &Field : Entry.Fields)
      Claim(Instance, Field.Name, "field", false);
    for (const StagedConstant &Constant : Entry.Constants)
      Claim(Static, Constant.Name, "constant", false);
  }

  // A namespace cannot be created over a global that is not a table, and that
  // is only found out by looking, so it is looked for before anything is
  // linked.
  if (Error.empty()) {
    auto Probe = [&](const std::string &Name) {
      if (!Error.empty())
        return;
      std::string Culprit;
      if (PathBlocked(L, ParentOf(Name), Culprit)) {
        Error = "cannot register '" + Name + "': '" + Culprit +
                "' is already bound to a value that is not a table";
      }
    };
    for (const StagedClass &Entry : Pending.Classes)
      Probe(Entry.Name);
    for (const StagedCall &Entry : Pending.Calls)
      Probe(Entry.Name);
    for (const StagedEnum &Entry : Pending.Enums)
      Probe(Entry.Name);
    for (const StagedConstant &Entry : Pending.Constants)
      Probe(Entry.Name);
    for (const StagedValue &Entry : Pending.Values)
      Probe(Entry.Name);
  }

  // A callable that names a class must be registered after that class (or in
  // the same commit), because its parameter checks are resolved when it is
  // linked.
  if (Error.empty()) {
    std::unordered_set<const void *> Known;
    for (const StagedClass &Entry : Pending.Classes)
      Known.insert(Entry.Token);

    auto Require = [&](const Target &Call, const std::string &Owner) {
      if (!Error.empty())
        return;
      for (const void *Token : Call.Needs) {
        if (Known.count(Token) != 0 || World.ClassIdForToken(Token) != NoType)
          continue;
        Error = "'";
        Error += Owner;
        Error += "' takes a class that is not registered; register the class "
                 "first, or in the same commit";
        return;
      }
    };

    for (const StagedCall &Entry : Pending.Calls)
      Require(Entry.Call, Entry.Name);
    for (const StagedValue &Entry : Pending.Values)
      Require(Entry.Factory, Entry.Name);
    for (const StagedClass &Entry : Pending.Classes) {
      for (const StagedCall &Call : Entry.Calls)
        Require(Call.Call, Entry.Name + "." + Call.Name);
      for (const StagedProperty &Property : Entry.Properties) {
        Require(Property.Getter, Entry.Name + "." + Property.Name);
        Require(Property.Setter, Entry.Name + "." + Property.Name);
      }
      for (const StagedField &Field : Entry.Fields) {
        Require(Field.Getter, Entry.Name + "." + Field.Name);
        Require(Field.Setter, Entry.Name + "." + Field.Name);
      }
      for (const StagedOperator &Operation : Entry.Operators)
        Require(Operation.Call, Entry.Name + ".__operator");
    }
  }

  if (!Error.empty())
    return RegistrationResult(
        ErrorDiagnostic(ErrorCategory::Registration, std::move(Error)));

  const Model::Checkpoint Before = World.Mark();
  std::string Fault;
  const bool Linked = RunGuarded(
      *R,
      [&] {
        World.LockClasses(false);
        LinkBatch(*R, Pending, Error);
      },
      Fault);
  if (!Linked && Error.empty())
    Error = Describe(Fault);

  std::string Ignored;
  if (!Error.empty()) {
    // Undo whatever the link got through, so the failure publishes nothing.
    // Putting things back needs a little memory, and the failure may well have
    // been the ceiling, so the limit is lifted while the undo runs.
    const std::size_t Limit = R->Memory.Limit;
    R->Memory.Limit = 0;
    if (!RunGuarded(
            *R,
            [&] {
              World.Rollback(Before);
              // What the failed link built is garbage now.
              lua_gc(L, LUA_GCCOLLECT, 0);
            },
            Ignored))
      World.Seal();
    R->Memory.Limit = Limit;
  } else {
    World.Seal();
  }

  RunGuarded(*R, [&] { World.LockClasses(true); }, Ignored);
  if (!Error.empty())
    return RegistrationResult(
        ErrorDiagnostic(ErrorCategory::Registration, std::move(Error)));
  return RegistrationResult();
}
} // namespace detail

RegistrationResult BindingRegistry::CommitBatch(detail::Batch &&Pending) {
  if (!Runtime_ || !Life_)
    return RegistrationResult(ErrorDiagnostic(ErrorCategory::Registration,
                                              "registry is not attached"));
  if (!Life_->Alive.load(std::memory_order_acquire))
    return RegistrationResult(
        ErrorDiagnostic(ErrorCategory::Registration, "the state is gone"));
  return detail::Commit(*Runtime_, std::move(Pending));
}

State *BindingRegistry::Owner() const noexcept {
  if (!Runtime_ || !Life_ || !Life_->Alive.load(std::memory_order_acquire))
    return nullptr;
  return Runtime_->Owner;
}

ReflectionSnapshot BindingRegistry::Reflection() const {
  State *Host = Owner();
  return Host ? Host->Reflection() : ReflectionSnapshot();
}
} // namespace luna
