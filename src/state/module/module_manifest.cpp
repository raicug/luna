// clang-format off
#include <luna/module/module_manifest.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna {
namespace {

[[nodiscard]] bool IsDigit(char Character) noexcept {
  return Character >= '0' && Character <= '9';
}

[[nodiscard]] bool IsLetter(char Character) noexcept {
  return (Character >= 'A' && Character <= 'Z') ||
         (Character >= 'a' && Character <= 'z');
}

[[nodiscard]] bool IsIdentifierLeadingCharacter(char Character) noexcept {
  return IsLetter(Character) || Character == '_';
}

[[nodiscard]] bool IsIdentifierCharacter(char Character) noexcept {
  return IsIdentifierLeadingCharacter(Character) || IsDigit(Character);
}

[[nodiscard]] bool IsPrereleaseCharacter(char Character) noexcept {
  return IsLetter(Character) || IsDigit(Character) || Character == '-';
}

[[nodiscard]] bool IsAllDigits(std::string_view Text) noexcept {
  if (Text.empty())
    return false;
  for (const char Character : Text) {
    if (!IsDigit(Character))
      return false;
  }
  return true;
}

[[nodiscard]] std::vector<std::string_view> SplitDots(std::string_view Text) {
  std::vector<std::string_view> Segments;
  std::size_t Start = 0;
  while (true) {
    const std::size_t Separator = Text.find('.', Start);
    if (Separator == std::string_view::npos) {
      Segments.push_back(Text.substr(Start));
      break;
    }
    Segments.push_back(Text.substr(Start, Separator - Start));
    Start = Separator + 1;
  }
  return Segments;
}

[[nodiscard]] SemanticVersionStatus
ParseCoreNumber(std::string_view Text, std::uint64_t &Value) noexcept {
  if (Text.empty())
    return SemanticVersionStatus::MissingCore;
  if (!IsAllDigits(Text))
    return SemanticVersionStatus::InvalidNumber;
  if (Text.size() > 1 && Text.front() == '0')
    return SemanticVersionStatus::LeadingZero;

  constexpr std::uint64_t Limit = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t Accumulator = 0;
  for (const char Character : Text) {
    const std::uint64_t Digit = static_cast<std::uint64_t>(Character - '0');
    if (Accumulator > (Limit - Digit) / 10)
      return SemanticVersionStatus::NumberOverflow;
    Accumulator = Accumulator * 10 + Digit;
  }
  Value = Accumulator;
  return SemanticVersionStatus::Valid;
}

struct ParsedVersion final {
  std::uint64_t Major = 0;
  std::uint64_t Minor = 0;
  std::uint64_t Patch = 0;
  std::vector<std::string> Prerelease;
  std::vector<std::string> Build;
};

[[nodiscard]] SemanticVersionStatus ParseVersionText(std::string_view Text,
                                                     ParsedVersion &Parsed) {
  if (Text.empty())
    return SemanticVersionStatus::Empty;
  if (Text.size() > SemanticVersion::MaximumLength)
    return SemanticVersionStatus::TooLong;

  std::string_view Remaining = Text;
  std::string_view BuildText;
  const std::size_t BuildStart = Remaining.find('+');
  if (BuildStart != std::string_view::npos) {
    BuildText = Remaining.substr(BuildStart + 1);
    Remaining = Remaining.substr(0, BuildStart);
  }

  std::string_view PrereleaseText;
  const std::size_t PrereleaseStart = Remaining.find('-');
  if (PrereleaseStart != std::string_view::npos) {
    PrereleaseText = Remaining.substr(PrereleaseStart + 1);
    Remaining = Remaining.substr(0, PrereleaseStart);
  }

  const std::vector<std::string_view> CoreSegments = SplitDots(Remaining);
  if (CoreSegments.size() != 3)
    return SemanticVersionStatus::MissingCore;

  std::uint64_t Core[3] = {0, 0, 0};
  for (std::size_t Index = 0; Index < 3; ++Index) {
    const SemanticVersionStatus CoreStatus =
        ParseCoreNumber(CoreSegments[Index], Core[Index]);
    if (CoreStatus != SemanticVersionStatus::Valid)
      return CoreStatus;
  }

  std::vector<std::string> Prerelease;
  if (PrereleaseStart != std::string_view::npos) {
    for (const std::string_view Segment : SplitDots(PrereleaseText)) {
      if (Segment.empty())
        return SemanticVersionStatus::EmptyPrereleaseIdentifier;
      for (const char Character : Segment) {
        if (!IsPrereleaseCharacter(Character))
          return SemanticVersionStatus::InvalidPrereleaseCharacter;
      }
      if (IsAllDigits(Segment) && Segment.size() > 1 && Segment.front() == '0')
        return SemanticVersionStatus::LeadingZero;
      Prerelease.emplace_back(Segment);
    }
  }

  std::vector<std::string> Build;
  if (BuildStart != std::string_view::npos) {
    for (const std::string_view Segment : SplitDots(BuildText)) {
      if (Segment.empty())
        return SemanticVersionStatus::EmptyBuildIdentifier;
      for (const char Character : Segment) {
        if (!IsPrereleaseCharacter(Character))
          return SemanticVersionStatus::InvalidBuildCharacter;
      }
      Build.emplace_back(Segment);
    }
  }

  Parsed.Major = Core[0];
  Parsed.Minor = Core[1];
  Parsed.Patch = Core[2];
  Parsed.Prerelease = std::move(Prerelease);
  Parsed.Build = std::move(Build);
  return SemanticVersionStatus::Valid;
}

[[nodiscard]] std::strong_ordering CompareNumbers(std::uint64_t Left,
                                                  std::uint64_t Right) {
  if (Left < Right)
    return std::strong_ordering::less;
  if (Left > Right)
    return std::strong_ordering::greater;
  return std::strong_ordering::equal;
}

[[nodiscard]] std::strong_ordering CompareText(std::string_view Left,
                                               std::string_view Right) {
  const int Comparison = Left.compare(Right);
  if (Comparison < 0)
    return std::strong_ordering::less;
  if (Comparison > 0)
    return std::strong_ordering::greater;
  return std::strong_ordering::equal;
}

[[nodiscard]] std::strong_ordering
ComparePrereleaseIdentifier(std::string_view Left, std::string_view Right) {
  const bool LeftNumeric = IsAllDigits(Left);
  const bool RightNumeric = IsAllDigits(Right);
  if (LeftNumeric != RightNumeric)
    return LeftNumeric ? std::strong_ordering::less
                       : std::strong_ordering::greater;
  if (!LeftNumeric)
    return CompareText(Left, Right);
  if (Left.size() != Right.size())
    return Left.size() < Right.size() ? std::strong_ordering::less
                                      : std::strong_ordering::greater;
  return CompareText(Left, Right);
}

[[nodiscard]] std::size_t HashBytes(std::string_view Text) noexcept {
  std::uint64_t Accumulator = 0xcbf29ce484222325ULL;
  for (const char Character : Text) {
    Accumulator ^=
        static_cast<std::uint64_t>(static_cast<unsigned char>(Character));
    Accumulator *= 0x100000001b3ULL;
  }
  return static_cast<std::size_t>(Accumulator);
}

[[nodiscard]] std::size_t CombineHash(std::size_t Accumulator,
                                      std::size_t Value) noexcept {
  constexpr std::size_t Mixer = static_cast<std::size_t>(0x9e3779b97f4a7c15ULL);
  return Accumulator ^
         (Value + Mixer + (Accumulator << 6) + (Accumulator >> 2));
}

[[nodiscard]] ModuleManifestStatus
ClassifyIdentityText(std::string_view Text) noexcept {
  if (Text.empty())
    return ModuleManifestStatus::EmptyIdentity;
  if (Text.size() > ModuleManifest::MaximumIdentityLength)
    return ModuleManifestStatus::IdentityTooLong;

  bool AtSegmentStart = true;
  for (const char Character : Text) {
    if (Character == ModuleManifest::IdentitySeparator) {
      if (AtSegmentStart)
        return ModuleManifestStatus::InvalidIdentity;
      AtSegmentStart = true;
      continue;
    }
    if (AtSegmentStart) {
      if (!IsIdentifierLeadingCharacter(Character))
        return ModuleManifestStatus::InvalidIdentity;
      AtSegmentStart = false;
      continue;
    }
    if (!IsIdentifierCharacter(Character))
      return ModuleManifestStatus::InvalidIdentity;
  }
  if (AtSegmentStart)
    return ModuleManifestStatus::InvalidIdentity;
  return ModuleManifestStatus::Valid;
}

} // namespace

