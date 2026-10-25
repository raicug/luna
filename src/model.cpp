#include "model.hpp"

#include <luna/state.hpp>

#include <lua.h>
#include <lualib.h>

#include <utility>

namespace luna {
namespace detail {
namespace {
void PushRoot(lua_State *L) {
  lua_pushvalue(L, LUA_GLOBALSINDEX);
}
} // namespace

Model::Model(lua_State *VM) : L(VM) {}

Model::~Model() {
  Release();
}

void Model::Release() noexcept {
  if (!L)
    return;

  for (ClassInfo &Entry : Classes) {
    if (Entry.MetatableRef != LUA_NOREF)
      luna::detail::Unpin(L, Entry.MetatableRef);
    if (Entry.MembersRef != LUA_NOREF)
      luna::detail::Unpin(L, Entry.MembersRef);
    if (Entry.SettersRef != LUA_NOREF)
      luna::detail::Unpin(L, Entry.SettersRef);
    if (Entry.PublicRef != LUA_NOREF)
      luna::detail::Unpin(L, Entry.PublicRef);
    Entry.MetatableRef = LUA_NOREF;
    Entry.MembersRef = LUA_NOREF;
    Entry.SettersRef = LUA_NOREF;
    Entry.PublicRef = LUA_NOREF;
  }
  for (auto &Entry : Namespaces) {
    if (Entry.second != LUA_NOREF)
      luna::detail::Unpin(L, Entry.second);
    Entry.second = LUA_NOREF;
  }

  L = nullptr;
}

TypeId Model::ClassIdForToken(const void *Token) const noexcept {
  if (!Token)
    return NoType;
  const auto Found = ByToken.find(Token);
  return Found == ByToken.end() ? NoType : Found->second;
}

const ClassInfo *Model::Class(TypeId Id) const noexcept {
  if (Id == NoType || Id > Classes.size())
    return nullptr;
  return &Classes[Id - 1];
}

ClassInfo *Model::Class(TypeId Id) noexcept {
  if (Id == NoType || Id > Classes.size())
    return nullptr;
  return &Classes[Id - 1];
}

ClassInfo *Model::FindOrNull(std::string_view Name) noexcept {
  const auto Found = ByName.find(Name);
  return Found == ByName.end() ? nullptr : &Classes[Found->second - 1];
}

bool Model::RebaseTo(TypeId From, TypeId Ancestor,
                     void *&Pointer) const noexcept {
  TypeId Current = From;
  std::ptrdiff_t Offset = 0;
  for (std::size_t Guard = 0; Current != NoType && Guard <= Classes.size();
       ++Guard) {
    if (Current == Ancestor) {
      Pointer = static_cast<char *>(Pointer) + Offset;
      return true;
    }
    const ClassInfo *Info = Class(Current);
    if (!Info)
      return false;
    Offset += Info->BaseOffset;
    Current = Info->BaseId;
  }
  return false;
}

namespace {
bool NamedAs(const ClassInfo &Info, std::string_view Name) noexcept {
  if (Info.Name == Name)
    return true;
  const std::string_view Full(Info.Name);
  const std::size_t Dot = Full.rfind('.');
  if (Dot != std::string_view::npos && Full.substr(Dot + 1) == Name)
    return true;
  for (const std::string_view Alias : Info.Aliases) {
    if (Alias == Name)
      return true;
  }
  return false;
}
} // namespace

bool Model::DerivesFrom(TypeId From, std::string_view Name) const noexcept {
  TypeId Current = From;
  for (std::size_t Guard = 0; Current != NoType && Guard <= Classes.size();
       ++Guard) {
    const ClassInfo *Info = Class(Current);
    if (!Info)
      return false;
    if (NamedAs(*Info, Name))
      return true;
    Current = Info->BaseId;
  }
  return false;
}

TypeId Model::AddClass(std::string Name, std::string StableKey,
                       const void *Token) {
  if (const ClassInfo *Existing = FindOrNull(Name))
    return Existing->Id;

  // One C++ type is one class: a second name becomes an alias of the first.
  if (Token) {
    const auto Found = ByToken.find(Token);
    if (Found != ByToken.end()) {
      if (ClassInfo *Owner = Class(Found->second)) {
        const std::string_view Alias = Intern(Name);
        AliasLog_.push_back(AliasUndo{Owner->Id, Owner->Aliases.size(), Alias});
        Owner->Aliases.push_back(Alias);
        ByName.emplace(Alias, Owner->Id);
        return Owner->Id;
      }
    }
  }

  ClassInfo Entry;
  Entry.Name = std::move(Name);
  Entry.StableKey = std::move(StableKey);
  Entry.Token = Token;
  Entry.Id = static_cast<TypeId>(Classes.size() + 1);
  Classes.push_back(std::move(Entry));
  ClassInfo &Stored = Classes.back();
  // Interned so the lookup keys stay valid for the life of the state.
  ByName.emplace(Intern(Stored.Name), Stored.Id);
  if (Token)
    ByToken.emplace(Token, Stored.Id);

  return Stored.Id;
}

std::string_view Model::Intern(std::string_view Name) {
  const auto Found = Interned.find(Name);
  if (Found != Interned.end())
    return std::string_view(*std::next(Names.begin(), Found->second));

  Names.emplace_back(Name);
  const int Index = static_cast<int>(Names.size() - 1);
  std::string_view View(Names.back());
  Interned.emplace(View, Index);
  return View;
}

namespace {
bool IsCallableKind(SymbolKind Kind) noexcept {
  return Kind == SymbolKind::Function || Kind == SymbolKind::Method ||
         Kind == SymbolKind::StaticMethod || Kind == SymbolKind::Factory ||
         Kind == SymbolKind::Constructor || Kind == SymbolKind::OverloadSet;
}
} // namespace

void Model::AddSymbol(std::string_view Qualified, SymbolKind Kind, int Arity,
                      int Required) {
  std::lock_guard<std::mutex> Guard(SymbolLock);
  const std::string Key(Qualified);
  const auto Known = SymbolIndex.find(Key);
  if (Known != SymbolIndex.end()) {
    for (const std::size_t Index : Known->second) {
      PendingSymbol &Existing = SymbolEntries[Index];
      if (Existing.Kind == Kind)
        return;
      // One name, several signatures: reflection reports it as an overload set.
      if (IsCallableKind(Kind) && IsCallableKind(Existing.Kind)) {
        SymbolEdits_.push_back(SymbolEdit{Index, Existing});
        Existing.Kind = SymbolKind::OverloadSet;
        Existing.Arity = -1;
        Existing.Required = -1;
        SymbolsDirty = true;
        return;
      }
    }
  }
  SymbolIndex[Key].push_back(SymbolEntries.size());
  SymbolEntries.push_back(PendingSymbol{Key, Kind, Arity, Required});
  SymbolsDirty = true;
}

void Model::InheritSymbols(std::string_view BaseName,
                           std::string_view DerivedName) {
  std::lock_guard<std::mutex> Guard(SymbolLock);
  const std::string Prefix = std::string(BaseName) + ".";
  std::vector<PendingSymbol> Copies;
  for (const PendingSymbol &Existing : SymbolEntries) {
    if (Existing.Qualified.size() <= Prefix.size())
      continue;
    if (Existing.Qualified.compare(0, Prefix.size(), Prefix) != 0)
      continue;

    const std::string Inherited = std::string(DerivedName) + "." +
                                  Existing.Qualified.substr(Prefix.size());
    // A member the derived class declares itself is not overwritten.
    if (SymbolIndex.count(Inherited) != 0)
      continue;
    Copies.push_back(PendingSymbol{Inherited, Existing.Kind, Existing.Arity,
                                   Existing.Required});
  }

  if (Copies.empty())
    return;
  for (PendingSymbol &Copy : Copies) {
    SymbolIndex[Copy.Qualified].push_back(SymbolEntries.size());
    SymbolEntries.push_back(std::move(Copy));
  }
  SymbolsDirty = true;
}

std::shared_ptr<const std::vector<Symbol>> Model::Snapshot() const {
  std::lock_guard<std::mutex> Guard(SymbolLock);
  if (!SymbolsDirty && CachedSymbols)
    return CachedSymbols;

  auto Built = std::make_shared<std::vector<Symbol>>();
  Built->reserve(SymbolEntries.size());
  for (const PendingSymbol &Entry : SymbolEntries) {
    const std::size_t Dot = Entry.Qualified.rfind('.');
    const std::string Short = Dot == std::string::npos
                                  ? Entry.Qualified
                                  : Entry.Qualified.substr(Dot + 1);
    Built->emplace_back(Short, Entry.Qualified, Entry.Kind, Entry.Arity,
                        Entry.Required);
  }

  CachedSymbols = std::move(Built);
  SymbolsDirty = false;
  return CachedSymbols;
}

int Model::EnsureNamespace(std::string_view Path, std::string &Error) {
  const auto Cached = Namespaces.find(Path);
  if (Cached != Namespaces.end())
    return Cached->second;

  if (Path.empty()) {
    PushRoot(L);
    const int Ref = luna::detail::PinTop(L);
    Namespaces.emplace(Intern(std::string_view()), Ref);
    return Ref;
  }

  PushRoot(L);
  std::size_t Start = 0;
  while (Start <= Path.size()) {
    const std::size_t Dot = Path.find('.', Start);
    const std::string_view Segment = Dot == std::string_view::npos
                                         ? Path.substr(Start)
                                         : Path.substr(Start, Dot - Start);

    if (Segment.empty()) {
      Error = "namespace path has an empty segment";
      lua_pop(L, 1);
      return LUA_NOREF;
    }

    lua_pushlstring(L, Segment.data(), Segment.size());
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      lua_createtable(L, 0, 4);
      RememberSlot(lua_gettop(L) - 1, Segment);
      lua_pushlstring(L, Segment.data(), Segment.size());
      lua_pushvalue(L, -2);
      lua_rawset(L, -4);
    } else if (!lua_istable(L, -1)) {
      Error = "namespace '";
      Error += std::string(Path);
      Error += "' collides with a non-table value";
      lua_pop(L, 2);
      return LUA_NOREF;
    }

    lua_remove(L, -2);
    if (Dot == std::string_view::npos)
      break;
    Start = Dot + 1;
  }

