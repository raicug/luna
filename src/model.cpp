#include "model.hpp"

#include <luna/state.hpp>

#include <lua.h>
#include <lualib.h>

#include <utility>

namespace luna {
namespace detail {
namespace {

// The global table is the root namespace.
void PushRoot(lua_State *L) { lua_pushvalue(L, LUA_GLOBALSINDEX); }

} // namespace

Model::Model(lua_State *VM) : L(VM) {}

Model::~Model() {
  if (!L)
    return;

  // Metatables and per-class tables live in the registry, so they are released
  // with the VM. Unref defensively in case the model outlives nothing.
  for (ClassInfo &Entry : Classes) {
    if (Entry.MetatableRef != LUA_NOREF)
      luna::detail::Unpin(L, Entry.MetatableRef);
    if (Entry.MembersRef != LUA_NOREF)
      luna::detail::Unpin(L, Entry.MembersRef);
    if (Entry.SettersRef != LUA_NOREF)
      luna::detail::Unpin(L, Entry.SettersRef);
  }
  for (auto &Entry : Namespaces) {
    if (Entry.second != LUA_NOREF)
      luna::detail::Unpin(L, Entry.second);
  }
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

TypeId Model::AddClass(std::string Name, std::string StableKey,
                       const void *Token) {
  if (const ClassInfo *Existing = FindOrNull(Name))
    return Existing->Id;

  ClassInfo Entry;
  Entry.Name = std::move(Name);
  Entry.StableKey = std::move(StableKey);
  Entry.Token = Token;
  Entry.Id = static_cast<TypeId>(Classes.size() + 1);

  Classes.push_back(std::move(Entry));
  ClassInfo &Stored = Classes.back();

  ByName.emplace(std::string_view(Stored.Name), Stored.Id);
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

void Model::AddSymbol(std::string_view Qualified, SymbolKind Kind) {
  for (const PendingSymbol &Existing : SymbolEntries) {
    if (Existing.Qualified == Qualified && Existing.Kind == Kind)
      return;
  }
  SymbolEntries.push_back(PendingSymbol{std::string(Qualified), Kind});
  SymbolsDirty = true;
}

std::shared_ptr<const std::vector<Symbol>> Model::Snapshot() const {
  if (!SymbolsDirty && CachedSymbols)
    return CachedSymbols;

  auto Built = std::make_shared<std::vector<Symbol>>();
  Built->reserve(SymbolEntries.size());

  for (const PendingSymbol &Entry : SymbolEntries) {
    const std::size_t Dot = Entry.Qualified.rfind('.');
    const std::string Short =
        Dot == std::string::npos ? Entry.Qualified
                                 : Entry.Qualified.substr(Dot + 1);
    Built->emplace_back(Short, Entry.Qualified, Entry.Kind);
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

  // Resolve segment by segment from the globals table, creating as needed.
  PushRoot(L);

  std::size_t Start = 0;
  while (Start <= Path.size()) {
    const std::size_t Dot = Path.find('.', Start);
    const std::string_view Segment =
        Dot == std::string_view::npos
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

    lua_remove(L, -2); // drop the parent, keep the child on top

    if (Dot == std::string_view::npos)
      break;
    Start = Dot + 1;
  }

  // The table is on top; pin it and cache the ref. PinTop pops, so nothing is
  // left on the stack.
  const int Ref = luna::detail::PinTop(L);
  Namespaces.emplace(Intern(Path), Ref);
  AddSymbol(Path, SymbolKind::Namespace);
  return Ref;
}

} // namespace detail
} // namespace luna
