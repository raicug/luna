// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>

#include "state/testing/test_hooks.hpp"

#include <rapidcheck.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

class ByteCursor final {
public:
  explicit ByteCursor(std::span<const std::uint8_t> Bytes) noexcept
      : BytesValue(Bytes) {}

  [[nodiscard]] std::uint8_t Next() noexcept {
    const std::size_t Index = IndexValue++;
    if (BytesValue.empty())
      return static_cast<std::uint8_t>(Index * 37U + 13U);
    return BytesValue[Index % BytesValue.size()];
  }

  [[nodiscard]] std::size_t Pick(std::size_t Count) noexcept {
    return Count == 0 ? 0 : static_cast<std::size_t>(Next()) % Count;
  }

private:
  std::span<const std::uint8_t> BytesValue;
  std::size_t IndexValue = 0;
};

constexpr std::size_t IdentityCount = 4;

constexpr std::array<std::string_view, IdentityCount> IdentityTexts{
    "luna.alpha", "luna.bravo", "luna.charlie", "luna.delta"};

constexpr std::array<std::string_view, IdentityCount> NamespaceNames{
    "Alpha", "Bravo", "Charlie", "Delta"};

struct VersionPoolEntry final {
  std::string_view Text;
  std::uint64_t Major = 0;
  std::uint64_t Minor = 0;
  std::uint64_t Patch = 0;
  std::string_view Prerelease;
};

constexpr std::array<VersionPoolEntry, 4> VersionPool{
    {{"1.0.0", 1, 0, 0, ""},
     {"1.1.0-rc.1", 1, 1, 0, "rc.1"},
     {"1.1.0", 1, 1, 0, ""},
     {"2.0.0", 2, 0, 0, ""}}};

enum class ModelComparator {
  Equal,
  NotEqual,
  Less,
  LessOrEqual,
  Greater,
  GreaterOrEqual
};

struct ConstraintPoolEntry final {
  std::string_view Text;
  ModelComparator Comparator = ModelComparator::Equal;
  std::size_t Version = 0;
};

constexpr std::array<ConstraintPoolEntry, 8> ConstraintPool{
    {{"==1.0.0", ModelComparator::Equal, 0},
     {"!=1.1.0", ModelComparator::NotEqual, 2},
     {"<2.0.0", ModelComparator::Less, 3},
     {"<=1.1.0-rc.1", ModelComparator::LessOrEqual, 1},
     {">1.0.0", ModelComparator::Greater, 0},
     {">=1.0.0", ModelComparator::GreaterOrEqual, 0},
     {">=1.1.0", ModelComparator::GreaterOrEqual, 2},
     {">=2.0.0", ModelComparator::GreaterOrEqual, 3}}};

constexpr std::array<std::string_view, 3> DocumentationPool{
    {"", "Documents one module.", "Documents another module."}};

constexpr std::size_t ExportPoolSize = 2;

[[nodiscard]] std::string ExportName(std::size_t Identity, std::size_t Slot) {
  std::string Name(NamespaceNames[Identity]);
  if (Slot == 1)
    Name.append(".Tag");
  return Name;
}

[[nodiscard]] Luna::SymbolKind ExportKind(std::size_t Slot) noexcept {
  return Slot == 1 ? Luna::SymbolKind::Constant : Luna::SymbolKind::Namespace;
}

struct ModelVersion final {
  std::uint64_t Major = 0;
  std::uint64_t Minor = 0;
  std::uint64_t Patch = 0;
  std::vector<std::string> Prerelease;
};

[[nodiscard]] std::vector<std::string> SplitIdentifiers(std::string_view Text) {
  std::vector<std::string> Identifiers;
  if (Text.empty())
    return Identifiers;
  std::size_t Start = 0;
  while (true) {
    const std::size_t Separator = Text.find('.', Start);
    if (Separator == std::string_view::npos) {
      Identifiers.emplace_back(Text.substr(Start));
      return Identifiers;
    }
    Identifiers.emplace_back(Text.substr(Start, Separator - Start));
    Start = Separator + 1;
  }
}

[[nodiscard]] ModelVersion ModelVersionOf(std::size_t Index) {
  const VersionPoolEntry &Entry = VersionPool[Index];
  ModelVersion Version;
  Version.Major = Entry.Major;
  Version.Minor = Entry.Minor;
  Version.Patch = Entry.Patch;
  Version.Prerelease = SplitIdentifiers(Entry.Prerelease);
  return Version;
}

[[nodiscard]] bool IsNumericIdentifier(const std::string &Text) {
  if (Text.empty())
    return false;
  return std::all_of(Text.begin(), Text.end(), [](char Character) {
    return Character >= '0' && Character <= '9';
  });
}