std::string_view
SemanticVersionStatusText(SemanticVersionStatus Status) noexcept {
  switch (Status) {
  case SemanticVersionStatus::Valid:
    return "valid";
  case SemanticVersionStatus::Empty:
    return "empty";
  case SemanticVersionStatus::TooLong:
    return "too-long";
  case SemanticVersionStatus::MissingCore:
    return "missing-core";
  case SemanticVersionStatus::InvalidNumber:
    return "invalid-number";
  case SemanticVersionStatus::LeadingZero:
    return "leading-zero";
  case SemanticVersionStatus::NumberOverflow:
    return "number-overflow";
  case SemanticVersionStatus::EmptyPrereleaseIdentifier:
    return "empty-prerelease-identifier";
  case SemanticVersionStatus::InvalidPrereleaseCharacter:
    return "invalid-prerelease-character";
  case SemanticVersionStatus::EmptyBuildIdentifier:
    return "empty-build-identifier";
  case SemanticVersionStatus::InvalidBuildCharacter:
    return "invalid-build-character";
  }
  return "invalid";
}

SemanticVersionStatus SemanticVersion::Classify(std::string_view Text) {
  ParsedVersion Parsed;
  return ParseVersionText(Text, Parsed);
}

SemanticVersion SemanticVersion::Parse(std::string_view Text,
                                       SemanticVersionStatus &Status) {
  ParsedVersion Parsed;
  Status = ParseVersionText(Text, Parsed);
  if (Status != SemanticVersionStatus::Valid)
    return SemanticVersion();

  SemanticVersion Version;
  Version.SpecifiedValue = true;
  Version.MajorValue = Parsed.Major;
  Version.MinorValue = Parsed.Minor;
  Version.PatchValue = Parsed.Patch;
  Version.PrereleaseValues = std::move(Parsed.Prerelease);
  Version.BuildValues = std::move(Parsed.Build);
  return Version;
}

