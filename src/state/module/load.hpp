#pragma once

// The load-once module load model: available module definitions, staged module
// requests, and the canonical plan entry one loaded module contributes.
//
// A consumer makes a dependency available by providing its definition - the
// immutable manifest plus the erased scoped registration callback - to the
// State. Providing a definition is pure Luna-side metadata: it touches no
// virtual-machine path, runs no callback, and publishes nothing. Loading is the
// separate operation: it resolves the graph rooted at one requested manifest
// against the available definitions plus the versions already loaded, then
// executes every not-yet-loaded callback of that graph, dependency-first in
// canonical order, inside one outermost registration transaction.
//
// That split is what keeps the single-transaction guarantee of Requirement 10.5
// reachable: because availability never runs a callback, one load can still run
// every dependency callback and the requested callback together, and a failure
// anywhere in the graph restores the exact pre-load State.

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

// One available module definition: what the module is, and how it registers.
struct ModuleDefinition final {
  ModuleManifest Manifest;
  ModuleRegistration Registration;
};

// One module load a builder staged. The parent scope is the canonical qualified
// name of the namespace the module was requested inside, empty at root scope.
struct StagedModule final {
  std::string ParentQualifiedName;
  ModuleManifest Manifest;
  ModuleRegistration Registration;
};

// Every module definition available to resolution in one State. The catalog
// half answers resolution questions; the definition half answers which callback
// belongs to one selected identity and version.
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

// Diagnostic subject of one module request: `module 'Identity@Version'`.
[[nodiscard]] std::string ModuleSubject(const ModuleManifest &Manifest);
[[nodiscard]] std::string ModuleSubject(std::string_view Identity,
                                        std::string_view Version);

// Canonical text of one dependency's declared constraints, for example
// `>=1.2.0, <2.0.0`, or `none` when the dependency declares none.
[[nodiscard]] std::string
ConstraintText(const std::vector<VersionConstraint> &Constraints);

// One resolution failure, including its canonical dependency path.
[[nodiscard]] ErrorDiagnostic
ModuleResolutionDiagnostic(std::string_view Subject,
                           const ModuleDiagnostic &Diagnostic);

// One conflicting definition or version of an already loaded module identity.
[[nodiscard]] ErrorDiagnostic ModuleConflictDiagnostic(std::string_view Subject,
                                                       std::string_view Reason);

// What one module contributed to the attempt: the canonical namespaces it
// declared and the canonical type names it declared.
struct ModuleContribution final {
  std::vector<std::string> Namespaces;
  std::vector<std::string> Types;
};

// One loaded module as a plan entry: the module scope symbol carrying its
// manifest provenance, its reflection record, and the canonical module
// enumeration of dependencies with resolved versions, exports, namespaces, and
// types. A module installs no virtual-machine value of its own, so its entry
// carries no path.
[[nodiscard]] DescriptorPlanEntry
MakeModulePlanEntry(const ModuleManifest &Manifest,
                    const ModuleResolution &Resolution,
                    const ModuleContribution &Contribution);

} // namespace Luna::Detail
