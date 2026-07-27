#pragma once

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

struct ModuleDependencyPath final {
  std::vector<std::string> Keys;

  [[nodiscard]] std::string ToString() const;

  [[nodiscard]] friend bool
  operator==(const ModuleDependencyPath &Left,
             const ModuleDependencyPath &Right) = default;
};

struct ModuleDiagnostic final {
  ModuleResolutionStatus Status = ModuleResolutionStatus::Resolved;
  std::string Identity;
  std::string Detail;
  ModuleDependencyPath Path;

  [[nodiscard]] std::string Message() const;
};

struct ModuleSelection final {
  std::string Identity;
  SemanticVersion Version;

  [[nodiscard]] std::string Key() const;
};

struct ModulePin final {
  std::string Identity;
  SemanticVersion Version;
};

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

  [[nodiscard]] std::vector<const ModuleManifest *>
  VersionsOf(std::string_view Identity) const;

  [[nodiscard]] bool Contains(std::string_view Identity) const;

  [[nodiscard]] std::vector<std::string> Identities() const;

  [[nodiscard]] std::size_t Count() const;

private:
  std::map<std::string, std::vector<ModuleManifest>, std::less<>> Entries;
};

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

[[nodiscard]] ModuleResolution
ResolveModuleGraph(const ModuleCatalog &Catalog,
                   std::string_view RequestedIdentity,
                   const SemanticVersion &RequestedVersion,
                   const std::vector<ModulePin> &Pins);

[[nodiscard]] ModuleResolution
ResolveModuleGraph(const ModuleCatalog &Catalog, const ModuleManifest &Request);

[[nodiscard]] ModuleProvenance ProvenanceOf(const ModuleManifest &Manifest);

[[nodiscard]] std::string ModuleKey(std::string_view Identity,
                                    const SemanticVersion &Version);

} // namespace Luna::Detail