std::optional<SemanticVersion>
SemanticVersion::TryParse(std::string_view Text) {
  SemanticVersionStatus Status = SemanticVersionStatus::Valid;
  SemanticVersion Version = SemanticVersion::Parse(Text, Status);
  if (Status != SemanticVersionStatus::Valid)
    return std::nullopt;
  return Version;
}

std::string SemanticVersion::ToString() const {
  if (!SpecifiedValue)
    return std::string();

  std::string Text = std::to_string(MajorValue);
  Text.push_back('.');
  Text.append(std::to_string(MinorValue));
  Text.push_back('.');
  Text.append(std::to_string(PatchValue));
  for (std::size_t Index = 0; Index < PrereleaseValues.size(); ++Index) {
    Text.push_back(Index == 0 ? '-' : '.');
    Text.append(PrereleaseValues[Index]);
  }
  for (std::size_t Index = 0; Index < BuildValues.size(); ++Index) {
    Text.push_back(Index == 0 ? '+' : '.');
    Text.append(BuildValues[Index]);
  }
  return Text;
}

std::size_t SemanticVersion::Hash() const { return HashBytes(ToString()); }

std::strong_ordering
SemanticVersion::ComparePrecedence(const SemanticVersion &Left,
                                   const SemanticVersion &Right) {
  if (Left.SpecifiedValue != Right.SpecifiedValue)
    return Left.SpecifiedValue ? std::strong_ordering::greater
                               : std::strong_ordering::less;
  if (!Left.SpecifiedValue)
    return std::strong_ordering::equal;

  if (const std::strong_ordering Core =
          CompareNumbers(Left.MajorValue, Right.MajorValue);
      Core != std::strong_ordering::equal)
    return Core;
  if (const std::strong_ordering Core =
          CompareNumbers(Left.MinorValue, Right.MinorValue);
      Core != std::strong_ordering::equal)
    return Core;
  if (const std::strong_ordering Core =
          CompareNumbers(Left.PatchValue, Right.PatchValue);
      Core != std::strong_ordering::equal)
    return Core;

  const bool LeftPrerelease = !Left.PrereleaseValues.empty();
  const bool RightPrerelease = !Right.PrereleaseValues.empty();
  if (LeftPrerelease != RightPrerelease)
    return LeftPrerelease ? std::strong_ordering::less
                          : std::strong_ordering::greater;
  if (!LeftPrerelease)
    return std::strong_ordering::equal;

  const std::size_t Shared =
      std::min(Left.PrereleaseValues.size(), Right.PrereleaseValues.size());
  for (std::size_t Index = 0; Index < Shared; ++Index) {
    const std::strong_ordering Identifier = ComparePrereleaseIdentifier(
        Left.PrereleaseValues[Index], Right.PrereleaseValues[Index]);
    if (Identifier != std::strong_ordering::equal)
      return Identifier;
  }
  return CompareNumbers(Left.PrereleaseValues.size(),
                        Right.PrereleaseValues.size());
}

