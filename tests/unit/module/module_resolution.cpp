// clang-format off
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>

#include "state/module/registry.hpp"
#include "state/module/resolution.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Luna::ModuleDependency;
using Luna::ModuleExport;
using Luna::ModuleManifest;
using Luna::ModuleManifestStatus;
using Luna::SemanticVersion;
using Luna::SemanticVersionStatus;
using Luna::SymbolKind;
using Luna::VersionComparator;
using Luna::VersionConstraint;
using Luna::VersionConstraintStatus;
using Luna::Detail::ModuleCatalog;
using Luna::Detail::ModuleLifecycleStatus;
using Luna::Detail::ModuleLoadStatus;
using Luna::Detail::ModulePin;
using Luna::Detail::ModuleRegistry;
using Luna::Detail::ModuleResolution;
using Luna::Detail::ModuleResolutionStatus;
using Luna::Detail::ResolveModuleGraph;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "module resolution check failed: " << Description << '\n';
}

[[nodiscard]] SemanticVersion Version(std::string_view Text) {
  const std::optional<SemanticVersion> Parsed = SemanticVersion::TryParse(Text);
  if (!Parsed) {
    ++FailureCount;
    std::cerr << "module resolution check failed: unparsable test version '"
              << Text << "'\n";
    return SemanticVersion();
  }
  return *Parsed;
}

[[nodiscard]] VersionConstraint Constraint(std::string_view Text) {
  const std::optional<VersionConstraint> Parsed =
      VersionConstraint::TryParse(Text);
  if (!Parsed) {
    ++FailureCount;
    std::cerr << "module resolution check failed: unparsable test constraint '"
              << Text << "'\n";
    return VersionConstraint();
  }
  return *Parsed;
}

[[nodiscard]] ModuleDependency
Dependency(std::string Identity,
           const std::vector<std::string_view> &ConstraintTexts) {
  ModuleDependency Declared;
  Declared.Identity = std::move(Identity);
  for (const std::string_view Text : ConstraintTexts)
    Declared.Constraints.push_back(Constraint(Text));
  return Declared;
}

[[nodiscard]] ModuleManifest
Manifest(std::string Identity, std::string_view VersionText,
         std::vector<ModuleDependency> Dependencies,
         std::vector<ModuleExport> Exports = {},
         std::string Documentation = {}) {
  ModuleManifestStatus Status = ModuleManifestStatus::Valid;
  ModuleManifest Created = ModuleManifest::Create(
      std::move(Identity), Version(VersionText), std::move(Dependencies),
      std::move(Documentation), std::move(Exports), Status);
  if (Status != ModuleManifestStatus::Valid) {
    ++FailureCount;
    std::cerr << "module resolution check failed: invalid test manifest ("
              << Luna::ModuleManifestStatusText(Status) << ")\n";
  }
  return Created;
}

[[nodiscard]] ModuleManifestStatus
RejectedStatus(std::string Identity, SemanticVersion VersionValue,
               std::vector<ModuleDependency> Dependencies,
               std::vector<ModuleExport> Exports) {
  ModuleManifestStatus Status = ModuleManifestStatus::Valid;
  const ModuleManifest Created = ModuleManifest::Create(
      std::move(Identity), std::move(VersionValue), std::move(Dependencies),
      std::string(), std::move(Exports), Status);
  Check(Created.Status() == Status && !Created.IsValid(),
        "a rejected manifest reports its status and is never valid");
  return Status;
}

[[nodiscard]] ModuleExport Exported(std::string Name, SymbolKind Kind) {
  ModuleExport Declared;
  Declared.Name = std::move(Name);
  Declared.Kind = Kind;
  return Declared;
}

[[nodiscard]] bool AddToCatalog(ModuleCatalog &Catalog, ModuleManifest Value) {
  return Catalog.Add(std::move(Value)) == ModuleCatalog::AddStatus::Added;
}

[[nodiscard]] std::string SelectionText(const ModuleResolution &Resolution) {
  std::string Text;
  for (const auto &Selection : Resolution.Selections) {
    if (!Text.empty())
      Text.append(", ");
    Text.append(Selection.Key());
  }
  return Text;
}

[[nodiscard]] std::string LoadOrderText(const ModuleResolution &Resolution) {
  std::string Text;
  for (const std::string &Key : Resolution.LoadOrder) {
    if (!Text.empty())
      Text.append(", ");
    Text.append(Key);
  }
  return Text;
}