[[nodiscard]] int ComparePrecedence(const ModelVersion &Left,
                                    const ModelVersion &Right) {
  if (Left.Major != Right.Major)
    return Left.Major < Right.Major ? -1 : 1;
  if (Left.Minor != Right.Minor)
    return Left.Minor < Right.Minor ? -1 : 1;
  if (Left.Patch != Right.Patch)
    return Left.Patch < Right.Patch ? -1 : 1;
  if (Left.Prerelease.empty() && Right.Prerelease.empty())
    return 0;
  if (Left.Prerelease.empty())
    return 1;
  if (Right.Prerelease.empty())
    return -1;

  const std::size_t Shared =
      std::min(Left.Prerelease.size(), Right.Prerelease.size());
  for (std::size_t Index = 0; Index < Shared; ++Index) {
    const std::string &One = Left.Prerelease[Index];
    const std::string &Other = Right.Prerelease[Index];
    const bool OneIsNumeric = IsNumericIdentifier(One);
    const bool OtherIsNumeric = IsNumericIdentifier(Other);
    if (OneIsNumeric && OtherIsNumeric) {
      const std::uint64_t First = std::stoull(One);
      const std::uint64_t Second = std::stoull(Other);
      if (First != Second)
        return First < Second ? -1 : 1;
      continue;
    }
    if (OneIsNumeric != OtherIsNumeric)
      return OneIsNumeric ? -1 : 1;
    if (One != Other)
      return One < Other ? -1 : 1;
  }
  if (Left.Prerelease.size() == Right.Prerelease.size())
    return 0;
  return Left.Prerelease.size() < Right.Prerelease.size() ? -1 : 1;
}

[[nodiscard]] int ComparePool(std::size_t Left, std::size_t Right) {
  return ComparePrecedence(ModelVersionOf(Left), ModelVersionOf(Right));
}

[[nodiscard]] bool Satisfies(std::size_t Candidate,
                             const ConstraintPoolEntry &Constraint) {
  const int Order = ComparePool(Candidate, Constraint.Version);
  switch (Constraint.Comparator) {
  case ModelComparator::Equal:
    return Order == 0;
  case ModelComparator::NotEqual:
    return Order != 0;
  case ModelComparator::Less:
    return Order < 0;
  case ModelComparator::LessOrEqual:
    return Order <= 0;
  case ModelComparator::Greater:
    return Order > 0;
  case ModelComparator::GreaterOrEqual:
    return Order >= 0;
  }
  return false;
}

struct ModelConstraint final {
  ModelComparator Comparator = ModelComparator::Equal;
  std::size_t Version = 0;
  std::string Text;
};

[[nodiscard]] ModelConstraint PoolConstraint(std::size_t Index) {
  ModelConstraint Constraint;
  Constraint.Comparator = ConstraintPool[Index].Comparator;
  Constraint.Version = ConstraintPool[Index].Version;
  Constraint.Text = std::string(ConstraintPool[Index].Text);
  return Constraint;
}

[[nodiscard]] bool Satisfies(std::size_t Candidate,
                             const ModelConstraint &Constraint) {
  ConstraintPoolEntry Entry;
  Entry.Comparator = Constraint.Comparator;
  Entry.Version = Constraint.Version;
  return Satisfies(Candidate, Entry);
}

[[nodiscard]] bool SatisfiesEvery(std::size_t Candidate,
                                  const std::vector<ModelConstraint> &All) {
  return std::all_of(All.begin(), All.end(),
                     [Candidate](const ModelConstraint &Constraint) {
                       return Satisfies(Candidate, Constraint);
                     });
}

} // namespace

namespace {

struct ModelDependency final {
  std::size_t Identity = 0;
  std::vector<std::size_t> Constraints;

  [[nodiscard]] friend bool operator==(const ModelDependency &,
                                       const ModelDependency &) = default;
};

struct ModelManifest final {
  std::size_t Identity = 0;
  std::size_t Version = 0;
  std::vector<ModelDependency> Dependencies;
  std::vector<std::size_t> Exports;
  std::size_t Documentation = 0;

