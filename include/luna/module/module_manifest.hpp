#pragma once

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

class SemanticVersion {
public:
  static constexpr std::size_t MaximumLength = 256;

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

  [[nodiscard]] std::string ToString() const;

  [[nodiscard]] std::size_t Hash() const;

  [[nodiscard]] static std::strong_ordering
  ComparePrecedence(const SemanticVersion &Left, const SemanticVersion &Right);

  [[nodiscard]] bool HasSamePrecedence(const SemanticVersion &Other) const;

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

enum class VersionConstraintStatus {
  Valid,
  Empty,
  TooLong,
  InvalidComparator,
  InvalidVersion
};

[[nodiscard]] std::string_view
VersionConstraintStatusText(VersionConstraintStatus Status) noexcept;

class VersionConstraint {
public:
  static constexpr std::size_t MaximumLength =
      SemanticVersion::MaximumLength + 2;

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

[[nodiscard]] std::strong_ordering
CompareConstraint(const VersionConstraint &Left,
                  const VersionConstraint &Right);

struct ModuleDependency final {
  std::string Identity;
  std::vector<VersionConstraint> Constraints;
};

[[nodiscard]] bool operator==(const ModuleDependency &Left,
                              const ModuleDependency &Right);

struct ModuleExport final {
  SymbolKind Kind = SymbolKind::Namespace;
  std::string Name;
  std::string Documentation;
};

[[nodiscard]] bool operator==(const ModuleExport &Left,
                              const ModuleExport &Right);

[[nodiscard]] std::strong_ordering CompareExport(const ModuleExport &Left,
                                                 const ModuleExport &Right);

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

class ModuleManifest {
public:
  static constexpr std::size_t MaximumIdentityLength = 256;
  static constexpr char IdentitySeparator = '.';

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

  [[nodiscard]] std::string Key() const;

  [[nodiscard]] const ModuleDependency *
  FindDependency(std::string_view Identity) const noexcept;

  [[nodiscard]] std::size_t Hash() const;

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

[[nodiscard]] std::strong_ordering CompareManifest(const ModuleManifest &Left,
                                                   const ModuleManifest &Right);

} // namespace Luna