void CheckVersionParsing() {
  const std::array<std::pair<const char *, SemanticVersionStatus>, 11> Rejected{
      {{"", SemanticVersionStatus::Empty},
       {"1.2", SemanticVersionStatus::MissingCore},
       {"1.2.3.4", SemanticVersionStatus::MissingCore},
       {"1.2.x", SemanticVersionStatus::InvalidNumber},
       {"01.2.3", SemanticVersionStatus::LeadingZero},
       {"1.02.3", SemanticVersionStatus::LeadingZero},
       {"1.2.3-", SemanticVersionStatus::EmptyPrereleaseIdentifier},
       {"1.2.3-alpha..1", SemanticVersionStatus::EmptyPrereleaseIdentifier},
       {"1.2.3-al pha", SemanticVersionStatus::InvalidPrereleaseCharacter},
       {"1.2.3-01", SemanticVersionStatus::LeadingZero},
       {"1.2.3+", SemanticVersionStatus::EmptyBuildIdentifier}}};
  for (const auto &[Text, Status] : Rejected) {
    Check(SemanticVersion::Classify(Text) == Status,
          std::string("rejected version status for '").append(Text) + "'");
    Check(!SemanticVersion::TryParse(Text).has_value(),
          std::string("rejected version parse for '").append(Text) + "'");
  }

  const std::string TooLong(SemanticVersion::MaximumLength + 1, '1');
  Check(SemanticVersion::Classify(TooLong) == SemanticVersionStatus::TooLong,
        "an over-long version text is rejected");
  Check(SemanticVersion::Classify("99999999999999999999.0.0") ==
            SemanticVersionStatus::NumberOverflow,
        "an out-of-range core number is rejected");

  const SemanticVersion Simple = Version("1.2.3");
  Check(Simple.IsValid() && Simple.Major() == 1 && Simple.Minor() == 2 &&
            Simple.Patch() == 3,
        "a release version parses its core triple");
  Check(!Simple.IsPrerelease(), "a release version has no prerelease");
  Check(Simple.ToString() == "1.2.3", "a release version round-trips");

  const SemanticVersion Rich = Version("1.0.0-alpha.1+build.7");
  Check(Rich.IsPrerelease(), "a prerelease version reports itself");
  Check(Rich.PrereleaseIdentifiers().size() == 2 &&
            Rich.PrereleaseIdentifiers()[0] == "alpha" &&
            Rich.PrereleaseIdentifiers()[1] == "1",
        "prerelease identifiers are parsed in order");
  Check(Rich.BuildIdentifiers().size() == 2 &&
            Rich.BuildIdentifiers()[1] == "7",
        "build identifiers are parsed in order");
  Check(Rich.ToString() == "1.0.0-alpha.1+build.7",
        "a full version round-trips through its canonical text");

  const SemanticVersion Unspecified;
  Check(!Unspecified.IsValid(), "a default version is unspecified");
  Check(Unspecified.ToString().empty(), "an unspecified version has no text");
}

void CheckVersionPrecedence() {
  const std::array<const char *, 9> Ascending{
      "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta",
      "1.0.0-beta",  "1.0.0-beta.2",  "1.0.0-beta.11",
      "1.0.0-rc.1",  "1.0.0",         "1.0.1"};
  for (std::size_t Index = 1; Index < Ascending.size(); ++Index) {
    const SemanticVersion Lower = Version(Ascending[Index - 1]);
    const SemanticVersion Higher = Version(Ascending[Index]);
    Check(SemanticVersion::ComparePrecedence(Lower, Higher) ==
              std::strong_ordering::less,
          std::string("standard precedence orders ")
              .append(Ascending[Index - 1])
              .append(" below ")
              .append(Ascending[Index]));
    Check(SemanticVersion::ComparePrecedence(Higher, Lower) ==
              std::strong_ordering::greater,
          "precedence is antisymmetric");
  }

  const SemanticVersion First = Version("1.0.0+build.1");
  const SemanticVersion Second = Version("1.0.0+build.2");
  Check(First.HasSamePrecedence(Second),
        "build metadata is ignored for precedence");
  Check(!(First == Second),
        "exact equality still distinguishes build metadata");
  Check(Version("1.0.0") == Version("1.0.0"),
        "equal versions compare exactly equal");
  Check(SemanticVersion::ComparePrecedence(
            SemanticVersion(), Version("0.0.0")) == std::strong_ordering::less,
        "an unspecified version has the lowest precedence");
}

