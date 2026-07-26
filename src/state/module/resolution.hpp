#pragma once

// Deterministic load-once module dependency resolution. Resolution accumulates
// constraints by stable module identity, visits identities in canonical sorted
// order, and selects the highest available version satisfying every accumulated
// constraint using standard semantic-version precedence. A missing dependency,
// an unsatisfied constraint, a conflicting selected version, and a cycle each
// produce one deterministic diagnostic carrying the canonical dependency path.
// Nothing here touches a State, a virtual machine, or a native target: this is
// the pure resolution model the transactional load of a later task consumes.

// clang-format off
#include <luna/module/module_manifest.hpp>

#include "state/identity/symbol_descriptor.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

// Deterministic outcome of one resolution attempt.
enum class ModuleResolutionStatus {
  Resolved,
  InvalidRequest,
  MissingDependency,
  UnsatisfiedConstraint,
  ConflictingSelection,
  DependencyCycle
};

[[nodiscard]] std::string_view
ModuleResolutionStatusText(ModuleResolutionStatus Status) noexcept;

// One canonical dependency path. Every entry except a still unselected tail is
// an `Identity@Version` key, so a path never depends on visit order.
struct ModuleDependencyPath final {
  std::vector<std::string> Keys;

  [[nodiscard]] std::string ToString() const;

  [[nodiscard]] friend bool
  operator==(const ModuleDependencyPath &Left,
             const ModuleDependencyPath &Right) = default;
};

// One deterministic resolution diagnostic.
struct ModuleDiagnostic final {
  ModuleResolutionStatus Status = ModuleResolutionStatus::Resolved;
  std::string Identity;
  std::string Detail;
  ModuleDependencyPath Path;

  [[nodiscard]] std::string Message() const;
};

// One resolved module: its stable identity and the selected version.
struct ModuleSelection final {
  std::string Identity;
  SemanticVersion Version;

  [[nodiscard]] std::string Key() const;
};

// One already loaded module. A pin behaves as an accumulated equality
// constraint, so a graph that needs a different version of a loaded module
// reports a conflicting selection instead of silently loading twice.
struct ModulePin final {
  std::string Identity;
  SemanticVersion Version;
};

// Every module version available to resolution. The catalog owns immutable
// manifests and keeps them in canonical order, so enumeration never depends on
// insertion order.
class ModuleCatalog final {
public:
  enum class AddStatus {
    Added,
    InvalidManifest,
    Duplicate,
    ConflictingDefinition
  };

  [[nodiscard]] AddStatus Add(ModuleManifest Manifest);

  [[nodiscard]] const ModuleManifest *
  Find(std::string_view Identity, const SemanticVersion &Version) const;

  // Every available version of one identity in ascending precedence order,
  // with exact version text as the final stable key.
  [[nodiscard]] std::vector<const ModuleManifest *>
  VersionsOf(std::string_view Identity) const;

  [[nodiscard]] bool Contains(std::string_view Identity) const;

  [[nodiscard]] std::vector<std::string> Identities() const;

  [[nodiscard]] std::size_t Count() const;

private:
  std::map<std::string, std::vector<ModuleManifest>, std::less<>> Entries;
};

// One complete resolution outcome. A rejected attempt selects nothing, so a
// caller can never observe a partially resolved graph.
struct ModuleResolution final {
  ModuleResolutionStatus Status = ModuleResolutionStatus::Resolved;
  std::vector<ModuleSelection> Selections;
  std::vector<std::string> LoadOrder;
  ModuleDiagnostic Diagnostic;

  [[nodiscard]] bool IsResolved() const noexcept {
    return Status == ModuleResolutionStatus::Resolved;
  }

  [[nodiscard]] const ModuleSelection *
  Find(std::string_view Identity) const noexcept;
};

// Resolves the graph rooted at one requested identity and version. Selections
// are returned in canonical identity order; `LoadOrder` lists the same
// selections dependency-first in canonical order, which is the order the
// transactional load of a later task executes callbacks in.
[[nodiscard]] ModuleResolution
ResolveModuleGraph(const ModuleCatalog &Catalog,
                   std::string_view RequestedIdentity,
                   const SemanticVersion &RequestedVersion,
                   const std::vector<ModulePin> &Pins);

[[nodiscard]] ModuleResolution
ResolveModuleGraph(const ModuleCatalog &Catalog, const ModuleManifest &Request);

// Canonical provenance of one manifest: the identity and version every
// module-owned declaration carries in symbol identity and reflection.
[[nodiscard]] ModuleProvenance ProvenanceOf(const ModuleManifest &Manifest);

// Canonical `Identity@Version` key of one identity and version pair.
[[nodiscard]] std::string ModuleKey(std::string_view Identity,
                                    const SemanticVersion &Version);

} // namespace Luna::Detail
