// clang-format off
#include "state/module/resolution.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] bool ManifestPrecedes(const ModuleManifest &Left,
                                    const ModuleManifest &Right) {
  return CompareManifest(Left, Right) == std::strong_ordering::less;
}

[[nodiscard]] bool
SatisfiesEvery(const SemanticVersion &Candidate,
               const std::vector<VersionConstraint> &Constraints) {
  for (const VersionConstraint &Constraint : Constraints) {
    if (!Constraint.IsSatisfiedBy(Candidate))
      return false;
  }
  return true;
}

[[nodiscard]] const VersionConstraint *
FirstViolatedConstraint(const SemanticVersion &Candidate,
                        const std::vector<VersionConstraint> &Constraints) {
  for (const VersionConstraint &Constraint : Constraints) {
    if (!Constraint.IsSatisfiedBy(Candidate))
      return &Constraint;
  }
  return nullptr;
}

[[nodiscard]] std::string
DescribeConstraints(const std::vector<VersionConstraint> &Constraints) {
  std::string Text;
  for (std::size_t Index = 0; Index < Constraints.size(); ++Index) {
    if (Index != 0)
      Text.append(", ");
    Text.append(Constraints[Index].ToString());
  }
  if (Text.empty())
    Text.append("none");
  return Text;
}

[[nodiscard]] std::string
DescribeVersions(const std::vector<const ModuleManifest *> &Candidates) {
  std::string Text;
  for (std::size_t Index = 0; Index < Candidates.size(); ++Index) {
    if (Index != 0)
      Text.append(", ");
    Text.append(Candidates[Index]->Version().ToString());
  }
  if (Text.empty())
    Text.append("none");
  return Text;
}

struct ConstraintRecord final {
  std::vector<VersionConstraint> Constraints;
  ModuleDependencyPath Origin;
};

[[nodiscard]] ModuleResolution MakeFailure(ModuleResolutionStatus Status,
                                           std::string Identity,
                                           std::string Detail,
                                           ModuleDependencyPath Path) {
  ModuleResolution Resolution;
  Resolution.Status = Status;
  Resolution.Diagnostic.Status = Status;
  Resolution.Diagnostic.Identity = std::move(Identity);
  Resolution.Diagnostic.Detail = std::move(Detail);
  Resolution.Diagnostic.Path = std::move(Path);
  return Resolution;
}

[[nodiscard]] ModuleDependencyPath
ExtendPath(const ModuleDependencyPath &Origin, std::string Tail) {
  ModuleDependencyPath Path = Origin;
  Path.Keys.push_back(std::move(Tail));
  return Path;
}

[[nodiscard]] bool
FindCycle(const ModuleCatalog &Catalog,
          const std::map<std::string, SemanticVersion> &Selected,
          const std::string &Identity, std::vector<std::string> &Stack,
          std::set<std::string> &Active, std::set<std::string> &Finished,
          ModuleDependencyPath &Cycle) {
  const auto Selection = Selected.find(Identity);
  if (Selection == Selected.end())
    return false;

  Stack.push_back(ModuleKey(Identity, Selection->second));
  Active.insert(Identity);

  const ModuleManifest *Manifest = Catalog.Find(Identity, Selection->second);
  if (Manifest != nullptr) {
    for (const ModuleDependency &Dependency : Manifest->Dependencies()) {
      if (Active.count(Dependency.Identity) != 0) {
        Cycle.Keys = Stack;
        const auto Repeated = Selected.find(Dependency.Identity);
        Cycle.Keys.push_back(
            Repeated == Selected.end()
                ? Dependency.Identity
                : ModuleKey(Dependency.Identity, Repeated->second));
        return true;
      }
      if (Finished.count(Dependency.Identity) != 0)
        continue;
      if (FindCycle(Catalog, Selected, Dependency.Identity, Stack, Active,
                    Finished, Cycle))
        return true;
    }
  }

  Active.erase(Identity);
  Finished.insert(Identity);
  Stack.pop_back();
  return false;
}

