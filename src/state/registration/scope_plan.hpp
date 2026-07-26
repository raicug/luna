#pragma once

// Namespace declarations as canonical plan entries, plus the private Luna
// ownership identity of every committed namespace table.
//
// A namespace is one reflected scope plus one Luna-owned table at an exact
// canonical path. Qualified names are built one validated identifier segment at
// a time and joined with the single canonical separator, so no segment ever
// needs escaping. A namespace declaration submits the same
// `DescriptorPlanEntry` schema every other category uses: the scope symbol, the
// exact virtual-machine path, and the reflection record of the scope.
//
// The ownership table is what makes reopening safe. A table may be reused as a
// namespace only when its recorded State identity, scope identity, qualified
// name, committed lifecycle generation, and table identity all agree with the
// request. A script-created table, a foreign table, a stale Luna table, or a
// symbol of another category at the same path is a collision, never an
// adoption.

// clang-format off
#include <luna/reflection/ids.hpp>

#include "state/identity/symbol_descriptor.hpp"
#include "state/registration/plan.hpp"
#include "state/transaction/lifecycle.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

// One staged namespace of a builder plan: the validated identifier segment the
// consumer asked for, the canonical qualified name it resolves to, and the
// declared documentation surface its reflection record publishes.
struct StagedNamespace final {
  std::string Segment;
  std::string QualifiedName;

  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;
};

// The canonical qualified name of `Segment` inside `Parent`. An empty parent
// yields the root-scope name, so the separator never leads or trails.
[[nodiscard]] std::string JoinQualifiedName(std::string_view Parent,
                                            std::string_view Segment);

// The parent path of one canonical qualified name, empty for a root-scope name.
[[nodiscard]] std::string_view
ParentQualifiedName(std::string_view QualifiedName) noexcept;

// The final canonical segment of one qualified name, which is the local name of
// its reflection record.
[[nodiscard]] std::string_view
FinalSegment(std::string_view QualifiedName) noexcept;

// One namespace declaration as a plan entry: the namespace scope symbol, the
// exact reflected table path, and the reflection record of the scope.
[[nodiscard]] DescriptorPlanEntry
MakeNamespacePlanEntry(std::string QualifiedName, SymbolId Parent);

// What the parent scope of one declaration resolves to inside an active
// transaction: the parent's canonical symbol identity, whether it is a
// Luna-owned scope of this State that still holds exactly the table its
// committed generation published, and whether that ownership belongs to the
// lifecycle generation the attempt captured.
struct ParentScopeResolution final {
  SymbolId Identity;
  bool IsOwned = true;
  bool IsCurrent = true;
};

// Private Luna ownership identity of one committed namespace table. None of it
// is reachable from a script: the record lives on Luna's side and the table is
// identified by the protected reference Luna retained for it.
struct NamespaceOwnership final {
  StateIdentity Identity;
  SymbolId Scope;
  std::string QualifiedName;
  std::uint64_t LifecycleGeneration = 0;

  // Identity of the retained table and the protected reference that keeps it
  // alive, so the identity can never be recycled by a later allocation.
  const void *Table = nullptr;
  int Reference = 0;
};

class NamespaceOwnershipTable final {
public:
  // Publication is the only caller: a namespace table is marked as Luna-owned
  // only once its transaction published it.
  void Record(NamespaceOwnership Ownership);

  [[nodiscard]] const NamespaceOwnership *
  Find(std::string_view QualifiedName) const noexcept;

  [[nodiscard]] std::size_t Size() const noexcept { return Records.size(); }

  [[nodiscard]] std::span<const NamespaceOwnership> All() const noexcept {
    return Records;
  }

  // True when `Ownership` describes exactly the requested committed namespace
  // of this State, including the table identity an ordinary query observes now.
  [[nodiscard]] static bool Matches(const NamespaceOwnership &Ownership,
                                    const StateIdentity &Identity,
                                    std::string_view QualifiedName,
                                    const SymbolId &Scope,
                                    const void *Table) noexcept;

  // True when the ownership belongs to the lifecycle generation the attempt
  // captured. A namespace published by a replaced generation is stale, so it is
  // never reopened and never adopted.
  [[nodiscard]] static bool
  IsCurrent(const NamespaceOwnership &Ownership,
            std::uint64_t LifecycleGeneration) noexcept;

private:
  std::vector<NamespaceOwnership> Records;
};

} // namespace Luna::Detail