  [[nodiscard]] friend bool operator==(const ModelManifest &,
                                       const ModelManifest &) = default;
};

using ModelCatalog = std::array<std::vector<ModelManifest>, IdentityCount>;

[[nodiscard]] std::string ModelKey(std::size_t Identity, std::size_t Version) {
  std::string Key(IdentityTexts[Identity]);
  Key.push_back('@');
  Key.append(VersionPool[Version].Text);
  return Key;
}

[[nodiscard]] const ModelManifest *FindAvailable(const ModelCatalog &Catalog,
                                                 std::size_t Identity,
                                                 std::size_t Version) {
  for (const ModelManifest &Available : Catalog[Identity]) {
    if (ComparePool(Available.Version, Version) == 0)
      return &Available;
  }
  return nullptr;
}

enum class ModelAddStatus { Added, Duplicate, Conflicting };

[[nodiscard]] ModelAddStatus AddAvailable(ModelCatalog &Catalog,
                                          const ModelManifest &Definition) {
  if (const ModelManifest *Existing =
          FindAvailable(Catalog, Definition.Identity, Definition.Version))
    return *Existing == Definition ? ModelAddStatus::Duplicate
                                   : ModelAddStatus::Conflicting;
  Catalog[Definition.Identity].push_back(Definition);
  std::vector<ModelManifest> &Versions = Catalog[Definition.Identity];
  std::stable_sort(Versions.begin(), Versions.end(),
                   [](const ModelManifest &Left, const ModelManifest &Right) {
                     return ComparePool(Left.Version, Right.Version) < 0;
                   });
  return ModelAddStatus::Added;
}

[[nodiscard]] std::size_t AvailableCount(const ModelCatalog &Catalog) {
  std::size_t Total = 0;
  for (const std::vector<ModelManifest> &Versions : Catalog)
    Total += Versions.size();
  return Total;
}

enum class ModelResolutionStatus {
  Resolved,
  MissingDependency,
  UnsatisfiedConstraint,
  ConflictingSelection,
  DependencyCycle
};

[[nodiscard]] std::string_view
ModelResolutionStatusText(ModelResolutionStatus Status) noexcept {
  switch (Status) {
  case ModelResolutionStatus::Resolved:
    return "resolved";
  case ModelResolutionStatus::MissingDependency:
    return "missing-dependency";
  case ModelResolutionStatus::UnsatisfiedConstraint:
    return "unsatisfied-constraint";
  case ModelResolutionStatus::ConflictingSelection:
    return "conflicting-selection";
  case ModelResolutionStatus::DependencyCycle:
    return "dependency-cycle";
  }
  return "invalid";
}

struct ModelResolution final {
  ModelResolutionStatus Status = ModelResolutionStatus::Resolved;
  std::map<std::size_t, std::size_t> Selected;
  std::vector<std::size_t> LoadOrder;
  std::vector<std::string> Path;

  [[nodiscard]] bool IsResolved() const noexcept {
    return Status == ModelResolutionStatus::Resolved;
  }