bool SemanticVersion::HasSamePrecedence(const SemanticVersion &Other) const {
  return SemanticVersion::ComparePrecedence(*this, Other) ==
         std::strong_ordering::equal;
}

bool operator==(const SemanticVersion &Left, const SemanticVersion &Right) {
  return Left.SpecifiedValue == Right.SpecifiedValue &&
         Left.MajorValue == Right.MajorValue &&
         Left.MinorValue == Right.MinorValue &&
         Left.PatchValue == Right.PatchValue &&
         Left.PrereleaseValues == Right.PrereleaseValues &&
         Left.BuildValues == Right.BuildValues;
}

std::string_view VersionComparatorText(VersionComparator Comparator) noexcept {
  switch (Comparator) {
  case VersionComparator::Equal:
    return "==";
  case VersionComparator::NotEqual:
    return "!=";
  case VersionComparator::Less:
    return "<";
  case VersionComparator::LessOrEqual:
    return "<=";
  case VersionComparator::Greater:
    return ">";
  case VersionComparator::GreaterOrEqual:
    return ">=";
  }
  return "==";
}

std::string_view
VersionConstraintStatusText(VersionConstraintStatus Status) noexcept {
  switch (Status) {
  case VersionConstraintStatus::Valid:
    return "valid";
  case VersionConstraintStatus::Empty:
    return "empty";
  case VersionConstraintStatus::TooLong:
    return "too-long";
  case VersionConstraintStatus::InvalidComparator:
    return "invalid-comparator";
  case VersionConstraintStatus::InvalidVersion:
    return "invalid-version";
  }
  return "invalid";
}

VersionConstraint VersionConstraint::Create(VersionComparator Comparator,
                                            SemanticVersion Version) {
  VersionConstraint Constraint;
  Constraint.ComparatorValue = Comparator;
  Constraint.VersionValue = std::move(Version);
  return Constraint;
}

VersionConstraintStatus VersionConstraint::Classify(std::string_view Text) {
  if (Text.empty())
    return VersionConstraintStatus::Empty;
  if (Text.size() > VersionConstraint::MaximumLength)
    return VersionConstraintStatus::TooLong;

  std::string_view Remaining = Text;
  if (Remaining.size() >= 2) {
    const std::string_view Pair = Remaining.substr(0, 2);
    if (Pair == "==" || Pair == "!=" || Pair == "<=" || Pair == ">=")
      Remaining = Remaining.substr(2);
    else if (Pair.front() == '<' || Pair.front() == '>' || Pair.front() == '=')
      Remaining = Remaining.substr(1);
    else if (!IsDigit(Pair.front()))
      return VersionConstraintStatus::InvalidComparator;
  } else if (!IsDigit(Remaining.front())) {
    return VersionConstraintStatus::InvalidComparator;
  }

  if (SemanticVersion::Classify(Remaining) != SemanticVersionStatus::Valid)
    return VersionConstraintStatus::InvalidVersion;
  return VersionConstraintStatus::Valid;
}

