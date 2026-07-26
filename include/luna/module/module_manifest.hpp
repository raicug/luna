#pragma once

// Immutable load-once module manifests. A manifest is a Luna-owned value: a
// validated stable module identity, one parsed semantic version, normalized
// dependency constraints, documentation, and normalized exported symbol
// metadata. Versions and constraints are parsed once into structural records,
// so resolution never reparses text and never depends on registration order,
// locale, or unordered-container iteration. Nothing here refers to a State, a
// virtual machine, a stack index, or a native target.

// clang-format off
#include <luna/reflection/ids.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna {

// Deterministic reason one semantic-version text is accepted or rejected.
enum class SemanticVersionStatus {
  Valid,
  Empty,
  TooLong,
  MissingCore,
  InvalidNumber,
  LeadingZero,
  NumberOverflow,
  EmptyPrereleaseIdentifier,
  InvalidPrereleaseCharacter,
  EmptyBuildIdentifier,
  InvalidBuildCharacter
};

[[nodiscard]] std::string_view
SemanticVersionStatusText(SemanticVersionStatus Status) noexcept;

// One parsed semantic version. Precedence follows the standard rules: the core
// triple compares numerically, a prerelease sorts below its release, numeric
// prerelease identifiers compare numerically, alphanumeric identifiers compare
// lexically by ASCII, a larger identifier set sorts above a shorter prefix, and
// build metadata never participates in precedence.
class SemanticVersion {
public:
  // Explicit Luna-owned policy bound on one version text.
  static constexpr std::size_t MaximumLength = 256;

  // A default-constructed version is the reserved unspecified value: it has no
  // precedence and never satisfies a constraint.
  SemanticVersion() = default;

  [[nodiscard]] static SemanticVersionStatus Classify(std::string_view Text);

  [[nodiscard]] static SemanticVersion Parse(std::string_view Text,
                                             SemanticVersionStatus &Status);

  [[nodiscard]] static std::optional<SemanticVersion>
  TryParse(std::string_view Text);

  [[nodiscard]] bool IsValid() const noexcept { return SpecifiedValue; }

  [[nodiscard]] std::uint64_t Major() const noexcept { return MajorValue; }
  [[nodiscard]] std::uint64_t Minor() const noexcept { return MinorValue; }
  [[nodiscard]] std::uint64_t Patch() const noexcept { return PatchValue; }

  [[nodiscard]] const std::vector<std::string> &
  PrereleaseIdentifiers() const noexcept {
    return PrereleaseValues;
  }

  [[nodiscard]] const std::vector<std::string> &
  BuildIdentifiers() const noexcept {
    return BuildValues;
  }

  [[nodiscard]] bool IsPrerelease() const noexcept {
    return !PrereleaseValues.empty();
  }

  // Canonical text of this version, including build metadata when present.
  [[nodiscard]] std::string ToString() const;

  [[nodiscard]] std::size_t Hash() const;

  // Standard semantic-version precedence. Build metadata is ignored, so two
  // versions differing only in build metadata compare equal here.
  [[nodiscard]] static std::strong_ordering
  ComparePrecedence(const SemanticVersion &Left, const SemanticVersion &Right);

  [[nodiscard]] bool HasSamePrecedence(const SemanticVersion &Other) const;

  // Exact value equality, including build metadata. Manifest identity uses
  // exact equality so a rebuilt definition is never mistaken for the loaded
  // one, while resolution uses precedence.
  friend bool operator==(const SemanticVersion &Left,
                         const SemanticVersion &Right);

private:
  bool SpecifiedValue = false;
  std::uint64_t MajorValue = 0;
  std::uint64_t MinorValue = 0;
  std::uint64_t PatchValue = 0;
  std::vector<std::string> PrereleaseValues;
  std::vector<std::string> BuildValues;
};

// Comparison one dependency constraint applies to a candidate version.
enum class VersionComparator {
  Equal,
  NotEqual,
  Less,
  LessOrEqual,
  Greater,
  GreaterOrEqual
};

[[nodiscard]] std::string_view
VersionComparatorText(VersionComparator Comparator) noexcept;

// Deterministic reason one constraint text is accepted or rejected.
enum class VersionConstraintStatus {
  Valid,
  Empty,
  TooLong,
  InvalidComparator,
  InvalidVersion
};

[[nodiscard]] std::string_view
VersionConstraintStatusText(VersionConstraintStatus Status) noexcept;

// One parsed dependency constraint. Satisfaction is evaluated with standard
// precedence, so a prerelease candidate is compared by its prerelease rules
// rather than by text.
class VersionConstraint {
public:
  static constexpr std::size_t MaximumLength =
      SemanticVersion::MaximumLength + 2;

  // A default-constructed constraint is unspecified and satisfies nothing.
  VersionConstraint() = default;

  [[nodiscard]] static VersionConstraint Create(VersionComparator Comparator,
                                                SemanticVersion Version);

  [[nodiscard]] static VersionConstraintStatus Classify(std::string_view Text);

  [[nodiscard]] static VersionConstraint Parse(std::string_view Text,
                                               VersionConstraintStatus &Status);

  [[nodiscard]] static std::optional<VersionConstraint>
  TryParse(std::string_view Text);

