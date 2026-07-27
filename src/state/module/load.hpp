#pragma once

// clang-format off
#include <luna/binding/module_registration.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/module/module_manifest.hpp>

#include "state/module/resolution.hpp"
#include "state/registration/plan.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

struct ModuleDefinition final {
  ModuleManifest Manifest;
  ModuleRegistration Registration;
};

struct StagedModule final {
  std::string ParentQualifiedName;
  ModuleManifest Manifest;
  ModuleRegistration Registration;
};

class ModuleDefinitionLibrary final {
public:
  enum class AddStatus {
    Added,
    Duplicate,
    InvalidManifest,
    MissingRegistration,
    ConflictingDefinition
  };

  [[nodiscard]] static std::string_view
  AddStatusText(AddStatus Status) noexcept;

  [[nodiscard]] AddStatus Add(ModuleDefinition Definition);

  [[nodiscard]] const ModuleCatalog &Catalog() const noexcept {
    return Available;
  }

  [[nodiscard]] const ModuleDefinition *
  Find(std::string_view Identity, const SemanticVersion &Version) const;

  [[nodiscard]] std::size_t Count() const noexcept {
    return Definitions.size();
  }

private:
  ModuleCatalog Available;
  std::vector<ModuleDefinition> Definitions;
};

[[nodiscard]] std::string ModuleSubject(const ModuleManifest &Manifest);
[[nodiscard]] std::string ModuleSubject(std::string_view Identity,
                                        std::string_view Version);

[[nodiscard]] std::string
ConstraintText(const std::vector<VersionConstraint> &Constraints);

[[nodiscard]] ErrorDiagnostic
ModuleResolutionDiagnostic(std::string_view Subject,
                           const ModuleDiagnostic &Diagnostic);

[[nodiscard]] ErrorDiagnostic ModuleConflictDiagnostic(std::string_view Subject,
                                                       std::string_view Reason);

struct ModuleContribution final {
  std::vector<std::string> Namespaces;
  std::vector<std::string> Types;
};

[[nodiscard]] DescriptorPlanEntry
MakeModulePlanEntry(const ModuleManifest &Manifest,
                    const ModuleResolution &Resolution,
                    const ModuleContribution &Contribution);

} // namespace Luna::Detail