void CheckConstraints() {
  Check(Constraint(">=1.2.0").Comparator() == VersionComparator::GreaterOrEqual,
        "a >= constraint parses its comparator");
  Check(Constraint("1.2.0").Comparator() == VersionComparator::Equal,
        "a bare version constrains equality");
  Check(Constraint("=1.2.0") == Constraint("==1.2.0"),
        "both equality spellings parse to one constraint");
  Check(VersionConstraint::Classify("~1.2.0") ==
            VersionConstraintStatus::InvalidComparator,
        "an unknown comparator is rejected");
  Check(VersionConstraint::Classify(">=1.2") ==
            VersionConstraintStatus::InvalidVersion,
        "a malformed constrained version is rejected");
  Check(VersionConstraint::Classify("") == VersionConstraintStatus::Empty,
        "an empty constraint is rejected");
  Check(!VersionConstraint().IsValid(), "a default constraint is unspecified");
  Check(!VersionConstraint().IsSatisfiedBy(Version("1.0.0")),
        "an unspecified constraint satisfies nothing");

  Check(Constraint(">=1.0.0").IsSatisfiedBy(Version("1.0.0")),
        ">= accepts its own version");
  Check(!Constraint(">1.0.0").IsSatisfiedBy(Version("1.0.0")),
        "> rejects its own version");
  Check(Constraint(">=1.0.0").IsSatisfiedBy(Version("1.0.1-rc.1")),
        ">= accepts a higher-precedence prerelease");
  Check(!Constraint(">=1.0.0").IsSatisfiedBy(Version("1.0.0-rc.1")),
        "a prerelease sorts below its release for satisfaction");
  Check(Constraint("!=1.0.0").IsSatisfiedBy(Version("1.0.1")),
        "!= accepts a different version");
  Check(Constraint("==1.0.0").IsSatisfiedBy(Version("1.0.0+build.9")),
        "equality ignores build metadata");
  Check(Constraint("<=2.0.0").IsSatisfiedBy(Version("2.0.0")),
        "<= accepts its own version");
  Check(Constraint("<2.0.0").IsSatisfiedBy(Version("2.0.0-rc.1")),
        "< accepts a prerelease of the excluded release");
  Check(Constraint(">=1.0.0").ToString() == ">=1.0.0",
        "a constraint round-trips through its canonical text");
}

void CheckManifestValidationAndNormalization() {
  Check(RejectedStatus("", Version("1.0.0"), {}, {}) ==
            ModuleManifestStatus::EmptyIdentity,
        "an empty module identity is rejected");
  Check(RejectedStatus("studio..physics", Version("1.0.0"), {}, {}) ==
            ModuleManifestStatus::InvalidIdentity,
        "a malformed module identity is rejected");
  Check(RejectedStatus(
            std::string(ModuleManifest::MaximumIdentityLength + 1, 'a'),
            Version("1.0.0"), {}, {}) == ModuleManifestStatus::IdentityTooLong,
        "an over-long module identity is rejected");
  Check(RejectedStatus("studio.physics", SemanticVersion(), {}, {}) ==
            ModuleManifestStatus::InvalidVersion,
        "an unspecified manifest version is rejected");

  std::vector<ModuleDependency> SelfDependent;
  SelfDependent.push_back(Dependency("studio.physics", {">=1.0.0"}));
  Check(RejectedStatus("studio.physics", Version("1.0.0"),
                       std::move(SelfDependent),
                       {}) == ModuleManifestStatus::SelfDependency,
        "a self dependency is rejected");

  std::vector<ModuleDependency> Unconstrained;
  Unconstrained.push_back(Dependency("studio.math", {}));
  Check(RejectedStatus("studio.physics", Version("1.0.0"),
                       std::move(Unconstrained),
                       {}) == ModuleManifestStatus::MissingDependencyConstraint,
        "a dependency without constraints is rejected");

  std::vector<ModuleDependency> BadConstraint;
  ModuleDependency Declared;
  Declared.Identity = "studio.math";
  Declared.Constraints.push_back(VersionConstraint());
  BadConstraint.push_back(std::move(Declared));
  Check(RejectedStatus("studio.physics", Version("1.0.0"),
                       std::move(BadConstraint),
                       {}) == ModuleManifestStatus::InvalidDependencyConstraint,
        "an unparsed dependency constraint is rejected");

  std::vector<ModuleExport> BadExports;
  BadExports.push_back(Exported("1Bad", SymbolKind::Namespace));
  Check(RejectedStatus("studio.physics", Version("1.0.0"), {},
                       std::move(BadExports)) ==
            ModuleManifestStatus::InvalidExportName,
        "a malformed export name is rejected");

  std::vector<ModuleExport> Duplicates;
  Duplicates.push_back(Exported("Studio.Simulate", SymbolKind::OverloadSet));
  Duplicates.push_back(Exported("Studio.Simulate", SymbolKind::OverloadSet));
  Check(RejectedStatus("studio.physics", Version("1.0.0"), {},
                       std::move(Duplicates)) ==
            ModuleManifestStatus::DuplicateExport,
        "a duplicate export declaration is rejected");

  std::vector<ModuleDependency> FirstOrder;
  FirstOrder.push_back(Dependency("studio.math", {">=1.0.0"}));
  FirstOrder.push_back(Dependency("studio.core", {">=2.0.0", "<3.0.0"}));
  FirstOrder.push_back(Dependency("studio.math", {"<2.0.0"}));
  std::vector<ModuleExport> FirstExports;
  FirstExports.push_back(Exported("Studio.Vector", SymbolKind::Class));
  FirstExports.push_back(Exported("Studio.Simulate", SymbolKind::OverloadSet));

  std::vector<ModuleDependency> SecondOrder;
  SecondOrder.push_back(Dependency("studio.core", {"<3.0.0"}));
  SecondOrder.push_back(Dependency("studio.math", {"<2.0.0", ">=1.0.0"}));
  SecondOrder.push_back(Dependency("studio.core", {">=2.0.0"}));
  std::vector<ModuleExport> SecondExports;
  SecondExports.push_back(Exported("Studio.Simulate", SymbolKind::OverloadSet));
  SecondExports.push_back(Exported("Studio.Vector", SymbolKind::Class));

  const ModuleManifest First =
      Manifest("studio.physics", "1.0.0", std::move(FirstOrder),
               std::move(FirstExports), "physics bindings");
  const ModuleManifest Second =
      Manifest("studio.physics", "1.0.0", std::move(SecondOrder),
               std::move(SecondExports), "physics bindings");
  Check(First.IsValid() && Second.IsValid(),
        "both declaration orders produce a valid manifest");
  Check(First == Second,
        "normalization makes declaration order irrelevant to identity");
  Check(First.Hash() == Second.Hash(),
        "equal normalized manifests hash equally");
  Check(First.Dependencies().size() == 2 &&
            First.Dependencies()[0].Identity == "studio.core" &&
            First.Dependencies()[1].Identity == "studio.math",
        "dependencies are merged by identity and sorted");
  Check(First.Dependencies()[1].Constraints.size() == 2,
        "duplicate constraint declarations are deduplicated");
  Check(First.Exports().size() == 2 &&
            First.Exports()[0].Name == "Studio.Simulate",
        "exports are canonically ordered");
  Check(First.Key() == "studio.physics@1.0.0",
        "a manifest key is its identity and version");
  Check(First.FindDependency("studio.math") != nullptr &&
            First.FindDependency("studio.audio") == nullptr,
        "dependency lookup answers by identity");

  const ModuleManifest Documented =
      Manifest("studio.physics", "1.0.0", {}, {}, "other documentation");
  Check(!(Documented == Manifest("studio.physics", "1.0.0", {}, {}, "docs")),
        "documentation participates in normalized equality");
}