VersionConstraint VersionConstraint::Parse(std::string_view Text,
                                           VersionConstraintStatus &Status) {
  Status = VersionConstraint::Classify(Text);
  if (Status != VersionConstraintStatus::Valid)
    return VersionConstraint();

  std::string_view Remaining = Text;
  VersionComparator Comparator = VersionComparator::Equal;
  if (Remaining.size() >= 2 && Remaining.substr(0, 2) == "==") {
    Remaining = Remaining.substr(2);
  } else if (Remaining.size() >= 2 && Remaining.substr(0, 2) == "!=") {
    Comparator = VersionComparator::NotEqual;
    Remaining = Remaining.substr(2);
  } else if (Remaining.size() >= 2 && Remaining.substr(0, 2) == "<=") {
    Comparator = VersionComparator::LessOrEqual;
    Remaining = Remaining.substr(2);
  } else if (Remaining.size() >= 2 && Remaining.substr(0, 2) == ">=") {
    Comparator = VersionComparator::GreaterOrEqual;
    Remaining = Remaining.substr(2);
  } else if (Remaining.front() == '<') {
    Comparator = VersionComparator::Less;
    Remaining = Remaining.substr(1);
  } else if (Remaining.front() == '>') {
    Comparator = VersionComparator::Greater;
    Remaining = Remaining.substr(1);
  } else if (Remaining.front() == '=') {
    Remaining = Remaining.substr(1);
  }

  SemanticVersionStatus VersionStatus = SemanticVersionStatus::Valid;
  SemanticVersion Version = SemanticVersion::Parse(Remaining, VersionStatus);
  if (VersionStatus != SemanticVersionStatus::Valid) {
    Status = VersionConstraintStatus::InvalidVersion;
    return VersionConstraint();
  }
  return VersionConstraint::Create(Comparator, std::move(Version));
}

std::optional<VersionConstraint>
VersionConstraint::TryParse(std::string_view Text) {
  VersionConstraintStatus Status = VersionConstraintStatus::Valid;
  VersionConstraint Constraint = VersionConstraint::Parse(Text, Status);
  if (Status != VersionConstraintStatus::Valid)
    return std::nullopt;
  return Constraint;
}

bool VersionConstraint::IsValid() const noexcept {
  return VersionValue.IsValid();
}

bool VersionConstraint::IsSatisfiedBy(const SemanticVersion &Candidate) const {
  if (!VersionValue.IsValid() || !Candidate.IsValid())
    return false;

  const std::strong_ordering Precedence =
      SemanticVersion::ComparePrecedence(Candidate, VersionValue);
  switch (ComparatorValue) {
  case VersionComparator::Equal:
    return Precedence == std::strong_ordering::equal;
  case VersionComparator::NotEqual:
    return Precedence != std::strong_ordering::equal;
  case VersionComparator::Less:
    return Precedence == std::strong_ordering::less;
  case VersionComparator::LessOrEqual:
    return Precedence != std::strong_ordering::greater;
  case VersionComparator::Greater:
    return Precedence == std::strong_ordering::greater;
  case VersionComparator::GreaterOrEqual:
    return Precedence != std::strong_ordering::less;
  }
  return false;
}

std::string VersionConstraint::ToString() const {
  std::string Text(VersionComparatorText(ComparatorValue));
  Text.append(VersionValue.ToString());
  return Text;
}

std::size_t VersionConstraint::Hash() const { return HashBytes(ToString()); }

bool operator==(const VersionConstraint &Left, const VersionConstraint &Right) {
  return Left.ComparatorValue == Right.ComparatorValue &&
         Left.VersionValue == Right.VersionValue;
}

std::strong_ordering CompareConstraint(const VersionConstraint &Left,
                                       const VersionConstraint &Right) {
  if (Left.Comparator() != Right.Comparator())
    return CompareNumbers(static_cast<std::uint64_t>(Left.Comparator()),
                          static_cast<std::uint64_t>(Right.Comparator()));
  if (const std::strong_ordering Precedence =
          SemanticVersion::ComparePrecedence(Left.Version(), Right.Version());
      Precedence != std::strong_ordering::equal)
    return Precedence;
  const std::string LeftText = Left.Version().ToString();
  const std::string RightText = Right.Version().ToString();
  return CompareText(LeftText, RightText);
}

bool operator==(const ModuleDependency &Left, const ModuleDependency &Right) {
  return Left.Identity == Right.Identity &&
         Left.Constraints == Right.Constraints;
}

bool operator==(const ModuleExport &Left, const ModuleExport &Right) {
  return Left.Kind == Right.Kind && Left.Name == Right.Name &&
         Left.Documentation == Right.Documentation;
}