void AppendLoadOrder(const ModuleCatalog &Catalog,
                     const std::map<std::string, SemanticVersion> &Selected,
                     const std::string &Identity,
                     std::set<std::string> &Visited,
                     std::vector<std::string> &Order) {
  if (Visited.count(Identity) != 0)
    return;
  Visited.insert(Identity);

  const auto Selection = Selected.find(Identity);
  if (Selection == Selected.end())
    return;

  const ModuleManifest *Manifest = Catalog.Find(Identity, Selection->second);
  if (Manifest != nullptr) {
    for (const ModuleDependency &Dependency : Manifest->Dependencies())
      AppendLoadOrder(Catalog, Selected, Dependency.Identity, Visited, Order);
  }
  Order.push_back(ModuleKey(Identity, Selection->second));
}

} // namespace

std::string_view
ModuleResolutionStatusText(ModuleResolutionStatus Status) noexcept {
  switch (Status) {
  case ModuleResolutionStatus::Resolved:
    return "resolved";
  case ModuleResolutionStatus::InvalidRequest:
    return "invalid-request";
  case ModuleResolutionStatus::MissingDependency:
    return "missing-dependency";
  case ModuleResolutionStatus::UnsatisfiedConstraint:
    return "unsatisfied-constraint";
  case ModuleResolutionStatus::ConflictingSelection:
    return "conflicting-selection";
  case ModuleResolutionStatus::DependencyCycle:
    return "dependency-cycle";
  }
  return "invalid";
}

std::string ModuleDependencyPath::ToString() const {
  std::string Text;
  for (std::size_t Index = 0; Index < Keys.size(); ++Index) {
    if (Index != 0)
      Text.append(" -> ");
    Text.append(Keys[Index]);
  }
  return Text;
}

std::string ModuleDiagnostic::Message() const {
  std::string Text("module resolution ");
  Text.append(ModuleResolutionStatusText(Status));
  if (!Identity.empty()) {
    Text.append(" for '");
    Text.append(Identity);
    Text.push_back('\'');
  }
  if (!Detail.empty()) {
    Text.append(": ");
    Text.append(Detail);
  }
  const std::string PathText = Path.ToString();
  if (!PathText.empty()) {
    Text.append(" (dependency path: ");
    Text.append(PathText);
    Text.push_back(')');
  }
  return Text;
}

std::string ModuleSelection::Key() const {
  return ModuleKey(Identity, Version);
}

ModuleCatalog::AddStatus ModuleCatalog::Add(ModuleManifest Manifest) {
  if (!Manifest.IsValid())
    return AddStatus::InvalidManifest;

  std::vector<ModuleManifest> &Versions = Entries[Manifest.Identity()];
  for (const ModuleManifest &Existing : Versions) {
    if (!Existing.Version().HasSamePrecedence(Manifest.Version()))
      continue;
    return Existing == Manifest ? AddStatus::Duplicate
                                : AddStatus::ConflictingDefinition;
  }

  Versions.push_back(std::move(Manifest));
  std::stable_sort(Versions.begin(), Versions.end(), ManifestPrecedes);
  return AddStatus::Added;
}

const ModuleManifest *
ModuleCatalog::Find(std::string_view Identity,
                    const SemanticVersion &Version) const {
  const auto Entry = Entries.find(Identity);
  if (Entry == Entries.end())
    return nullptr;
  for (const ModuleManifest &Manifest : Entry->second) {
    if (Manifest.Version().HasSamePrecedence(Version))
      return &Manifest;
  }
  return nullptr;
}

std::vector<const ModuleManifest *>
ModuleCatalog::VersionsOf(std::string_view Identity) const {
  std::vector<const ModuleManifest *> Candidates;
  const auto Entry = Entries.find(Identity);
  if (Entry == Entries.end())
    return Candidates;
  Candidates.reserve(Entry->second.size());
  for (const ModuleManifest &Manifest : Entry->second)
    Candidates.push_back(&Manifest);
  return Candidates;
}

bool ModuleCatalog::Contains(std::string_view Identity) const {
  return Entries.find(Identity) != Entries.end();
}