void CheckCatalogOrdering() {
  ModuleCatalog Catalog;
  Check(AddToCatalog(Catalog, Manifest("studio.math", "1.1.0", {})),
        "a catalog accepts a valid manifest");
  Check(AddToCatalog(Catalog, Manifest("studio.math", "1.0.0", {})),
        "a catalog accepts an older version");
  Check(AddToCatalog(Catalog, Manifest("studio.math", "1.2.0-rc.1", {})),
        "a catalog accepts a prerelease version");
  Check(Catalog.Add(Manifest("studio.math", "1.0.0", {})) ==
            ModuleCatalog::AddStatus::Duplicate,
        "an identical redeclaration is a benign duplicate");
  Check(Catalog.Add(
            Manifest("studio.math", "1.0.0", {},
                     {Exported("Studio.Add", SymbolKind::OverloadSet)})) ==
            ModuleCatalog::AddStatus::ConflictingDefinition,
        "a same-version unequal definition conflicts");

  ModuleManifestStatus Status = ModuleManifestStatus::Valid;
  const ModuleManifest Invalid =
      ModuleManifest::Create("", Version("1.0.0"), {}, {}, {}, Status);
  Check(Catalog.Add(Invalid) == ModuleCatalog::AddStatus::InvalidManifest,
        "a catalog rejects an invalid manifest");

  const std::vector<const ModuleManifest *> Versions =
      Catalog.VersionsOf("studio.math");
  Check(Versions.size() == 3 && Versions[0]->Version().ToString() == "1.0.0" &&
            Versions[1]->Version().ToString() == "1.1.0" &&
            Versions[2]->Version().ToString() == "1.2.0-rc.1",
        "catalog versions are ordered by precedence");
  Check(Catalog.Contains("studio.math") && !Catalog.Contains("studio.audio"),
        "catalog membership answers by identity");
  Check(Catalog.Count() == 3, "the catalog counts every available version");
  Check(Catalog.Find("studio.math", Version("1.1.0")) != nullptr &&
            Catalog.Find("studio.math", Version("9.9.9")) == nullptr,
        "catalog lookup answers by identity and version");
  Check(Catalog.Identities().size() == 1 &&
            Catalog.Identities()[0] == "studio.math",
        "catalog identities are enumerated canonically");
}