std::strong_ordering CompareExport(const ModuleExport &Left,
                                   const ModuleExport &Right) {
  if (const std::strong_ordering Name = CompareText(Left.Name, Right.Name);
      Name != std::strong_ordering::equal)
    return Name;
  return CompareNumbers(static_cast<std::uint64_t>(Left.Kind),
                        static_cast<std::uint64_t>(Right.Kind));
}

std::string_view
ModuleManifestStatusText(ModuleManifestStatus Status) noexcept {
  switch (Status) {
  case ModuleManifestStatus::Valid:
    return "valid";
  case ModuleManifestStatus::EmptyIdentity:
    return "empty-identity";
  case ModuleManifestStatus::IdentityTooLong:
    return "identity-too-long";
  case ModuleManifestStatus::InvalidIdentity:
    return "invalid-identity";
  case ModuleManifestStatus::InvalidVersion:
    return "invalid-version";
  case ModuleManifestStatus::InvalidDependencyIdentity:
    return "invalid-dependency-identity";
  case ModuleManifestStatus::SelfDependency:
    return "self-dependency";
  case ModuleManifestStatus::MissingDependencyConstraint:
    return "missing-dependency-constraint";
  case ModuleManifestStatus::InvalidDependencyConstraint:
    return "invalid-dependency-constraint";
  case ModuleManifestStatus::InvalidExportName:
    return "invalid-export-name";
  case ModuleManifestStatus::DuplicateExport:
    return "duplicate-export";
  }
  return "invalid";
}

ModuleManifestStatus
ModuleManifest::ClassifyIdentity(std::string_view Text) noexcept {
  return ClassifyIdentityText(Text);
}

ModuleManifest ModuleManifest::Create(
    std::string Identity, SemanticVersion Version,
    std::vector<ModuleDependency> Dependencies, std::string Documentation,
    std::vector<ModuleExport> Exports, ModuleManifestStatus &Status) {
  ModuleManifest Manifest;
  Manifest.IdentityValue = std::move(Identity);
  Manifest.VersionValue = std::move(Version);
  Manifest.DocumentationValue = std::move(Documentation);

  Status = ClassifyIdentityText(Manifest.IdentityValue);
  if (Status == ModuleManifestStatus::Valid && !Manifest.VersionValue.IsValid())
    Status = ModuleManifestStatus::InvalidVersion;

  if (Status == ModuleManifestStatus::Valid) {
    for (const ModuleDependency &Dependency : Dependencies) {
      if (ClassifyIdentityText(Dependency.Identity) !=
          ModuleManifestStatus::Valid) {
        Status = ModuleManifestStatus::InvalidDependencyIdentity;
        break;
      }
      if (Dependency.Identity == Manifest.IdentityValue) {
        Status = ModuleManifestStatus::SelfDependency;
        break;
      }
      if (Dependency.Constraints.empty()) {
        Status = ModuleManifestStatus::MissingDependencyConstraint;
        break;
      }
      bool ConstraintsValid = true;
      for (const VersionConstraint &Constraint : Dependency.Constraints) {
        if (!Constraint.IsValid()) {
          ConstraintsValid = false;
          break;
        }
      }
      if (!ConstraintsValid) {
        Status = ModuleManifestStatus::InvalidDependencyConstraint;
        break;
      }
    }
  }

  if (Status == ModuleManifestStatus::Valid) {
    for (const ModuleExport &Exported : Exports) {
      if (ClassifyIdentityText(Exported.Name) != ModuleManifestStatus::Valid) {
        Status = ModuleManifestStatus::InvalidExportName;
        break;
      }
    }
  }

  if (Status != ModuleManifestStatus::Valid) {
    Manifest.StatusValue = Status;
    Manifest.DependencyValues = std::move(Dependencies);
    Manifest.ExportValues = std::move(Exports);
    return Manifest;
  }

  std::map<std::string, std::vector<VersionConstraint>> MergedDependencies;
  for (ModuleDependency &Dependency : Dependencies) {
    std::vector<VersionConstraint> &Constraints =
        MergedDependencies[Dependency.Identity];
    for (VersionConstraint &Constraint : Dependency.Constraints)
      Constraints.push_back(std::move(Constraint));
  }
  for (auto &[DependencyIdentity, Constraints] : MergedDependencies) {
    std::stable_sort(
        Constraints.begin(), Constraints.end(),
        [](const VersionConstraint &Left, const VersionConstraint &Right) {
          return CompareConstraint(Left, Right) == std::strong_ordering::less;
        });
    Constraints.erase(std::unique(Constraints.begin(), Constraints.end()),
                      Constraints.end());
    ModuleDependency Normalized;
    Normalized.Identity = DependencyIdentity;
    Normalized.Constraints = std::move(Constraints);
    Manifest.DependencyValues.push_back(std::move(Normalized));
  }

  std::stable_sort(Exports.begin(), Exports.end(),
                   [](const ModuleExport &Left, const ModuleExport &Right) {
                     return CompareExport(Left, Right) ==
                            std::strong_ordering::less;
                   });
  for (std::size_t Index = 1; Index < Exports.size(); ++Index) {
    if (CompareExport(Exports[Index - 1], Exports[Index]) ==
        std::strong_ordering::equal) {
      Status = ModuleManifestStatus::DuplicateExport;
      Manifest.StatusValue = Status;
      Manifest.ExportValues = std::move(Exports);
      return Manifest;
    }
  }
  Manifest.ExportValues = std::move(Exports);
  Manifest.StatusValue = ModuleManifestStatus::Valid;
  return Manifest;
}