  [[nodiscard]] bool IsValid() const noexcept;

  [[nodiscard]] VersionComparator Comparator() const noexcept {
    return ComparatorValue;
  }

  [[nodiscard]] const SemanticVersion &Version() const noexcept {
    return VersionValue;
  }

  [[nodiscard]] bool IsSatisfiedBy(const SemanticVersion &Candidate) const;

  [[nodiscard]] std::string ToString() const;

  [[nodiscard]] std::size_t Hash() const;

  friend bool operator==(const VersionConstraint &Left,
                         const VersionConstraint &Right);

private:
  VersionComparator ComparatorValue = VersionComparator::Equal;
  SemanticVersion VersionValue;
};

// Canonical constraint order: comparator, version precedence, then exact
// version text as the final stable key.
[[nodiscard]] std::strong_ordering
CompareConstraint(const VersionConstraint &Left,
                  const VersionConstraint &Right);

// One declared dependency: the stable identity of the required module and every
// constraint the requiring manifest places on it.
struct ModuleDependency final {
  std::string Identity;
  std::vector<VersionConstraint> Constraints;
};

[[nodiscard]] bool operator==(const ModuleDependency &Left,
                              const ModuleDependency &Right);

// One exported symbol a module publishes. Names are canonical dot-separated
// qualified names and never carry an availability flag or a native target.
struct ModuleExport final {
  SymbolKind Kind = SymbolKind::Namespace;
  std::string Name;
  std::string Documentation;
};

[[nodiscard]] bool operator==(const ModuleExport &Left,
                              const ModuleExport &Right);

// Canonical export order: qualified name, then symbol kind.
[[nodiscard]] std::strong_ordering CompareExport(const ModuleExport &Left,
                                                 const ModuleExport &Right);

// Deterministic reason one manifest is accepted or rejected.
enum class ModuleManifestStatus {
  Valid,
  EmptyIdentity,
  IdentityTooLong,
  InvalidIdentity,
  InvalidVersion,
  InvalidDependencyIdentity,
  SelfDependency,
  MissingDependencyConstraint,
  InvalidDependencyConstraint,
  InvalidExportName,
  DuplicateExport
};

[[nodiscard]] std::string_view
ModuleManifestStatusText(ModuleManifestStatus Status) noexcept;

// One immutable module manifest. `Create` validates every field and normalizes
// the accepted result: dependencies are merged by identity and sorted, every
// constraint list is canonically ordered and deduplicated, and exports are
// canonically ordered. Two manifests describing the same definition therefore
// compare equal regardless of the order their author declared them in.
class ModuleManifest {
public:
  static constexpr std::size_t MaximumIdentityLength = 256;
  static constexpr char IdentitySeparator = '.';

  // A default-constructed manifest is unspecified and never valid.
  ModuleManifest() = default;

  [[nodiscard]] static ModuleManifestStatus
  ClassifyIdentity(std::string_view Text) noexcept;

  [[nodiscard]] static ModuleManifest
  Create(std::string Identity, SemanticVersion Version,
         std::vector<ModuleDependency> Dependencies, std::string Documentation,
         std::vector<ModuleExport> Exports, ModuleManifestStatus &Status);

  [[nodiscard]] static std::optional<ModuleManifest>
  TryCreate(std::string Identity, SemanticVersion Version,
            std::vector<ModuleDependency> Dependencies,
            std::string Documentation, std::vector<ModuleExport> Exports);

  [[nodiscard]] ModuleManifestStatus Status() const noexcept {
    return StatusValue;
  }

  [[nodiscard]] bool IsValid() const noexcept {
    return StatusValue == ModuleManifestStatus::Valid;
  }

  [[nodiscard]] const std::string &Identity() const noexcept {
    return IdentityValue;
  }

  [[nodiscard]] const SemanticVersion &Version() const noexcept {
    return VersionValue;
  }

  [[nodiscard]] const std::vector<ModuleDependency> &
  Dependencies() const noexcept {
    return DependencyValues;
  }

  [[nodiscard]] const std::string &Documentation() const noexcept {
    return DocumentationValue;
  }

  [[nodiscard]] const std::vector<ModuleExport> &Exports() const noexcept {
    return ExportValues;
  }

  // Canonical `Identity@Version` text used by module keys and dependency paths.
  [[nodiscard]] std::string Key() const;

  [[nodiscard]] const ModuleDependency *
  FindDependency(std::string_view Identity) const noexcept;

  [[nodiscard]] std::size_t Hash() const;

  // Equality of the normalized definition. Load-once idempotence accepts a
  // repeated registration only when this comparison holds.
  friend bool operator==(const ModuleManifest &Left,
                         const ModuleManifest &Right);

private:
  ModuleManifestStatus StatusValue = ModuleManifestStatus::EmptyIdentity;
  std::string IdentityValue;
  SemanticVersion VersionValue;
  std::vector<ModuleDependency> DependencyValues;
  std::string DocumentationValue;
  std::vector<ModuleExport> ExportValues;
};

// Canonical module order: stable identity, then version precedence, then exact
// version text.
[[nodiscard]] std::strong_ordering CompareManifest(const ModuleManifest &Left,
                                                   const ModuleManifest &Right);

} // namespace Luna