void CheckHighestSatisfyingResolution() {
  ModuleCatalog Catalog;
  std::vector<ModuleDependency> AppDependencies;
  AppDependencies.push_back(Dependency("studio.math", {">=1.0.0", "<2.0.0"}));
  AppDependencies.push_back(Dependency("studio.render", {">=1.0.0"}));
  Check(AddToCatalog(Catalog, Manifest("studio.app", "1.0.0",
                                       std::move(AppDependencies))),
        "the requested module is available");

  std::vector<ModuleDependency> RenderDependencies;
  RenderDependencies.push_back(Dependency("studio.math", {">=1.1.0"}));
  Check(AddToCatalog(Catalog, Manifest("studio.render", "1.4.0",
                                       std::move(RenderDependencies))),
        "a dependency module is available");
  Check(AddToCatalog(Catalog, Manifest("studio.math", "1.0.0", {})),
        "an old dependency version is available");
  Check(AddToCatalog(Catalog, Manifest("studio.math", "1.2.0", {})),
        "a satisfying dependency version is available");
  Check(AddToCatalog(Catalog, Manifest("studio.math", "2.0.0", {})),
        "an excluded dependency version is available");

  const ModuleResolution Resolution = ResolveModuleGraph(
      Catalog, "studio.app", Version("1.0.0"), std::vector<ModulePin>());
  Check(Resolution.IsResolved(), "a satisfiable graph resolves");
  Check(SelectionText(Resolution) ==
            "studio.app@1.0.0, studio.math@1.2.0, studio.render@1.4.0",
        "selections are canonically ordered highest satisfying versions");
  Check(LoadOrderText(Resolution) ==
            "studio.math@1.2.0, studio.render@1.4.0, studio.app@1.0.0",
        "the load order is dependency-first and canonical");
  Check(Resolution.Find("studio.math") != nullptr &&
            Resolution.Find("studio.audio") == nullptr,
        "resolution lookup answers by identity");

  ModuleCatalog Permuted;
  Check(AddToCatalog(Permuted, Manifest("studio.math", "2.0.0", {})),
        "the permuted catalog accepts the excluded version first");
  Check(AddToCatalog(Permuted, Manifest("studio.math", "1.2.0", {})),
        "the permuted catalog accepts the satisfying version");
  Check(AddToCatalog(Permuted, Manifest("studio.math", "1.0.0", {})),
        "the permuted catalog accepts the old version");
  std::vector<ModuleDependency> PermutedRender;
  PermutedRender.push_back(Dependency("studio.math", {">=1.1.0"}));
  Check(AddToCatalog(Permuted, Manifest("studio.render", "1.4.0",
                                        std::move(PermutedRender))),
        "the permuted catalog accepts the render module");
  std::vector<ModuleDependency> PermutedApp;
  PermutedApp.push_back(Dependency("studio.render", {">=1.0.0"}));
  PermutedApp.push_back(Dependency("studio.math", {"<2.0.0"}));
  PermutedApp.push_back(Dependency("studio.math", {">=1.0.0"}));
  Check(AddToCatalog(Permuted,
                     Manifest("studio.app", "1.0.0", std::move(PermutedApp))),
        "the permuted catalog accepts the requested module");

  const ModuleResolution Repeated = ResolveModuleGraph(
      Permuted, "studio.app", Version("1.0.0"), std::vector<ModulePin>());
  Check(Repeated.IsResolved(), "the permuted graph resolves");
  Check(SelectionText(Repeated) == SelectionText(Resolution),
        "permuted declaration order selects the same versions");
  Check(LoadOrderText(Repeated) == LoadOrderText(Resolution),
        "permuted declaration order produces the same load order");
}

void CheckPrereleaseSelection() {
  ModuleCatalog Catalog;
  std::vector<ModuleDependency> Dependencies;
  Dependencies.push_back(Dependency("studio.math", {">=1.0.0"}));
  Check(AddToCatalog(Catalog,
                     Manifest("studio.app", "1.0.0", std::move(Dependencies))),
        "the requested module is available");
  Check(AddToCatalog(Catalog, Manifest("studio.math", "1.0.0", {})),
        "a release dependency version is available");
  Check(AddToCatalog(Catalog, Manifest("studio.math", "1.1.0-rc.1", {})),
        "a higher-precedence prerelease is available");
  Check(AddToCatalog(Catalog, Manifest("studio.math", "1.1.0-rc.2", {})),
        "an even higher prerelease is available");

  const ModuleResolution Resolution = ResolveModuleGraph(
      Catalog, "studio.app", Version("1.0.0"), std::vector<ModulePin>());
  Check(Resolution.IsResolved(), "a prerelease graph resolves");
  const auto *Selected = Resolution.Find("studio.math");
  Check(Selected != nullptr && Selected->Version.ToString() == "1.1.0-rc.2",
        "the highest satisfying version uses prerelease precedence");
}

