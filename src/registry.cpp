#include "runtime.hpp"

#include <luna/bound.hpp>
#include <luna/state.hpp>

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace luna {
namespace detail {
namespace {

std::size_t LastDot(std::string_view Name) { return Name.rfind('.'); }

std::string_view ParentOf(std::string_view Qualified) {
  const std::size_t Dot = LastDot(Qualified);
  return Dot == std::string_view::npos ? std::string_view()
                                       : Qualified.substr(0, Dot);
}

std::string_view LeafOf(std::string_view Qualified) {
  const std::size_t Dot = LastDot(Qualified);
  return Dot == std::string_view::npos ? Qualified
                                       : Qualified.substr(Dot + 1);
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
  case OperatorSlot::Add: return "__add";
  case OperatorSlot::Subtract: return "__sub";
  case OperatorSlot::Multiply: return "__mul";
  case OperatorSlot::Divide: return "__div";
  case OperatorSlot::Modulo: return "__mod";
  case OperatorSlot::Power: return "__pow";
  case OperatorSlot::Negate: return "__unm";
  case OperatorSlot::Equal: return "__eq";
  case OperatorSlot::Less: return "__lt";
  case OperatorSlot::LessOrEqual: return "__le";
  case OperatorSlot::Concat: return "__concat";
  case OperatorSlot::Len: return "__len";
  case OperatorSlot::ToString: return "__tostring";
  case OperatorSlot::Call: return "__call";
  case OperatorSlot::Iterate: return "__iter";
  case OperatorSlot::Index: return "__index";
  case OperatorSlot::NewIndex: return "__newindex";
  }
  return "__index";
}

// Pushes the table stored under a registry ref.
void PushRef(lua_State *L, int Ref) {
  lua_rawgeti(L, LUA_REGISTRYINDEX, Ref);
}

// table[Name] = closure popped from the top of the stack.
void SetFieldFromTop(lua_State *L, int TableRef, std::string_view Name) {
  const std::string Key(Name);
  PushRef(L, TableRef);
  lua_insert(L, -2);
  lua_setfield(L, -2, Key.c_str());
  lua_pop(L, 1);
}

// Stores a callable in a lookup table. A property read is then one hash lookup
// plus a direct call: a target that can be invoked in place is stored as its
// context userdata alone, whose first word is the dispatch thunk. Anything else
// falls back to a closure, which the metamethods recognise by type.
void SetResolvable(lua_State *L, int TableRef, std::string_view Name,
                   Target &Call) {
  if (Call.Direct())
    Call.Context(L, Call.Ctx.get());
  else
    Call.Push(L, Call.Ctx.get());

  SetFieldFromTop(L, TableRef, Name);
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
  PushRef(L, ValueRef);
  lua_setfield(L, -2, std::string(Leaf).c_str());
  lua_pop(L, 1);
  // The parent ref stays pinned: it belongs to the model's namespace cache, and
  // unpinning it here would hand the slot back for reuse.
}

} // namespace

RegistrationResult Commit(State &Owner, Batch &&Pending) {
  Runtime *R = Owner.GetRuntime();
  if (!R || !R->L)
    return RegistrationResult(ErrorDiagnostic(ErrorCategory::Registration,
                                              "the state is not ready"));

  Model &World = R->World;
  lua_State *L = R->L;

  if (Pending.Empty())
    return RegistrationResult();

  // -------------------------------------------------------------------------
  // 1. Validate. This is a pure pass over the batch; nothing links yet, so a
  //    rejection leaves the VM untouched and needs no undo.
  // -------------------------------------------------------------------------

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

    std::unordered_set<std::string> Members;
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

  if (!Error.empty())
    return RegistrationResult(
        ErrorDiagnostic(ErrorCategory::Registration, std::move(Error)));

  // -------------------------------------------------------------------------
  // 2. Allocate every class id first, so a member can refer to a class that is
  //    registered in the same batch.
  // -------------------------------------------------------------------------

  std::vector<TypeId> ClassIds;
  ClassIds.reserve(Pending.Classes.size());

  for (StagedClass &Entry : Pending.Classes) {
    const std::string Key =
        Entry.StableKey.empty() ? Entry.Name : Entry.StableKey;
    ClassIds.push_back(World.AddClass(Entry.Name, Key, Entry.Token));
  }

  // -------------------------------------------------------------------------
  // 3. Build each class: member tables, metatable, members, then link.
  // -------------------------------------------------------------------------

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

    // Member tables, created once per class. Methods and properties share one
    // resolve table so a read is a single hash lookup; a property entry is a
    // one-element table wrapping its getter, which distinguishes it from a
    // method without a second lookup.
    if (Info->MembersRef == LUA_NOREF) {
      lua_createtable(L, 0, static_cast<int>(Entry.Calls.size() +
                                             Entry.Properties.size() +
                                             Entry.Fields.size()));
      Info->MembersRef = luna::detail::PinTop(L);

      lua_createtable(
          L, 0,
          static_cast<int>(Entry.Properties.size() + Entry.Fields.size()));
      Info->SettersRef = luna::detail::PinTop(L);
    }

    // The class table is what a script sees: factories, statics, constants.
    lua_createtable(L, 0, static_cast<int>(Entry.Calls.size() + 4));
    const int PublicRef = luna::detail::PinTop(L);

    // An Index or NewIndex operator is not a metamethod of its own: it becomes
    // the fallback upvalue of __index / __newindex, so it is installed while
    // those closures are built rather than after.
    Target *IndexFallback = nullptr;
    Target *NewIndexFallback = nullptr;
    for (StagedOperator &Operation : Entry.Operators) {
      if (Operation.Slot == OperatorSlot::Index)
        IndexFallback = &Operation.Call;
      else if (Operation.Slot == OperatorSlot::NewIndex)
        NewIndexFallback = &Operation.Call;
    }

    // Metatable: __type for typeof(), __index and __newindex wired to the
    // tables above, then every operator the class declared. __type carries the
    // leaf name, so typeof() answers "Sprite" even for Studio.Sprite.
    const std::string_view Leaf = LeafOf(Entry.Name);
    lua_createtable(L, 0, 8);
    lua_pushlstring(L, Leaf.data(), Leaf.size());
    lua_setfield(L, -2, "__type");

    PushRef(L, Info->MembersRef);
    lua_pushlstring(L, Entry.Name.c_str(), Entry.Name.size());
    if (IndexFallback && IndexFallback->Valid()) {
      IndexFallback->Push(L, IndexFallback->Ctx.get());
    } else {
      lua_pushnil(L);
    }
    luna::detail::PushClosure(L, &ClassIndex, nullptr, 3);
    lua_setfield(L, -2, "__index");

    PushRef(L, Info->SettersRef);
    lua_pushlstring(L, Entry.Name.c_str(), Entry.Name.size());
    if (NewIndexFallback && NewIndexFallback->Valid()) {
      NewIndexFallback->Push(L, NewIndexFallback->Ctx.get());
    } else {
      lua_pushnil(L);
    }
    luna::detail::PushClosure(L, &ClassNewIndex, nullptr, 3);
    lua_setfield(L, -2, "__newindex");

    const int MetatableRef = luna::detail::PinTop(L);
    Info->MetatableRef = MetatableRef;

    // Operators. Index and NewIndex were consumed above.
    for (StagedOperator &Operation : Entry.Operators) {
      if (Operation.Slot == OperatorSlot::Index ||
          Operation.Slot == OperatorSlot::NewIndex)
        continue;

      Operation.Call.Push(L, Operation.Call.Ctx.get());

      // __iter must be the wrapper, not the target: the wrapper builds the
      // stepping closure and hands it the target plus the receiver.
      if (Operation.Slot == OperatorSlot::Iterate)
        luna::detail::PushClosure(L, &ClassIter, nullptr, 1);

      PushRef(L, MetatableRef);
      lua_insert(L, -2);
      lua_setfield(L, -2, MetamethodName(Operation.Slot));
      lua_pop(L, 1);
    }

    // Methods go into the member table; factories and statics into the class
    // table. A method is returned by __index and called with the receiver
    // prepended by Lua, so its first parameter reads argument 1.
    for (StagedCall &Call : Entry.Calls) {
      Call.Call.Push(L, Call.Call.Ctx.get());

      if (Call.Kind == Category::Method) {
        SetFieldFromTop(L, Info->MembersRef, Call.Name);
      } else {
        SetFieldFromTop(L, PublicRef, Call.Name);
      }

      const std::string Qualified = Entry.Name + "." + Call.Name;
      World.AddSymbol(Qualified,
                      Call.Kind == Category::Method  ? SymbolKind::Method
                      : Call.Kind == Category::Factory ? SymbolKind::Factory
                                                       : SymbolKind::StaticMethod);
    }

    // A property enters the resolve table as { context, thunk }, which is what
    // tells __index to read it rather than return it. Methods are stored as
    // bare closures and returned as-is.
    for (StagedProperty &Property : Entry.Properties) {
      SetResolvable(L, Info->MembersRef, Property.Name, Property.Getter);

      if (Property.Writable)
        SetResolvable(L, Info->SettersRef, Property.Name, Property.Setter);

      World.AddSymbol(Entry.Name + "." + Property.Name, SymbolKind::Property);
    }

    for (StagedField &Field : Entry.Fields) {
      SetResolvable(L, Info->MembersRef, Field.Name, Field.Getter);
      SetResolvable(L, Info->SettersRef, Field.Name, Field.Setter);
      World.AddSymbol(Entry.Name + "." + Field.Name, SymbolKind::Field);
    }

    for (StagedConstant &Constant : Entry.Constants) {
      PushOwnedValue(L, Constant.Value);
      SetFieldFromTop(L, PublicRef, Constant.Name);
      World.AddSymbol(Entry.Name + "." + Constant.Name, SymbolKind::Constant);
    }

    // Link the class table at its qualified name.
    LinkInto(L, World, Entry.Name, PublicRef, Error);
    if (!Error.empty())
      break;

    luna::detail::Unpin(L, PublicRef);
    World.AddSymbol(Entry.Name, SymbolKind::Class);
  }

  if (!Error.empty())
    return RegistrationResult(
        ErrorDiagnostic(ErrorCategory::Registration, std::move(Error)));

  // -------------------------------------------------------------------------
  // 4. Root and namespaced functions.
  // -------------------------------------------------------------------------

  for (StagedCall &Entry : Pending.Calls) {
    Entry.Call.Push(L, Entry.Call.Ctx.get());

    const int Ref = luna::detail::PinTop(L);
    LinkInto(L, World, Entry.Name, Ref, Error);
    luna::detail::Unpin(L, Ref);

    if (!Error.empty())
      break;

    World.AddSymbol(Entry.Name, SymbolKind::Function);
  }

  if (!Error.empty())
    return RegistrationResult(
        ErrorDiagnostic(ErrorCategory::Registration, std::move(Error)));

  // -------------------------------------------------------------------------
  // 5. Constants.
  // -------------------------------------------------------------------------

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
    return RegistrationResult(
        ErrorDiagnostic(ErrorCategory::Registration, std::move(Error)));

  // -------------------------------------------------------------------------
  // 6. Enumerations.
  // -------------------------------------------------------------------------

  for (StagedEnum &Entry : Pending.Enums) {
    const int Count = static_cast<int>(Entry.Values.size());
    lua_createtable(L, 0, Count);

    for (const StagedEnumValue &Value : Entry.Values) {
      if (Entry.AsObjects) {
        // An interned enumerator object: Name, Value, EnumName, and identity
        // equality against itself only.
        lua_createtable(L, 0, 3);
        lua_pushlstring(L, Value.Name.c_str(), Value.Name.size());
        lua_setfield(L, -2, "Name");
        lua_pushnumber(L, static_cast<double>(Value.Value));
        lua_setfield(L, -2, "Value");
        lua_pushlstring(L, Entry.Name.c_str(), Entry.Name.size());
        lua_setfield(L, -2, "EnumName");
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

    const int Ref = luna::detail::PinTop(L);
    LinkInto(L, World, Entry.Name, Ref, Error);
    luna::detail::Unpin(L, Ref);

    if (!Error.empty())
      break;

    World.AddSymbol(Entry.Name, SymbolKind::Enumeration);
  }

  if (!Error.empty())
    return RegistrationResult(
        ErrorDiagnostic(ErrorCategory::Registration, std::move(Error)));

  // -------------------------------------------------------------------------
  // 7. Published instances.
  // -------------------------------------------------------------------------

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

    // A borrowed publication needs the lifetime block attached, and the model
    // keeps that block alive for as long as the state is.
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
    return RegistrationResult(
        ErrorDiagnostic(ErrorCategory::Registration, std::move(Error)));

  return RegistrationResult();
}

} // namespace detail

// ---------------------------------------------------------------------------
// BindingRegistry
// ---------------------------------------------------------------------------

RegistrationResult BindingRegistry::CommitBatch(detail::Batch &&Pending) {
  if (!Owner_)
    return RegistrationResult(ErrorDiagnostic(ErrorCategory::Registration,
                                              "registry is not attached"));
  return detail::Commit(*Owner_, std::move(Pending));
}

ReflectionSnapshot BindingRegistry::Reflection() const {
  if (!Owner_)
    return ReflectionSnapshot();
  return Owner_->Reflection();
}

} // namespace luna