  [[nodiscard]] std::string PathText() const {
    std::string Text;
    for (std::size_t Index = 0; Index < Path.size(); ++Index) {
      if (Index != 0)
        Text.append(" -> ");
      Text.append(Path[Index]);
    }
    return Text;
  }
};

struct ModelRecord final {
  std::vector<ModelConstraint> Constraints;
  std::vector<std::string> Origin;
};

[[nodiscard]] std::vector<std::string> Extend(std::vector<std::string> Origin,
                                              std::string Tail) {
  Origin.push_back(std::move(Tail));
  return Origin;
}

[[nodiscard]] bool FindCycle(const ModelCatalog &Catalog,
                             const std::map<std::size_t, std::size_t> &Selected,
                             std::size_t Identity,
                             std::vector<std::string> &Stack,
                             std::set<std::size_t> &Active,
                             std::set<std::size_t> &Finished,
                             std::vector<std::string> &Cycle) {
  const auto Selection = Selected.find(Identity);
  if (Selection == Selected.end())
    return false;

  Stack.push_back(ModelKey(Identity, Selection->second));
  Active.insert(Identity);

  if (const ModelManifest *Manifest =
          FindAvailable(Catalog, Identity, Selection->second)) {
    for (const ModelDependency &Dependency : Manifest->Dependencies) {
      if (Active.count(Dependency.Identity) != 0) {
        Cycle = Stack;
        const auto Repeated = Selected.find(Dependency.Identity);
        Cycle.push_back(Repeated == Selected.end()
                            ? std::string(IdentityTexts[Dependency.Identity])
                            : ModelKey(Dependency.Identity, Repeated->second));
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

void AppendLoadOrder(const ModelCatalog &Catalog,
                     const std::map<std::size_t, std::size_t> &Selected,
                     std::size_t Identity, std::set<std::size_t> &Visited,
                     std::vector<std::size_t> &Order) {
  if (Visited.count(Identity) != 0)
    return;
  Visited.insert(Identity);

  const auto Selection = Selected.find(Identity);
  if (Selection == Selected.end())
    return;

  if (const ModelManifest *Manifest =
          FindAvailable(Catalog, Identity, Selection->second)) {
    for (const ModelDependency &Dependency : Manifest->Dependencies)
      AppendLoadOrder(Catalog, Selected, Dependency.Identity, Visited, Order);
  }
  Order.push_back(Identity);
}

[[nodiscard]] ModelResolution
Resolve(const ModelCatalog &Catalog, const ModelManifest &Request,
        const std::array<std::optional<ModelManifest>, IdentityCount> &Loaded) {
  ModelResolution Resolution;

  std::map<std::size_t, ModelRecord> Accumulated;
  ModelConstraint Root;
  Root.Comparator = ModelComparator::Equal;
  Root.Version = Request.Version;
  Root.Text = "==" + std::string(VersionPool[Request.Version].Text);
  Accumulated[Request.Identity].Constraints.push_back(std::move(Root));

  while (true) {
    for (const auto &[Identity, Version] : Resolution.Selected) {
      const ModelRecord &Record = Accumulated[Identity];
      const bool Violated = !SatisfiesEvery(Version, Record.Constraints);
      if (!Violated)
        continue;
      Resolution.Status = ModelResolutionStatus::ConflictingSelection;
      Resolution.Path = Extend(Record.Origin, ModelKey(Identity, Version));
      return Resolution;
    }

    std::optional<std::size_t> Next;
    for (const auto &[Identity, Record] : Accumulated) {
      if (Resolution.Selected.count(Identity) == 0) {
        Next = Identity;
        break;
      }
    }
    if (!Next)
      break;

    const std::size_t Identity = *Next;
    const ModelRecord &Record = Accumulated[Identity];
    const std::vector<ModelManifest> &Candidates = Catalog[Identity];
    if (Candidates.empty()) {
      Resolution.Status = ModelResolutionStatus::MissingDependency;
      Resolution.Path =
          Extend(Record.Origin, std::string(IdentityTexts[Identity]));
      return Resolution;
    }

    const ModelManifest *Chosen = nullptr;
    for (std::size_t Index = Candidates.size(); Index > 0; --Index) {
      const ModelManifest &Candidate = Candidates[Index - 1];
      if (SatisfiesEvery(Candidate.Version, Record.Constraints)) {
        Chosen = &Candidate;
        break;
      }
    }
    if (!Chosen) {
      Resolution.Status = ModelResolutionStatus::UnsatisfiedConstraint;
      Resolution.Path =
          Extend(Record.Origin, std::string(IdentityTexts[Identity]));
      return Resolution;
    }

    if (Loaded[Identity]) {
      const std::size_t Pinned = Loaded[Identity]->Version;
      if (!SatisfiesEvery(Pinned, Record.Constraints)) {
        Resolution.Status = ModelResolutionStatus::ConflictingSelection;
        Resolution.Path = Extend(Record.Origin, ModelKey(Identity, Pinned));
        return Resolution;
      }
      const ModelManifest *Available = FindAvailable(Catalog, Identity, Pinned);
      if (!Available) {
        Resolution.Status = ModelResolutionStatus::ConflictingSelection;
        Resolution.Path = Extend(Record.Origin, ModelKey(Identity, Pinned));
        return Resolution;
      }
      Chosen = Available;
    }

    Resolution.Selected.insert_or_assign(Identity, Chosen->Version);
    const std::vector<std::string> ChildOrigin =
        Extend(Record.Origin, ModelKey(Identity, Chosen->Version));
    for (const ModelDependency &Dependency : Chosen->Dependencies) {
      const bool IsNew = Accumulated.count(Dependency.Identity) == 0;
      ModelRecord &Child = Accumulated[Dependency.Identity];
      if (IsNew)
        Child.Origin = ChildOrigin;
      for (const std::size_t Constraint : Dependency.Constraints)
        Child.Constraints.push_back(PoolConstraint(Constraint));
    }
  }

  std::vector<std::string> Cycle;
  std::vector<std::string> Stack;
  std::set<std::size_t> Active;
  std::set<std::size_t> Finished;
  for (const auto &[Identity, Version] : Resolution.Selected) {
    if (Finished.count(Identity) != 0)
      continue;
    if (FindCycle(Catalog, Resolution.Selected, Identity, Stack, Active,
                  Finished, Cycle)) {
      Resolution.Status = ModelResolutionStatus::DependencyCycle;
      Resolution.Path = Cycle;
      return Resolution;
    }
    Stack.clear();
    Active.clear();
  }

  std::set<std::size_t> Visited;
  AppendLoadOrder(Catalog, Resolution.Selected, Request.Identity, Visited,
                  Resolution.LoadOrder);
  for (const auto &[Identity, Version] : Resolution.Selected)
    AppendLoadOrder(Catalog, Resolution.Selected, Identity, Visited,
                    Resolution.LoadOrder);
  return Resolution;
}

} // namespace

namespace {

struct ModelState final {
  ModelCatalog Available;
  std::array<std::optional<ModelManifest>, IdentityCount> Loaded;
  std::array<std::size_t, IdentityCount> Callbacks{};
  std::uint64_t Generation = 0;
};

[[nodiscard]] std::size_t LoadedCount(const ModelState &State) {
  std::size_t Total = 0;
  for (const auto &Loaded : State.Loaded) {
    if (Loaded)
      ++Total;
  }
  return Total;
}

enum class ExpectedOutcome {
  Published,
  Idempotent,
  Conflict,
  Unresolvable,
  CallbackFailure
};

struct ExpectedLoad final {
  ExpectedOutcome Outcome = ExpectedOutcome::Published;
  bool Succeeds = false;
  std::vector<std::size_t> Executed;
  ModelResolution Resolution;
  std::vector<std::pair<std::size_t, ModelManifest>> Selections;
};

[[nodiscard]] ExpectedLoad PlanLoad(const ModelState &State,
                                    const ModelManifest &Request,
                                    std::size_t ThrowKind,
                                    std::size_t PoisonIdentity) {
  ExpectedLoad Expected;

  if (State.Loaded[Request.Identity]) {
    const ModelManifest &Loaded = *State.Loaded[Request.Identity];
    const bool SameVersion = ComparePool(Loaded.Version, Request.Version) == 0;
    if (SameVersion && Loaded == Request) {
      Expected.Outcome = ExpectedOutcome::Idempotent;
      Expected.Succeeds = true;
      return Expected;
    }
    Expected.Outcome = ExpectedOutcome::Conflict;
    return Expected;
  }

  if (const ModelManifest *Available =
          FindAvailable(State.Available, Request.Identity, Request.Version)) {
    if (!(*Available == Request)) {
      Expected.Outcome = ExpectedOutcome::Conflict;
      return Expected;
    }
  }

  ModelCatalog Candidate = State.Available;
  static_cast<void>(AddAvailable(Candidate, Request));
  Expected.Resolution = Resolve(Candidate, Request, State.Loaded);
  if (!Expected.Resolution.IsResolved()) {
    Expected.Outcome = ExpectedOutcome::Unresolvable;
    return Expected;
  }

  for (const auto &[Identity, Version] : Expected.Resolution.Selected) {
    const ModelManifest *Selected = FindAvailable(Candidate, Identity, Version);
    if (Selected)
      Expected.Selections.emplace_back(Identity, *Selected);
  }

  std::vector<std::size_t> Order;
  for (const std::size_t Identity : Expected.Resolution.LoadOrder) {
    if (Identity != Request.Identity && State.Loaded[Identity])
      continue;
    Order.push_back(Identity);
  }

  const auto Poisoned = std::find(Order.begin(), Order.end(), PoisonIdentity);
  if (Poisoned != Order.end()) {
    Expected.Outcome = ExpectedOutcome::CallbackFailure;
    Expected.Executed.assign(Order.begin(), Poisoned + 1);
    return Expected;
  }
  Expected.Executed = Order;
  if (ThrowKind != 0) {
    Expected.Outcome = ExpectedOutcome::CallbackFailure;
    return Expected;
  }

  Expected.Outcome = ExpectedOutcome::Published;
  Expected.Succeeds = true;
  return Expected;
}

void ApplyLoad(ModelState &State, const ModelManifest &Request,
               const ExpectedLoad &Expected) {
  for (const std::size_t Identity : Expected.Executed)
    ++State.Callbacks[Identity];
  if (Expected.Outcome != ExpectedOutcome::Published)
    return;
  for (const auto &[Identity, Selected] : Expected.Selections)
    State.Loaded[Identity] = Selected;
  static_cast<void>(AddAvailable(State.Available, Request));
  ++State.Generation;
}

[[nodiscard]] std::string
ObservedEnumeration(const Luna::ReflectionSnapshot &Snapshot) {
  std::string Text;
  const Luna::ModuleRecordRange Modules = Snapshot.Modules();
  for (std::size_t Index = 0; Index < Modules.Size(); ++Index) {
    const Luna::ModuleRecord Module = Modules.At(Index);
    Text.append(Module.Identity()).append("@").append(Module.Version());
    for (std::size_t Position = 0; Position < Module.DependencyCount();
         ++Position) {
      const Luna::ModuleDependencyRecord Dependency =
          Module.Dependency(Position);
      Text.append("|dep:").append(Dependency.Identity()).append("=");
      Text.append(Dependency.Version()).append("(");
      Text.append(Dependency.Constraints()).append(")");
    }
    for (std::size_t Position = 0; Position < Module.ExportCount(); ++Position)
      Text.append("|exp:").append(Module.Export(Position).Name());
    for (std::size_t Position = 0; Position < Module.NamespaceCount();
         ++Position)
      Text.append("|ns:").append(Module.Namespace(Position));
    for (std::size_t Position = 0; Position < Module.TypeCount(); ++Position)
      Text.append("|ty:").append(Module.TypeName(Position));
    Text.append(";");
  }
  return Text;
}

[[nodiscard]] std::string
ConstraintText(const std::vector<std::size_t> &Constraints) {
  std::string Text;
  for (const std::size_t Index : Constraints) {
    if (!Text.empty())
      Text.append(", ");
    Text.append(ConstraintPool[Index].Text);
  }
  if (Text.empty())
    Text.append("none");
  return Text;
}

[[nodiscard]] std::string ExpectedEnumeration(const ModelState &State) {
  std::string Text;
  for (std::size_t Identity = 0; Identity < IdentityCount; ++Identity) {
    if (!State.Loaded[Identity])
      continue;
    const ModelManifest &Loaded = *State.Loaded[Identity];
    Text.append(IdentityTexts[Identity]).append("@");
    Text.append(VersionPool[Loaded.Version].Text);
    for (const ModelDependency &Dependency : Loaded.Dependencies) {
      Text.append("|dep:").append(IdentityTexts[Dependency.Identity]);
      Text.append("=");
      if (State.Loaded[Dependency.Identity])
        Text.append(
            VersionPool[State.Loaded[Dependency.Identity]->Version].Text);
      Text.append("(").append(ConstraintText(Dependency.Constraints));
      Text.append(")");
    }
    for (const std::size_t Slot : Loaded.Exports)
      Text.append("|exp:").append(ExportName(Identity, Slot));
    Text.append("|ns:").append(NamespaceNames[Identity]);
    Text.append(";");
  }
  return Text;
}

[[nodiscard]] int AddIntegers(int Left, int Right) { return Left + Right; }

[[nodiscard]] Luna::ModuleManifest BuildManifest(const ModelManifest &Model) {
  std::vector<Luna::ModuleDependency> Dependencies;
  for (const ModelDependency &Declared : Model.Dependencies) {
    Luna::ModuleDependency Dependency;
    Dependency.Identity = std::string(IdentityTexts[Declared.Identity]);
    for (const std::size_t Index : Declared.Constraints) {
      const auto Parsed =
          Luna::VersionConstraint::TryParse(ConstraintPool[Index].Text);
      if (Parsed)
        Dependency.Constraints.push_back(*Parsed);
    }
    Dependencies.push_back(std::move(Dependency));
  }

  std::vector<Luna::ModuleExport> Exports;
  for (const std::size_t Slot : Model.Exports) {
    Luna::ModuleExport Exported;
    Exported.Kind = ExportKind(Slot);
    Exported.Name = ExportName(Model.Identity, Slot);
    Exports.push_back(std::move(Exported));
  }

  const auto Version =
      Luna::SemanticVersion::TryParse(VersionPool[Model.Version].Text);
  if (!Version)
    return Luna::ModuleManifest();
  auto Created = Luna::ModuleManifest::TryCreate(
      std::string(IdentityTexts[Model.Identity]), *Version,
      std::move(Dependencies),
      std::string(DocumentationPool[Model.Documentation]), std::move(Exports));
  return Created ? std::move(*Created) : Luna::ModuleManifest();
}

struct ModuleCallback final {
  std::size_t Identity = 0;
  bool Poison = false;
  std::size_t ThrowKind = 0;
  std::vector<std::size_t> *Log = nullptr;

  void operator()(Luna::NamespaceBuilder &Builder) const {
    if (Log)
      Log->push_back(Identity);
    Luna::NamespaceBuilder Scope =
        Builder.RegisterNamespace(NamespaceNames[Identity]);
    static_cast<void>(
        Scope.RegisterConstant("Tag", static_cast<int>(Identity) + 1));
    if (Poison)
      static_cast<void>(Builder.RegisterNamespace("not a name"));
    if (ThrowKind == 1)
      throw std::runtime_error("module callback failed");
    if (ThrowKind == 2)
      throw 19;
  }
};

} // namespace

namespace {

struct ScenarioRequest final {
  ModelManifest Manifest;
  std::size_t ThrowKind = 0;
};

struct Scenario final {
  std::vector<ModelManifest> Definitions;
  std::vector<ScenarioRequest> Requests;
  std::size_t PoisonIdentity = IdentityCount;
};

[[nodiscard]] ModelManifest MakeModelManifest(ByteCursor &Cursor,
                                              std::size_t Identity,
                                              std::size_t Version) {
  ModelManifest Model;
  Model.Identity = Identity;
  Model.Version = Version;

  for (std::size_t Other = 0; Other < IdentityCount; ++Other) {
    if (Other == Identity)
      continue;
    if (Cursor.Pick(3) != 0)
      continue;
    ModelDependency Dependency;
    Dependency.Identity = Other;
    const std::size_t Count = 1U + Cursor.Pick(2);
    for (std::size_t Index = 0; Index < Count; ++Index) {
      const std::size_t Choice = Cursor.Pick(ConstraintPool.size());
      if (std::find(Dependency.Constraints.begin(),
                    Dependency.Constraints.end(),
                    Choice) == Dependency.Constraints.end())
        Dependency.Constraints.push_back(Choice);
    }
    std::sort(Dependency.Constraints.begin(), Dependency.Constraints.end());
    Model.Dependencies.push_back(std::move(Dependency));
  }

  for (std::size_t Slot = 0; Slot < ExportPoolSize; ++Slot) {
    if (Cursor.Pick(2) == 0)
      Model.Exports.push_back(Slot);
  }
  Model.Documentation = Cursor.Pick(DocumentationPool.size());
  return Model;
}

[[nodiscard]] Scenario MakeScenario(ByteCursor &Cursor) {
  Scenario Case;

  for (std::size_t Identity = 0; Identity < IdentityCount; ++Identity) {
    std::size_t Count = Cursor.Pick(3);
    if (Count == 0 && Cursor.Pick(3) != 0)
      Count = 1;
    std::vector<std::size_t> Versions;
    for (std::size_t Index = 0; Index < Count; ++Index) {
      const std::size_t Version = Cursor.Pick(VersionPool.size());
      if (std::find(Versions.begin(), Versions.end(), Version) !=
          Versions.end())
        continue;
      Versions.push_back(Version);
      Case.Definitions.push_back(MakeModelManifest(Cursor, Identity, Version));
    }
  }

  const std::size_t RequestCount = 1U + Cursor.Pick(3);
  for (std::size_t Index = 0; Index < RequestCount; ++Index) {
    const std::size_t Identity = Cursor.Pick(IdentityCount);
    const std::size_t Version = Cursor.Pick(VersionPool.size());
    const auto Provided =
        std::find_if(Case.Definitions.begin(), Case.Definitions.end(),
                     [Identity, Version](const ModelManifest &Existing) {
                       return Existing.Identity == Identity &&
                              ComparePool(Existing.Version, Version) == 0;
                     });

    ScenarioRequest Request;
    if (Provided != Case.Definitions.end() && Cursor.Pick(3) != 0)
      Request.Manifest = *Provided;
    else
      Request.Manifest = MakeModelManifest(Cursor, Identity, Version);

    const std::size_t Throw = Cursor.Pick(6);
    Request.ThrowKind = Throw < 2 ? Throw + 1 : 0;
    Case.Requests.push_back(std::move(Request));
  }

  const std::size_t Poison = Cursor.Pick(2U * IdentityCount);
  Case.PoisonIdentity = Poison < IdentityCount ? Poison : IdentityCount;
  return Case;
}

void VerifyObservations(
    Luna::State &Owner, Luna::BindingRegistry &Registry,
    const ModelState &Model, int EntryDepth,
    const std::array<std::size_t, IdentityCount> &Observed) {
  RC_ASSERT(Hooks::LoadedModuleCount(Owner) == LoadedCount(Model));
  RC_ASSERT(Hooks::AvailableModuleCount(Owner) ==
            AvailableCount(Model.Available));

  for (std::size_t Identity = 0; Identity < IdentityCount; ++Identity) {
    const bool IsLoaded = Model.Loaded[Identity].has_value();
    RC_ASSERT(Hooks::ModuleIsLoaded(Owner, IdentityTexts[Identity]) ==
              IsLoaded);
    const auto Version =
        Hooks::LoadedModuleVersion(Owner, IdentityTexts[Identity]);
    RC_ASSERT(Version.has_value() == IsLoaded);
    if (IsLoaded)
      RC_ASSERT(*Version == VersionPool[Model.Loaded[Identity]->Version].Text);

    const auto Kind = Hooks::ObserveVmPathValueKind(
        Owner, std::string(NamespaceNames[Identity]));
    RC_ASSERT(Kind.has_value());
    RC_ASSERT(*Kind == std::string(IsLoaded ? "table" : "absent"));
  }

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  RC_ASSERT(Snapshot.Generation() == Model.Generation);
  RC_ASSERT(Snapshot.Modules().Size() == LoadedCount(Model));
  RC_ASSERT(ObservedEnumeration(Snapshot) == ExpectedEnumeration(Model));

  RC_ASSERT(Observed == Model.Callbacks);
  RC_ASSERT(Hooks::BindingIsCommitted(Owner, "Seeded"));

  const auto Depth = Hooks::ObserveRootStackDepth(Owner);
  RC_ASSERT(Depth.has_value());
  RC_ASSERT(*Depth == EntryDepth);
}

void VerifyProvision(Luna::State &Owner, Luna::BindingRegistry &Registry,
                     ModelState &Model, const ModelManifest &Definition,
                     std::size_t PoisonIdentity, int EntryDepth,
                     std::vector<std::size_t> &Log,
                     std::array<std::size_t, IdentityCount> &Observed) {
  const ModelManifest *Available =
      FindAvailable(Model.Available, Definition.Identity, Definition.Version);
  const bool Succeeds = !Available || *Available == Definition;

  const auto Generations = Hooks::GenerationsOf(Owner);
  Log.clear();
  const auto Result = Registry.ProvideModule(
      BuildManifest(Definition),
      ModuleCallback{Definition.Identity, PoisonIdentity == Definition.Identity,
                     0, &Log});

  RC_ASSERT(Result.IsSuccess() == Succeeds);
  RC_ASSERT(Log.empty());
  RC_ASSERT(Hooks::GenerationsOf(Owner) == Generations);
  if (Succeeds)
    static_cast<void>(AddAvailable(Model.Available, Definition));
  VerifyObservations(Owner, Registry, Model, EntryDepth, Observed);
}

void VerifyLoad(Luna::State &Owner, Luna::BindingRegistry &Registry,
                ModelState &Model, const ModelManifest &Request,
                std::size_t ThrowKind, std::size_t PoisonIdentity,
                int EntryDepth, std::vector<std::size_t> &Log,
                std::array<std::size_t, IdentityCount> &Observed) {
  const ExpectedLoad Expected =
      PlanLoad(Model, Request, ThrowKind, PoisonIdentity);
  const auto Generations = Hooks::GenerationsOf(Owner);

  Log.clear();
  const auto Result = Registry.RegisterModule(
      BuildManifest(Request),
      ModuleCallback{Request.Identity, PoisonIdentity == Request.Identity,
                     ThrowKind, &Log});

  RC_ASSERT(Result.IsSuccess() == Expected.Succeeds);
  RC_ASSERT(Log == Expected.Executed);
  for (const std::size_t Identity : Log)
    ++Observed[Identity];

  if (!Expected.Succeeds) {
    RC_ASSERT(Result.Diagnostic());
    RC_ASSERT(!Result.Diagnostic()->Message().empty());
    if (Expected.Outcome == ExpectedOutcome::Conflict)
      RC_ASSERT(Result.Diagnostic()->Category() ==
                Luna::ErrorCategory::DuplicateGlobalName);
    if (Expected.Outcome == ExpectedOutcome::Unresolvable) {
      const std::string Message = Result.Diagnostic()->Message();
      std::string Status("module resolution ");
      Status.append(ModelResolutionStatusText(Expected.Resolution.Status));
      RC_ASSERT(Message.find(Status) != std::string::npos);
      const std::string Path = Expected.Resolution.PathText();
      RC_ASSERT(!Path.empty());
      RC_ASSERT(Message.find("(dependency path: " + Path + ")") !=
                std::string::npos);
    }
  }

  if (Expected.Outcome != ExpectedOutcome::Published)
    RC_ASSERT(Hooks::GenerationsOf(Owner) == Generations);

  ApplyLoad(Model, Request, Expected);
  VerifyObservations(Owner, Registry, Model, EntryDepth, Observed);
}

[[nodiscard]] std::string RunScenario(const Scenario &Case,
                                      bool ReversedProvision) {
  Luna::State Owner;
  RC_ASSERT(Owner.IsReady());
  Luna::BindingRegistry Registry = Owner.Bindings();

  const auto Depth = Hooks::ObserveRootStackDepth(Owner);
  RC_ASSERT(Depth.has_value());
  const int EntryDepth = *Depth;

  RC_ASSERT(Registry.Register("Seeded", &AddIntegers).IsSuccess());

  ModelState Model;
  Model.Generation = Registry.Reflection().Generation();
  std::vector<std::size_t> Log;
  std::array<std::size_t, IdentityCount> Observed{};
  VerifyObservations(Owner, Registry, Model, EntryDepth, Observed);

  const std::size_t Count = Case.Definitions.size();
  for (std::size_t Index = 0; Index < Count; ++Index) {
    const ModelManifest &Definition = ReversedProvision
                                          ? Case.Definitions[Count - 1 - Index]
                                          : Case.Definitions[Index];
    VerifyProvision(Owner, Registry, Model, Definition, Case.PoisonIdentity,
                    EntryDepth, Log, Observed);
  }

  for (const ScenarioRequest &Request : Case.Requests)
    VerifyLoad(Owner, Registry, Model, Request.Manifest, Request.ThrowKind,
               Case.PoisonIdentity, EntryDepth, Log, Observed);

  for (std::size_t Identity = 0; Identity < IdentityCount; ++Identity) {
    if (!Model.Loaded[Identity])
      continue;
    const ModelManifest Loaded = *Model.Loaded[Identity];

    VerifyLoad(Owner, Registry, Model, Loaded, 0, Case.PoisonIdentity,
               EntryDepth, Log, Observed);

    ModelManifest Unequal = Loaded;
    Unequal.Documentation =
        (Loaded.Documentation + 1) % DocumentationPool.size();
    VerifyLoad(Owner, Registry, Model, Unequal, 0, Case.PoisonIdentity,
               EntryDepth, Log, Observed);

    ModelManifest Replacement = Loaded;
    Replacement.Version = (Loaded.Version + 1) % VersionPool.size();
    VerifyLoad(Owner, Registry, Model, Replacement, 0, Case.PoisonIdentity,
               EntryDepth, Log, Observed);
  }

  std::string Script("assert(Seeded(2, 3) == 5)");
  for (std::size_t Identity = 0; Identity < IdentityCount; ++Identity) {
    if (!Model.Loaded[Identity])
      continue;
    Script.append("\nassert(").append(NamespaceNames[Identity]);
    Script.append(".Tag == ").append(std::to_string(Identity + 1));
    Script.append(")");
  }
  RC_ASSERT(Owner.Execute(Script).IsSuccess());

  RC_ASSERT(Registry.RegisterConstant("Recovered", 1).IsSuccess());
  ++Model.Generation;
  VerifyObservations(Owner, Registry, Model, EntryDepth, Observed);

  return ObservedEnumeration(Registry.Reflection());
}

} // namespace

int RunLoadOnceModuleResolutionProperties() {

  const bool Passed = rc::check(

      "Load-once module resolution follows the semantic-version transaction "
      "model",
      [](const std::vector<std::uint8_t> &Shape) {
        ByteCursor Cursor(Shape);
        const Scenario Case = MakeScenario(Cursor);
        RC_ASSERT(!Case.Requests.empty());

        const std::string Forward = RunScenario(Case, false);
        const std::string Reversed = RunScenario(Case, true);
        RC_ASSERT(Forward == Reversed);
      });

  return Passed ? 0 : 1;
}