void CheckMissingAndUnsatisfiedDependencies() {
  ModuleCatalog Catalog;
  std::vector<ModuleDependency> Missing;
  Missing.push_back(Dependency("studio.ghost", {">=1.0.0"}));
  Check(AddToCatalog(Catalog,
                     Manifest("studio.app", "1.0.0", std::move(Missing))),
        "the requested module is available");

  const ModuleResolution Absent = ResolveModuleGraph(
      Catalog, "studio.app", Version("1.0.0"), std::vector<ModulePin>());
  Check(Absent.Status == ModuleResolutionStatus::MissingDependency,
        "an absent dependency is rejected");
  Check(Absent.Selections.empty(), "a rejected resolution selects nothing");
  Check(Absent.Diagnostic.Identity == "studio.ghost",
        "the diagnostic names the missing module");
  Check(Absent.Diagnostic.Path.ToString() == "studio.app@1.0.0 -> studio.ghost",
        "the missing-dependency diagnostic carries the canonical path");
  Check(Absent.Diagnostic.Message().find("studio.app@1.0.0 -> studio.ghost") !=
            std::string::npos,
        "the diagnostic message includes the dependency path");

  ModuleCatalog Unsatisfiable;
  std::vector<ModuleDependency> TooNew;
  TooNew.push_back(Dependency("studio.math", {">=2.0.0"}));
  Check(AddToCatalog(Unsatisfiable,
                     Manifest("studio.app", "1.0.0", std::move(TooNew))),
        "the requested module is available");
  Check(AddToCatalog(Unsatisfiable, Manifest("studio.math", "1.0.0", {})),
        "only an unsatisfying version is available");

  const ModuleResolution Unsatisfied = ResolveModuleGraph(
      Unsatisfiable, "studio.app", Version("1.0.0"), std::vector<ModulePin>());
  Check(Unsatisfied.Status == ModuleResolutionStatus::UnsatisfiedConstraint,
        "an unsatisfiable constraint is rejected");
  Check(Unsatisfied.Diagnostic.Path.ToString() ==
            "studio.app@1.0.0 -> studio.math",
        "the unsatisfied-constraint diagnostic carries the canonical path");
  Check(Unsatisfied.Diagnostic.Detail.find(">=2.0.0") != std::string::npos,
        "the diagnostic reports the accumulated constraint");

  const ModuleResolution Unknown =
      ResolveModuleGraph(Unsatisfiable, "studio.absent", Version("1.0.0"),
                         std::vector<ModulePin>());
  Check(Unknown.Status == ModuleResolutionStatus::InvalidRequest,
        "an unavailable requested module is rejected");
  const ModuleResolution Unspecified = ResolveModuleGraph(
      Unsatisfiable, "studio.app", SemanticVersion(), std::vector<ModulePin>());
  Check(Unspecified.Status == ModuleResolutionStatus::InvalidRequest,
        "an unspecified requested version is rejected");
}

void CheckConflictingSelections() {
  ModuleCatalog Catalog;
  std::vector<ModuleDependency> AppDependencies;
  AppDependencies.push_back(Dependency("studio.math", {">=1.0.0"}));
  AppDependencies.push_back(Dependency("studio.tools", {">=1.0.0"}));
  Check(AddToCatalog(Catalog, Manifest("studio.app", "1.0.0",
                                       std::move(AppDependencies))),
        "the requested module is available");
  Check(AddToCatalog(Catalog, Manifest("studio.math", "1.0.0", {})),
        "an older dependency version is available");
  Check(AddToCatalog(Catalog, Manifest("studio.math", "2.0.0", {})),
        "a newer dependency version is available");
  std::vector<ModuleDependency> ToolDependencies;
  ToolDependencies.push_back(Dependency("studio.math", {"==1.0.0"}));
  Check(AddToCatalog(Catalog, Manifest("studio.tools", "1.0.0",
                                       std::move(ToolDependencies))),
        "a pinning dependency module is available");

  const ModuleResolution Conflict = ResolveModuleGraph(
      Catalog, "studio.app", Version("1.0.0"), std::vector<ModulePin>());
  Check(Conflict.Status == ModuleResolutionStatus::ConflictingSelection,
        "a later constraint that invalidates a selection is a conflict");
  Check(Conflict.Diagnostic.Identity == "studio.math",
        "the conflict names the contested module");
  Check(Conflict.Diagnostic.Path.ToString() ==
            "studio.app@1.0.0 -> studio.math@2.0.0",
        "the conflict diagnostic carries the canonical path");
  Check(Conflict.Diagnostic.Detail.find("==1.0.0") != std::string::npos,
        "the conflict diagnostic names the unsatisfied constraint");

  ModuleCatalog Pinned;
  std::vector<ModuleDependency> Requires;
  Requires.push_back(Dependency("studio.math", {">=2.0.0"}));
  Check(AddToCatalog(Pinned,
                     Manifest("studio.app", "1.0.0", std::move(Requires))),
        "the requested module is available");
  Check(AddToCatalog(Pinned, Manifest("studio.math", "1.0.0", {})),
        "the loaded dependency version is available");
  Check(AddToCatalog(Pinned, Manifest("studio.math", "2.0.0", {})),
        "a newer dependency version is available");

  std::vector<ModulePin> Pins;
  Pins.push_back(ModulePin{"studio.math", Version("1.0.0")});
  const ModuleResolution Loaded =
      ResolveModuleGraph(Pinned, "studio.app", Version("1.0.0"), Pins);
  Check(Loaded.Status == ModuleResolutionStatus::ConflictingSelection,
        "a loaded version that cannot satisfy the graph is a conflict");
  Check(Loaded.Diagnostic.Path.ToString() ==
            "studio.app@1.0.0 -> studio.math@1.0.0",
        "the loaded-version conflict carries the canonical path");

  ModuleCatalog Compatible;
  std::vector<ModuleDependency> Flexible;
  Flexible.push_back(Dependency("studio.math", {">=1.0.0"}));
  Check(AddToCatalog(Compatible,
                     Manifest("studio.app", "1.0.0", std::move(Flexible))),
        "the requested module is available");
  Check(AddToCatalog(Compatible, Manifest("studio.math", "1.0.0", {})),
        "the loaded dependency version is available");
  Check(AddToCatalog(Compatible, Manifest("studio.math", "2.0.0", {})),
        "a newer dependency version is available");
  const ModuleResolution Reused =
      ResolveModuleGraph(Compatible, "studio.app", Version("1.0.0"), Pins);
  Check(Reused.IsResolved(), "a compatible loaded version resolves");
  const auto *Selection = Reused.Find("studio.math");
  Check(Selection != nullptr && Selection->Version.ToString() == "1.0.0",
        "load-once reuses the loaded version instead of loading another");
}