std::optional<ModuleManifest>
ModuleManifest::TryCreate(std::string Identity, SemanticVersion Version,
                          std::vector<ModuleDependency> Dependencies,
                          std::string Documentation,
                          std::vector<ModuleExport> Exports) {
  ModuleManifestStatus Status = ModuleManifestStatus::Valid;
  ModuleManifest Manifest = ModuleManifest::Create(
      std::move(Identity), std::move(Version), std::move(Dependencies),
      std::move(Documentation), std::move(Exports), Status);
  if (Status != ModuleManifestStatus::Valid)
    return std::nullopt;
  return Manifest;
}

std::string ModuleManifest::Key() const {
  std::string Text = IdentityValue;
  Text.push_back('@');
  Text.append(VersionValue.ToString());
  return Text;
}

const ModuleDependency *
ModuleManifest::FindDependency(std::string_view Identity) const noexcept {
  for (const ModuleDependency &Dependency : DependencyValues) {
    if (Dependency.Identity == Identity)
      return &Dependency;
  }
  return nullptr;
}

std::size_t ModuleManifest::Hash() const {
  std::size_t Accumulator = HashBytes(IdentityValue);
  Accumulator = CombineHash(Accumulator, VersionValue.Hash());
  Accumulator = CombineHash(Accumulator, HashBytes(DocumentationValue));
  for (const ModuleDependency &Dependency : DependencyValues) {
    Accumulator = CombineHash(Accumulator, HashBytes(Dependency.Identity));
    for (const VersionConstraint &Constraint : Dependency.Constraints)
      Accumulator = CombineHash(Accumulator, Constraint.Hash());
  }
  for (const ModuleExport &Exported : ExportValues) {
    Accumulator = CombineHash(Accumulator, HashBytes(Exported.Name));
    Accumulator =
        CombineHash(Accumulator, static_cast<std::size_t>(Exported.Kind));
    Accumulator = CombineHash(Accumulator, HashBytes(Exported.Documentation));
  }
  return Accumulator;
}

bool operator==(const ModuleManifest &Left, const ModuleManifest &Right) {
  return Left.StatusValue == Right.StatusValue &&
         Left.IdentityValue == Right.IdentityValue &&
         Left.VersionValue == Right.VersionValue &&
         Left.DocumentationValue == Right.DocumentationValue &&
         Left.DependencyValues == Right.DependencyValues &&
         Left.ExportValues == Right.ExportValues;
}

std::strong_ordering CompareManifest(const ModuleManifest &Left,
                                     const ModuleManifest &Right) {
  if (const std::strong_ordering Identity =
          CompareText(Left.Identity(), Right.Identity());
      Identity != std::strong_ordering::equal)
    return Identity;
  if (const std::strong_ordering Precedence =
          SemanticVersion::ComparePrecedence(Left.Version(), Right.Version());
      Precedence != std::strong_ordering::equal)
    return Precedence;
  const std::string LeftText = Left.Version().ToString();
  const std::string RightText = Right.Version().ToString();
  return CompareText(LeftText, RightText);
}

} // namespace Luna
