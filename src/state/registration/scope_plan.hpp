#pragma once

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

struct StagedNamespace final {
  std::string Segment;
  std::string QualifiedName;

  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;
};

[[nodiscard]] std::string JoinQualifiedName(std::string_view Parent,
                                            std::string_view Segment);

[[nodiscard]] std::string_view
ParentQualifiedName(std::string_view QualifiedName) noexcept;

[[nodiscard]] std::string_view
FinalSegment(std::string_view QualifiedName) noexcept;

[[nodiscard]] DescriptorPlanEntry
MakeNamespacePlanEntry(std::string QualifiedName, SymbolId Parent);

struct ParentScopeResolution final {
  SymbolId Identity;
  bool IsOwned = true;
  bool IsCurrent = true;
};

struct NamespaceOwnership final {
  StateIdentity Identity;
  SymbolId Scope;
  std::string QualifiedName;
  std::uint64_t LifecycleGeneration = 0;

  const void *Table = nullptr;
  int Reference = 0;
};

class NamespaceOwnershipTable final {
public:
  void Record(NamespaceOwnership Ownership);

  [[nodiscard]] const NamespaceOwnership *
  Find(std::string_view QualifiedName) const noexcept;

  [[nodiscard]] std::size_t Size() const noexcept { return Records.size(); }

  [[nodiscard]] std::span<const NamespaceOwnership> All() const noexcept {
    return Records;
  }

  [[nodiscard]] static bool Matches(const NamespaceOwnership &Ownership,
                                    const StateIdentity &Identity,
                                    std::string_view QualifiedName,
                                    const SymbolId &Scope,
                                    const void *Table) noexcept;

  [[nodiscard]] static bool
  IsCurrent(const NamespaceOwnership &Ownership,
            std::uint64_t LifecycleGeneration) noexcept;

private:
  std::vector<NamespaceOwnership> Records;
};

} // namespace Luna::Detail