std::vector<std::string> ModuleCatalog::Identities() const {
  std::vector<std::string> Names;
  Names.reserve(Entries.size());
  for (const auto &[Identity, Versions] : Entries)
    Names.push_back(Identity);
  return Names;
}

std::size_t ModuleCatalog::Count() const {
  std::size_t Total = 0;
  for (const auto &[Identity, Versions] : Entries)
    Total += Versions.size();
  return Total;
}

const ModuleSelection *
ModuleResolution::Find(std::string_view Identity) const noexcept {
  for (const ModuleSelection &Selection : Selections) {
    if (Selection.Identity == Identity)
      return &Selection;
  }
  return nullptr;
}

ModuleResolution ResolveModuleGraph(const ModuleCatalog &Catalog,
                                    std::string_view RequestedIdentity,
                                    const SemanticVersion &RequestedVersion,
                                    const std::vector<ModulePin> &Pins) {
  const std::string Requested(RequestedIdentity);
  if (ModuleManifest::ClassifyIdentity(Requested) !=
          ModuleManifestStatus::Valid ||
      !RequestedVersion.IsValid())
    return MakeFailure(ModuleResolutionStatus::InvalidRequest, Requested,
                       "the requested module identity or version is invalid",
                       ModuleDependencyPath());
  if (Catalog.Find(Requested, RequestedVersion) == nullptr)
    return MakeFailure(ModuleResolutionStatus::InvalidRequest, Requested,
                       "the requested module version is not available",
                       ExtendPath(ModuleDependencyPath(),
                                  ModuleKey(Requested, RequestedVersion)));

  std::map<std::string, SemanticVersion> PinnedVersions;
  for (const ModulePin &Pin : Pins) {
    if (Pin.Version.IsValid())
      PinnedVersions.insert_or_assign(Pin.Identity, Pin.Version);
  }

  std::map<std::string, ConstraintRecord> Accumulated;
  ConstraintRecord &Root = Accumulated[Requested];
  Root.Constraints.push_back(
      VersionConstraint::Create(VersionComparator::Equal, RequestedVersion));

  std::map<std::string, SemanticVersion> Selected;
  while (true) {
    for (const auto &[Identity, Version] : Selected) {
      const ConstraintRecord &Record = Accumulated[Identity];
      const VersionConstraint *Violated =
          FirstViolatedConstraint(Version, Record.Constraints);
      if (Violated == nullptr)
        continue;
      std::string Detail("selected version ");
      Detail.append(Version.ToString());
      Detail.append(" does not satisfy ");
      Detail.append(Violated->ToString());
      return MakeFailure(
          ModuleResolutionStatus::ConflictingSelection, Identity,
          std::move(Detail),
          ExtendPath(Record.Origin, ModuleKey(Identity, Version)));
    }

    const std::string *Next = nullptr;
    for (const auto &[Identity, Record] : Accumulated) {
      if (Selected.count(Identity) == 0) {
        Next = &Identity;
        break;
      }
    }
    if (Next == nullptr)
      break;

    const std::string Identity = *Next;
    const ConstraintRecord &Record = Accumulated[Identity];
    const std::vector<const ModuleManifest *> Candidates =
        Catalog.VersionsOf(Identity);
    if (Candidates.empty())
      return MakeFailure(ModuleResolutionStatus::MissingDependency, Identity,
                         "no version of the module is available",
                         ExtendPath(Record.Origin, Identity));

    const ModuleManifest *Chosen = nullptr;
    for (std::size_t Index = Candidates.size(); Index > 0; --Index) {
      const ModuleManifest *Candidate = Candidates[Index - 1];
      if (SatisfiesEvery(Candidate->Version(), Record.Constraints)) {
        Chosen = Candidate;
        break;
      }
    }
    if (Chosen == nullptr) {
      std::string Detail("no available version satisfies ");
      Detail.append(DescribeConstraints(Record.Constraints));
      Detail.append("; available versions: ");
      Detail.append(DescribeVersions(Candidates));
      return MakeFailure(ModuleResolutionStatus::UnsatisfiedConstraint,
                         Identity, std::move(Detail),
                         ExtendPath(Record.Origin, Identity));
    }

    const auto Pinned = PinnedVersions.find(Identity);
    if (Pinned != PinnedVersions.end()) {
      const SemanticVersion &LoadedVersion = Pinned->second;
      const VersionConstraint *Violated =
          FirstViolatedConstraint(LoadedVersion, Record.Constraints);
      if (Violated != nullptr) {
        std::string Detail("loaded version ");
        Detail.append(LoadedVersion.ToString());
        Detail.append(" does not satisfy ");
        Detail.append(Violated->ToString());
        return MakeFailure(
            ModuleResolutionStatus::ConflictingSelection, Identity,
            std::move(Detail),
            ExtendPath(Record.Origin, ModuleKey(Identity, LoadedVersion)));
      }
      const ModuleManifest *LoadedManifest =
          Catalog.Find(Identity, LoadedVersion);
      if (LoadedManifest == nullptr) {
        std::string Detail("loaded version ");
        Detail.append(LoadedVersion.ToString());
        Detail.append(" is not available for resolution");
        return MakeFailure(
            ModuleResolutionStatus::ConflictingSelection, Identity,
            std::move(Detail),
            ExtendPath(Record.Origin, ModuleKey(Identity, LoadedVersion)));
      }
      Chosen = LoadedManifest;
    }

    Selected.insert_or_assign(Identity, Chosen->Version());
    const ModuleDependencyPath ChildOrigin =
        ExtendPath(Record.Origin, ModuleKey(Identity, Chosen->Version()));
    for (const ModuleDependency &Dependency : Chosen->Dependencies()) {
      const bool IsNew = Accumulated.count(Dependency.Identity) == 0;
      ConstraintRecord &Child = Accumulated[Dependency.Identity];
      if (IsNew)
        Child.Origin = ChildOrigin;
      for (const VersionConstraint &Constraint : Dependency.Constraints)
        Child.Constraints.push_back(Constraint);
    }
  }

  ModuleDependencyPath Cycle;
  std::vector<std::string> Stack;
  std::set<std::string> Active;
  std::set<std::string> Finished;
  for (const auto &[Identity, Version] : Selected) {
    if (Finished.count(Identity) != 0)
      continue;
    if (FindCycle(Catalog, Selected, Identity, Stack, Active, Finished,
                  Cycle)) {
      return MakeFailure(ModuleResolutionStatus::DependencyCycle, Identity,
                         "the dependency graph contains a cycle", Cycle);
    }
    Stack.clear();
    Active.clear();
  }

  ModuleResolution Resolution;
  Resolution.Status = ModuleResolutionStatus::Resolved;
  Resolution.Selections.reserve(Selected.size());
  for (const auto &[Identity, Version] : Selected) {
    ModuleSelection Selection;
    Selection.Identity = Identity;
    Selection.Version = Version;
    Resolution.Selections.push_back(std::move(Selection));
  }

  std::set<std::string> Visited;
  AppendLoadOrder(Catalog, Selected, Requested, Visited, Resolution.LoadOrder);
  for (const auto &[Identity, Version] : Selected)
    AppendLoadOrder(Catalog, Selected, Identity, Visited, Resolution.LoadOrder);
  return Resolution;
}

ModuleResolution ResolveModuleGraph(const ModuleCatalog &Catalog,
                                    const ModuleManifest &Request) {
  return ResolveModuleGraph(Catalog, Request.Identity(), Request.Version(),
                            std::vector<ModulePin>());
}

ModuleProvenance ProvenanceOf(const ModuleManifest &Manifest) {
  ModuleProvenance Provenance;
  Provenance.Identity = Manifest.Identity();
  Provenance.Version = Manifest.Version().ToString();
  return Provenance;
}

std::string ModuleKey(std::string_view Identity,
                      const SemanticVersion &Version) {
  std::string Text(Identity);
  Text.push_back('@');
  Text.append(Version.ToString());
  return Text;
}

} // namespace Luna::Detail