  const int Ref = luna::detail::PinTop(L);
  const std::string_view Key = Intern(Path);
  if (Namespaces.emplace(Key, Ref).second)
    NamespaceLog_.push_back(Key);
  AddSymbol(Path, SymbolKind::Namespace);
  return Ref;
}

Model::Checkpoint Model::Mark() const noexcept {
  return Checkpoint{SymbolEntries.size(), Classes.size(), Lifetimes.size()};
}

void Model::RememberSlot(int TableIndex, std::string_view Key) {
  const void *Identity = lua_topointer(L, TableIndex);
  int Table = LUA_NOREF;
  const auto Known = LoggedTables_.find(Identity);
  if (Known != LoggedTables_.end()) {
    Table = Known->second;
  } else {
    lua_pushvalue(L, TableIndex);
    Table = luna::detail::PinTop(L);
    LoggedTables_.emplace(Identity, Table);
  }

  lua_pushlstring(L, Key.data(), Key.size());
  lua_rawget(L, TableIndex);
  int Previous = LUA_NOREF;
  if (lua_isnil(L, -1))
    lua_pop(L, 1);
  else
    Previous = luna::detail::PinTop(L);
  SlotLog_.push_back(SlotUndo{Table, std::string(Key), Previous});
}

void Model::RememberBase(const ClassInfo &Info) {
  BaseLog_.push_back(BaseUndo{Info.Id, Info.BaseId, Info.BaseOffset});
}

void Model::Seal() noexcept {
  if (L) {
    for (const SlotUndo &Entry : SlotLog_) {
      if (Entry.Previous != LUA_NOREF)
        luna::detail::Unpin(L, Entry.Previous);
    }
    for (const auto &Entry : LoggedTables_)
      luna::detail::Unpin(L, Entry.second);
  }
  SlotLog_.clear();
  LoggedTables_.clear();
  SymbolEdits_.clear();
  CallableLog_.clear();
  NamespaceLog_.clear();
  AliasLog_.clear();
  BaseLog_.clear();
  LifetimeSetLog_.clear();
}

void Model::LockClasses(bool Readonly) {
  if (!L)
    return;
  for (const ClassInfo &Entry : Classes) {
    for (const int Ref : {Entry.MetatableRef, Entry.MembersRef,
                          Entry.SettersRef, Entry.PublicRef}) {
      if (Ref == LUA_NOREF)
        continue;
      lua_rawgeti(L, LUA_REGISTRYINDEX, Ref);
      lua_setreadonly(L, -1, Readonly ? 1 : 0);
      lua_pop(L, 1);
    }
  }
}

void Model::Rollback(const Checkpoint &Before) {
  // Lua side first, newest change first, so a slot written twice ends as it
  // began.
  if (L) {
    for (std::size_t Index = SlotLog_.size(); Index-- > 0;) {
      const SlotUndo &Entry = SlotLog_[Index];
      lua_rawgeti(L, LUA_REGISTRYINDEX, Entry.Table);
      lua_pushlstring(L, Entry.Key.data(), Entry.Key.size());
      if (Entry.Previous == LUA_NOREF)
        lua_pushnil(L);
      else
        lua_rawgeti(L, LUA_REGISTRYINDEX, Entry.Previous);
      lua_rawset(L, -3);
      lua_pop(L, 1);
    }
  }

  {
    std::lock_guard<std::mutex> Guard(SymbolLock);
    for (std::size_t Index = SymbolEdits_.size(); Index-- > 0;) {
      if (SymbolEdits_[Index].Index < SymbolEntries.size())
        SymbolEntries[SymbolEdits_[Index].Index] = SymbolEdits_[Index].Before;
    }
    for (std::size_t Index = SymbolEntries.size(); Index-- > Before.Symbols;) {
      const auto Found = SymbolIndex.find(SymbolEntries[Index].Qualified);
      if (Found == SymbolIndex.end())
        continue;
      if (!Found->second.empty() && Found->second.back() == Index)
        Found->second.pop_back();
      if (Found->second.empty())
        SymbolIndex.erase(Found);
    }
    if (SymbolEntries.size() > Before.Symbols)
      SymbolEntries.resize(Before.Symbols);
    SymbolsDirty = true;
    CachedSymbols.reset();
  }

  for (std::size_t Index = CallableLog_.size(); Index-- > 0;) {
    const auto Found = Callables.find(CallableLog_[Index].first);
    if (Found != Callables.end() &&
        Found->second.size() > CallableLog_[Index].second)
      Found->second.resize(CallableLog_[Index].second);
  }

  for (const std::string_view Name : NamespaceLog_) {
    const auto Found = Namespaces.find(Name);
    if (Found == Namespaces.end())
      continue;
    if (L && Found->second != LUA_NOREF)
      luna::detail::Unpin(L, Found->second);
    Namespaces.erase(Found);
  }

  for (std::size_t Index = AliasLog_.size(); Index-- > 0;) {
    const AliasUndo &Entry = AliasLog_[Index];
    if (ClassInfo *Owner = Class(Entry.Id)) {
      if (Owner->Aliases.size() > Entry.Count)
        Owner->Aliases.resize(Entry.Count);
    }
    ByName.erase(Entry.Alias);
  }

  for (const TypeId Id : LifetimeSetLog_) {
    if (ClassInfo *Owner = Class(Id))
      Owner->Lifetime.reset();
  }

  for (std::size_t Index = BaseLog_.size(); Index-- > 0;) {
    if (ClassInfo *Owner = Class(BaseLog_[Index].Id)) {
      Owner->BaseId = BaseLog_[Index].Base;
      Owner->BaseOffset = BaseLog_[Index].Offset;
    }
  }

  while (Classes.size() > Before.Classes) {
    ClassInfo &Doomed = Classes.back();
    if (L) {
      for (const int Ref : {Doomed.MetatableRef, Doomed.MembersRef,
                            Doomed.SettersRef, Doomed.PublicRef}) {
        if (Ref != LUA_NOREF)
          luna::detail::Unpin(L, Ref);
      }
    }
    for (const std::string_view Alias : Doomed.Aliases)
      ByName.erase(Alias);
    ByName.erase(std::string_view(Doomed.Name));
    const auto Token = ByToken.find(Doomed.Token);
    if (Token != ByToken.end() && Token->second == Doomed.Id)
      ByToken.erase(Token);
    Classes.pop_back();
  }

  if (Lifetimes.size() > Before.Lifetimes)
    Lifetimes.resize(Before.Lifetimes);

  Seal();
}
} // namespace detail
} // namespace luna
