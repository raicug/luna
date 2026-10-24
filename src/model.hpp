#pragma once

// luna : the committed model. Private.
//
// Everything here is plain data plus the VM references that describe it. It is
// the only place the model mutates, and it is never visible to a consumer.

#include <luna/bound.hpp>
#include <luna/fwd.hpp>

#include <lua.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace luna {
namespace detail {

// Object, ObjectTag and kObjectMagic live in luna/convert.hpp, because the hot
// read path inspects the block inline.

struct ClassInfo {
  std::string Name;
  std::string StableKey;
  TypeId Id = NoType;
  const void *Token = nullptr;

  int MetatableRef = LUA_NOREF;
  int MembersRef = LUA_NOREF; // name -> method closure, or a { getter } wrapper
  int SettersRef = LUA_NOREF; // name -> property/field setter closure

  // ClassOperator::Index fallback, captured as the fourth upvalue of __index.
  Target IndexFallback;
};

class Model {
public:
  explicit Model(lua_State *VM);
  ~Model();

  Model(const Model &) = delete;
  Model &operator=(const Model &) = delete;

  [[nodiscard]] lua_State *VM() const noexcept { return L; }
  void Attach(lua_State *VM) noexcept { L = VM; }

  // Resolves a per-type token to a class id, or NoType when the class this
  // State knows has not been registered.
  [[nodiscard]] TypeId ClassIdForToken(const void *Token) const noexcept;
  [[nodiscard]] const ClassInfo *Class(TypeId Id) const noexcept;
  [[nodiscard]] ClassInfo *Class(TypeId Id) noexcept;

  TypeId AddClass(std::string Name, std::string StableKey, const void *Token);

  // Interned so a view survives pool growth.
  std::string_view Intern(std::string_view Name);

  void AddSymbol(std::string_view Qualified, SymbolKind Kind);
  [[nodiscard]] std::shared_ptr<const std::vector<Symbol>> Snapshot() const;

  // Returns the registry ref of the table at a dotted path, creating it and
  // linking it into its parent if needed.
  int EnsureNamespace(std::string_view Path, std::string &Error);

  [[nodiscard]] ClassInfo *FindOrNull(std::string_view Name) noexcept;

  // Keeps a published value's lifetime control block alive for as long as the
  // state is, so a borrowed instance can hold a raw pointer to it.
  void RememberLifetime(std::shared_ptr<LifetimeHandle::Control> Control) {
    if (Control)
      Lifetimes.push_back(std::move(Control));
  }

private:
  lua_State *L = nullptr;
  std::deque<std::string> Names;
  std::unordered_map<std::string_view, int> Interned;

  std::vector<ClassInfo> Classes; // index + 1 = TypeId
  std::unordered_map<std::string_view, TypeId> ByName;
  std::unordered_map<const void *, TypeId> ByToken;

  struct PendingSymbol {
    std::string Qualified;
    SymbolKind Kind = SymbolKind::None;
  };
  std::vector<PendingSymbol> SymbolEntries;
  mutable std::shared_ptr<const std::vector<Symbol>> CachedSymbols;
  mutable bool SymbolsDirty = true;

  std::unordered_map<std::string_view, int> Namespaces;

  // Keeps a published value's lifetime control block alive for as long as the
  // state is, so a borrowed instance can hold a raw pointer to it.
  std::vector<std::shared_ptr<LifetimeHandle::Control>> Lifetimes;
};

} // namespace detail
} // namespace luna