void CheckDependencyCycles() {
  ModuleCatalog Catalog;
  std::vector<ModuleDependency> First;
  First.push_back(Dependency("studio.beta", {"==1.0.0"}));
  Check(AddToCatalog(Catalog,
                     Manifest("studio.alpha", "1.0.0", std::move(First))),
        "the first cycle member is available");
  std::vector<ModuleDependency> Second;
  Second.push_back(Dependency("studio.alpha", {"==1.0.0"}));
  Check(AddToCatalog(Catalog,
                     Manifest("studio.beta", "1.0.0", std::move(Second))),
        "the second cycle member is available");

  const ModuleResolution Cycle = ResolveModuleGraph(
      Catalog, "studio.alpha", Version("1.0.0"), std::vector<ModulePin>());
  Check(Cycle.Status == ModuleResolutionStatus::DependencyCycle,
        "a dependency cycle is rejected");
  Check(Cycle.Selections.empty(), "a rejected cycle selects nothing");
  Check(Cycle.Diagnostic.Path.ToString() ==
            "studio.alpha@1.0.0 -> studio.beta@1.0.0 -> studio.alpha@1.0.0",
        "the cycle diagnostic carries the canonical dependency path");

  const ModuleResolution FromOther = ResolveModuleGraph(
      Catalog, "studio.beta", Version("1.0.0"), std::vector<ModulePin>());
  Check(FromOther.Status == ModuleResolutionStatus::DependencyCycle,
        "the cycle is rejected from either entry point");
  Check(FromOther.Diagnostic.Path.ToString() ==
            "studio.alpha@1.0.0 -> studio.beta@1.0.0 -> studio.alpha@1.0.0",
        "the cycle path is canonical regardless of the entry point");
}

