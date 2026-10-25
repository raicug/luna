#pragma once

// The committed model: plain data plus the VM references that describe it.

#include <luna/bound.hpp>
#include <luna/fwd.hpp>

#include <lua.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace luna {
namespace detail {
struct ClassInfo {
  std::string Name;
  std::string StableKey;
  // Ties instances a bound function returns by pointer or reference to a
  // handle.
  std::shared_ptr<LifetimeHandle::Control> Lifetime;
  std::vector<std::string_view> Aliases;
  TypeId Id = NoType;
  TypeId BaseId = NoType;
  std::ptrdiff_t BaseOffset = 0;
  const void *Token = nullptr;
  int MetatableRef = LUA_NOREF;
  int MembersRef = LUA_NOREF;
  int PublicRef = LUA_NOREF;
  int SettersRef = LUA_NOREF;
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

  [[nodiscard]] TypeId ClassIdForToken(const void *Token) const noexcept;
  [[nodiscard]] const ClassInfo *Class(TypeId Id) const noexcept;
  [[nodiscard]] ClassInfo *Class(TypeId Id) noexcept;
  TypeId AddClass(std::string Name, std::string StableKey, const void *Token);
  std::string_view Intern(std::string_view Name);
  void AddSymbol(std::string_view Qualified, SymbolKind Kind, int Arity = -1,
                 int Required = -1);
  // Mirrors a base class's members onto a derived class for reflection.
  void InheritSymbols(std::string_view BaseName, std::string_view DerivedName);
  [[nodiscard]] std::shared_ptr<const std::vector<Symbol>> Snapshot() const;
  int EnsureNamespace(std::string_view Path, std::string &Error);
  [[nodiscard]] ClassInfo *FindOrNull(std::string_view Name) noexcept;

  [[nodiscard]] bool DerivesFrom(TypeId From,
                                 std::string_view Name) const noexcept;

  // Adjusts a derived instance pointer to its Ancestor subobject, when it has
  // one.
  [[nodiscard]] bool RebaseTo(TypeId From, TypeId Ancestor,
                              void *&Pointer) const noexcept;

  void Release() noexcept;

  // A commit marks the model, logs what it changes, and either seals the log or
  // rolls everything back, so a commit that fails half way publishes nothing.
  struct Checkpoint {
    std::size_t Symbols = 0;
    std::size_t Classes = 0;
    std::size_t Lifetimes = 0;
  };
  [[nodiscard]] Checkpoint Mark() const noexcept;
  void Rollback(const Checkpoint &Before);
  void Seal() noexcept;

  // Class tables are read-only to scripts between commits. A commit lifts that
  // while it writes, and puts it back after.
  void LockClasses(bool Readonly);
  void RememberLifetimeSet(const ClassInfo &Info) {
    LifetimeSetLog_.push_back(Info.Id);
  }

  // Records what Table[Key] holds now. TableIndex is an absolute stack index.
  void RememberSlot(int TableIndex, std::string_view Key);
  void RememberBase(const ClassInfo &Info);

  // One entry per registration: a second callable under a name becomes an
  // overload set.
  std::vector<Target> &CallablesAt(std::string_view Qualified) {
    const std::string_view Name = Intern(Qualified);
    std::vector<Target> &List = Callables[Name];
    CallableLog_.emplace_back(Name, List.size());
    return List;
  }

  void RememberLifetime(std::shared_ptr<LifetimeHandle::Control> Control) {
    if (Control)
      Lifetimes.push_back(std::move(Control));
  }

private:
  lua_State *L = nullptr;
  std::deque<std::string> Names;
  std::unordered_map<std::string_view, int> Interned;
  std::deque<ClassInfo> Classes;
  std::unordered_map<std::string_view, TypeId> ByName;
  std::unordered_map<const void *, TypeId> ByToken;
  struct PendingSymbol {
    std::string Qualified;
    SymbolKind Kind = SymbolKind::None;
    int Arity = -1;
    int Required = -1;
  };
  std::vector<PendingSymbol> SymbolEntries;
  // Qualified name to the entries that carry it, so adding a symbol is not a
  // scan.
  std::unordered_map<std::string, std::vector<std::size_t>> SymbolIndex;
  // Reflection may be read from another thread while a commit adds to the list.
  mutable std::mutex SymbolLock;
  struct SymbolEdit {
    std::size_t Index = 0;
    PendingSymbol Before;
  };
  struct SlotUndo {
    int Table = LUA_NOREF;
    std::string Key;
    int Previous = LUA_NOREF;
  };
  struct AliasUndo {
    TypeId Id = NoType;
    std::size_t Count = 0;
    std::string_view Alias;
  };
  struct BaseUndo {
    TypeId Id = NoType;
    TypeId Base = NoType;
    std::ptrdiff_t Offset = 0;
  };
  std::vector<SymbolEdit> SymbolEdits_;
  std::vector<SlotUndo> SlotLog_;
  std::unordered_map<const void *, int> LoggedTables_;
  std::vector<std::pair<std::string_view, std::size_t>> CallableLog_;
  std::vector<std::string_view> NamespaceLog_;
  std::vector<AliasUndo> AliasLog_;
  std::vector<BaseUndo> BaseLog_;
  std::vector<TypeId> LifetimeSetLog_;
  mutable std::shared_ptr<const std::vector<Symbol>> CachedSymbols;
  mutable bool SymbolsDirty = true;
  std::unordered_map<std::string_view, int> Namespaces;
  std::unordered_map<std::string_view, std::vector<Target>> Callables;
  std::vector<std::shared_ptr<LifetimeHandle::Control>> Lifetimes;
};
} // namespace detail
} // namespace luna
