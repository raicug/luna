// clang-format off
#include "state/module/load.hpp"

#include <luna/core/diagnostics/error_category.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/identity/symbol_descriptor.hpp"
#include "state/module/resolution.hpp"
#include "state/reflection/storage.hpp"
#include "state/registration/checks.hpp"
#include "state/registration/plan.hpp"
#include "state/registration/scope_plan.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {

std::string_view
ModuleDefinitionLibrary::AddStatusText(AddStatus Status) noexcept {
  switch (Status) {
  case AddStatus::Added:
    return "added";
  case AddStatus::Duplicate:
    return "duplicate";
  case AddStatus::InvalidManifest:
    return "invalid-manifest";
  case AddStatus::MissingRegistration:
    return "missing-registration";
  case AddStatus::ConflictingDefinition:
    return "conflicting-definition";
  }
  return "unknown";
}

ModuleDefinitionLibrary::AddStatus
ModuleDefinitionLibrary::Add(ModuleDefinition Definition) {
  if (!Definition.Manifest.IsValid())
    return AddStatus::InvalidManifest;
  if (!Definition.Registration.IsValid())
    return AddStatus::MissingRegistration;

  // The catalog decides availability: an identical definition of the same
  // precedence is a duplicate, an unequal one at the same precedence is a
  // conflict, and neither one replaces what is already available.
  const ModuleCatalog::AddStatus Added = Available.Add(Definition.Manifest);
  switch (Added) {
  case ModuleCatalog::AddStatus::Added:
    break;
  case ModuleCatalog::AddStatus::Duplicate:
    return AddStatus::Duplicate;
  case ModuleCatalog::AddStatus::InvalidManifest:
    return AddStatus::InvalidManifest;
  case ModuleCatalog::AddStatus::ConflictingDefinition:
    return AddStatus::ConflictingDefinition;
  }

  Definitions.push_back(std::move(Definition));
  return AddStatus::Added;
}

const ModuleDefinition *
ModuleDefinitionLibrary::Find(std::string_view Identity,
                              const SemanticVersion &Version) const {
  for (const ModuleDefinition &Definition : Definitions) {
    const ModuleManifest &Manifest = Definition.Manifest;
    if (Manifest.Identity() == Identity &&
        Manifest.Version().HasSamePrecedence(Version))
      return &Definition;
  }
  return nullptr;
}

std::string ModuleSubject(const ModuleManifest &Manifest) {
  return SubjectText(SymbolKindText(SymbolKind::Module), Manifest.Key());
}

std::string ModuleSubject(std::string_view Identity, std::string_view Version) {
  std::string Key(Identity);
  Key.push_back('@');
  Key.append(Version);
  return SubjectText(SymbolKindText(SymbolKind::Module), Key);
}

std::string ConstraintText(const std::vector<VersionConstraint> &Constraints) {
  std::string Text;
  for (const VersionConstraint &Constraint : Constraints) {
    if (!Text.empty())
      Text.append(", ");
    Text.append(Constraint.ToString());
  }
  if (Text.empty())
    Text.append("none");
  return Text;
}

ErrorDiagnostic ModuleResolutionDiagnostic(std::string_view Subject,
                                           const ModuleDiagnostic &Diagnostic) {
  return MalformedMetadataDiagnostic(Subject, Diagnostic.Message() + ".");
}

ErrorDiagnostic ModuleConflictDiagnostic(std::string_view Subject,
                                         std::string_view Reason) {
  return ErrorDiagnostic::Create(ErrorCategory::DuplicateGlobalName,
                                 "Cannot register " + std::string(Subject) +
                                     ": " + std::string(Reason));
}

DescriptorPlanEntry
MakeModulePlanEntry(const ModuleManifest &Manifest,
                    const ModuleResolution &Resolution,
                    const ModuleContribution &Contribution) {
  DescriptorPlanEntry Entry;
  Entry.Category = PlanEntryKind::Module;

  // A module installs no virtual-machine value of its own: its exported symbols
  // install themselves. Its canonical name is the manifest identity, which is
  // also the journal key of its module overlay.
  Entry.VmPath = Manifest.Identity();
  Entry.Symbol =
      MakeModuleSymbol(Manifest.Identity(), SymbolId(), ProvenanceOf(Manifest));
  if (const auto Identity =
          SymbolIdentityRegistry::ComputeIdentity(Entry.Symbol))
    Entry.Identity = *Identity;

  ReflectionRecordFields Record;
  Record.Kind = SymbolKind::Module;
  Record.Id = Entry.Identity;
  Record.Name = std::string(FinalSegment(Manifest.Identity()));
  Record.QualifiedName = Manifest.Identity();
  Record.Scope = ScopeId::Root();
  Record.Declaration = Entry.Identity;
  Record.Returns = ReturnShape::Zero;
  Record.Documentation = Manifest.Documentation();
  Entry.Record = std::move(Record);

  ReflectionModuleFields Module;
  Module.Identity = Manifest.Identity();
  Module.Version = Manifest.Version().ToString();
  Module.Symbol = Entry.Identity;
  Module.Documentation = Manifest.Documentation();

  // Every declared dependency together with the version resolution selected for
  // it, so module reflection reports resolved versions rather than constraints
  // alone.
  for (const ModuleDependency &Dependency : Manifest.Dependencies()) {
    ReflectionModuleDependencyFields Fields;
    Fields.Identity = Dependency.Identity;
    Fields.Constraints = ConstraintText(Dependency.Constraints);
    if (const ModuleSelection *Selected = Resolution.Find(Dependency.Identity))
      Fields.Version = Selected->Version.ToString();
    Module.Dependencies.push_back(std::move(Fields));
  }

  for (const ModuleExport &Exported : Manifest.Exports()) {
    ReflectionModuleExportFields Fields;
    Fields.Kind = Exported.Kind;
    Fields.Name = Exported.Name;
    Fields.Documentation = Exported.Documentation;
    Module.Exports.push_back(std::move(Fields));
  }

  Module.Namespaces = Contribution.Namespaces;
  Module.Types = Contribution.Types;
  Entry.ModuleFields = std::move(Module);
  return Entry;
}

} // namespace Luna::Detail