void CheckLoadOnceRules() {
  ModuleRegistry Registry;
  const ModuleManifest Physics =
      Manifest("studio.physics", "1.0.0", {},
               {Exported("Studio.Simulate", SymbolKind::OverloadSet)},
               "physics bindings");

  const auto Initial = Registry.ClassifyLoad(Physics);
  Check(Initial.Status == ModuleLoadStatus::Loadable && Initial.RunsCallbacks(),
        "an unloaded module is loadable and runs its callbacks");
  Check(Registry.Record(Physics), "a loadable module is recorded");
  Check(Registry.Count() == 1 && Registry.IsLoaded("studio.physics"),
        "the registry retains the loaded module");

  std::vector<ModuleExport> RepeatedExports;
  RepeatedExports.push_back(
      Exported("Studio.Simulate", SymbolKind::OverloadSet));
  const ModuleManifest Repeated =
      Manifest("studio.physics", "1.0.0", {}, std::move(RepeatedExports),
               "physics bindings");
  const auto Idempotent = Registry.ClassifyLoad(Repeated);
  Check(Idempotent.Status == ModuleLoadStatus::AlreadyLoaded,
        "an identical redefinition is idempotent");
  Check(Idempotent.IsSuccess() && !Idempotent.RunsCallbacks(),
        "an idempotent load succeeds without rerunning callbacks");
  Check(!Registry.Record(Repeated), "an idempotent load records nothing new");
  Check(Registry.Count() == 1, "an idempotent load leaves the registry intact");

  const ModuleManifest Changed = Manifest(
      "studio.physics", "1.0.0", {},
      {Exported("Studio.Step", SymbolKind::OverloadSet)}, "physics bindings");
  const auto Unequal = Registry.ClassifyLoad(Changed);
  Check(Unequal.Status == ModuleLoadStatus::ConflictingDefinition &&
            !Unequal.IsSuccess(),
        "a same-version unequal definition conflicts");
  Check(!Registry.Record(Changed) && Registry.Count() == 1,
        "a conflicting definition never replaces the loaded module");

  const ModuleManifest Newer =
      Manifest("studio.physics", "1.1.0", {},
               {Exported("Studio.Simulate", SymbolKind::OverloadSet)},
               "physics bindings");
  const auto DifferentVersion = Registry.ClassifyLoad(Newer);
  Check(DifferentVersion.Status == ModuleLoadStatus::ConflictingVersion,
        "a different version of a loaded module conflicts");
  Check(!Registry.Record(Newer) && Registry.Count() == 1,
        "a conflicting version never replaces the loaded module");
  const ModuleManifest *Loaded = Registry.Find("studio.physics");
  Check(Loaded != nullptr && Loaded->Version().ToString() == "1.0.0",
        "the originally loaded definition stays active");

  ModuleManifestStatus Status = ModuleManifestStatus::Valid;
  const ModuleManifest Invalid =
      ModuleManifest::Create("", Version("1.0.0"), {}, {}, {}, Status);
  const auto Rejected = Registry.ClassifyLoad(Invalid);
  Check(Rejected.Status == ModuleLoadStatus::InvalidManifest,
        "an invalid manifest is rejected before loading");

  Check(Registry.LoadedModules().size() == 1 &&
            Registry.LoadedModules()[0]->Identity() == "studio.physics",
        "loaded modules are enumerated canonically");
  const std::vector<ModulePin> Pins = Registry.Pins();
  Check(Pins.size() == 1 && Pins[0].Identity == "studio.physics" &&
            Pins[0].Version.ToString() == "1.0.0",
        "loaded modules become resolution pins");
}

void CheckLoadOnlyLifecycleRejection() {
  ModuleRegistry Registry;
  const ModuleManifest Physics = Manifest("studio.physics", "1.0.0", {});
  Check(Registry.Record(Physics), "a module is loaded for the lifecycle check");

  const auto Unload = Registry.RequestUnload("studio.physics");
  Check(Unload.Status == ModuleLifecycleStatus::UnsupportedUnload &&
            !Unload.IsSupported(),
        "unload is deterministically unsupported");
  Check(Unload.Identity == "studio.physics",
        "the unload result names the requested module");
  Check(Unload.Message().find("unsupported-unload") != std::string::npos,
        "the unload message states the load-only outcome");

  const ModuleManifest Replacement = Manifest("studio.physics", "2.0.0", {});
  const auto Replace = Registry.RequestReplacement(Replacement);
  Check(Replace.Status == ModuleLifecycleStatus::UnsupportedReplacement &&
            !Replace.IsSupported(),
        "replacement is deterministically unsupported");
  Check(Replace.Message().find("unsupported-replacement") != std::string::npos,
        "the replacement message states the load-only outcome");

  const auto Missing = Registry.RequestUnload("studio.absent");
  Check(Missing.Status == ModuleLifecycleStatus::UnsupportedUnload,
        "an unknown unload request answers identically");

  Check(Registry.Count() == 1 && Registry.IsLoaded("studio.physics"),
        "no lifecycle request mutated the loaded set");
  const ModuleManifest *Loaded = Registry.Find("studio.physics");
  Check(Loaded != nullptr && Loaded->Version().ToString() == "1.0.0",
        "the loaded definition is unchanged after both requests");
}

void CheckProvenance() {
  const ModuleManifest Physics = Manifest("studio.physics", "1.2.3-rc.1", {});
  const Luna::Detail::ModuleProvenance Provenance =
      Luna::Detail::ProvenanceOf(Physics);
  Check(Provenance.IsValid(), "manifest provenance is complete");
  Check(Provenance.Identity == "studio.physics" &&
            Provenance.Version == "1.2.3-rc.1",
        "provenance carries the manifest identity and version");
  Check(Luna::Detail::ModuleKey("studio.physics", Version("1.2.3-rc.1")) ==
            Physics.Key(),
        "the canonical module key matches the manifest key");
}

} // namespace

int RunModuleResolutionTests() {
  FailureCount = 0;
  CheckVersionParsing();
  CheckVersionPrecedence();
  CheckConstraints();
  CheckManifestValidationAndNormalization();
  CheckCatalogOrdering();
  CheckHighestSatisfyingResolution();
  CheckPrereleaseSelection();
  CheckMissingAndUnsatisfiedDependencies();
  CheckConflictingSelections();
  CheckDependencyCycles();
  CheckLoadOnceRules();
  CheckLoadOnlyLifecycleRejection();
  CheckProvenance();
  return FailureCount == 0 ? 0 : 1;
}
