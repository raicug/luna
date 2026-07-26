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

// Deterministic byte source. Equal bytes always rebuild the exact same
// scenario, so a shrunk counterexample replays the same load sequence.
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

// ---------------------------------------------------------------------------
// The independent model's vocabulary. Nothing below calls Luna's resolver, its
// registry, or its semantic-version comparison: precedence, satisfaction,
// selection, ordering, and the load state machine are all reimplemented here
// from the acceptance criteria, so an agreement is real evidence.
// ---------------------------------------------------------------------------

constexpr std::size_t IdentityCount = 4;

// Canonical sorted identity order is exactly index order, so the model can walk
// identities by index wherever resolution walks them by sorted name.
constexpr std::array<std::string_view, IdentityCount> IdentityTexts{
    "luna.alpha", "luna.bravo", "luna.charlie", "luna.delta"};

constexpr std::array<std::string_view, IdentityCount> NamespaceNames{
    "Alpha", "Bravo", "Charlie", "Delta"};

// One available version, described structurally so the model never reparses
// text. `1.1.0-rc.1` is the prerelease that makes `>=1.0.0` and `>=1.1.0`
// disagree, which is the standard precedence rule Requirement 10.3 names.
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

// One constraint, in the canonical order Luna sorts constraints into:
// comparator first, then version precedence. A subset of this pool taken in
// ascending index order is therefore already canonically ordered.
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

// Each identity exports its namespace and its constant. Canonical export order
// is qualified name order, which is again this pool's index order.
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

// One structural version, and the standard precedence comparison over it.
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

// Standard semantic-version precedence: the core triple numerically, a
// prerelease below its release, numeric identifiers numerically, alphanumeric
// identifiers by ASCII, and a longer identifier set above its shorter prefix.
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

// Constraint satisfaction is pure precedence, so a prerelease candidate is
// admitted or rejected by its prerelease rules rather than by its text.
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

// One accumulated constraint. The root request contributes an equality
// constraint on the exact requested version, which no pool entry can express.
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

// One normalized module definition in the model. Every field is already in the
// canonical form `ModuleManifest::Create` normalizes to - dependencies sorted
// and unique by identity, constraint and export sets sorted and deduplicated -
// so two equal model definitions build two equal manifests, and two unequal
// model definitions build two unequal manifests.
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

// Every available version of every identity, ascending by precedence.
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

// ---------------------------------------------------------------------------
// The independent resolver.
// ---------------------------------------------------------------------------

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

// One identity that entered the graph: every constraint accumulated for it, and
// the canonical path that first reached it.
struct ModelRecord final {
  std::vector<ModelConstraint> Constraints;
  std::vector<std::string> Origin;
};

[[nodiscard]] std::vector<std::string> Extend(std::vector<std::string> Origin,
                                              std::string Tail) {
  Origin.push_back(std::move(Tail));
  return Origin;
}

// Deterministic depth-first search over the selected graph. Children are
// visited in canonical dependency order, so a reported cycle is stable.
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

// Dependency-first canonical order of the selected graph.
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

// Resolves the graph rooted at one request: accumulate constraints by identity,
// visit identities in canonical order, and select the highest available version
// satisfying every accumulated constraint. An already loaded version acts as an
// equality pin.
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
    // An accumulated constraint added after a selection can invalidate it.
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

    // A loaded module pins its version, so a graph that needs another version
    // is a conflicting selection rather than a second load.
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

// ---------------------------------------------------------------------------
// The independent load state machine.
// ---------------------------------------------------------------------------

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

// What one load request must do. `Published` is the only outcome that changes
// anything at all.
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
  // Callbacks that ran, in the order they ran. A failed attempt may still have
  // executed part of the graph inside the transaction it poisons.
  std::vector<std::size_t> Executed;
  ModelResolution Resolution;
  std::vector<std::pair<std::size_t, ModelManifest>> Selections;
};

[[nodiscard]] ExpectedLoad PlanLoad(const ModelState &State,
                                    const ModelManifest &Request,
                                    std::size_t ThrowKind,
                                    std::size_t PoisonIdentity) {
  ExpectedLoad Expected;

  // Load-once classification against the committed registry.
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

  // An unequal definition of an available identity and version is a conflict,
  // never a replacement.
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

  // Dependency-first canonical order, skipping everything already loaded.
  std::vector<std::size_t> Order;
  for (const std::size_t Identity : Expected.Resolution.LoadOrder) {
    if (Identity != Request.Identity && State.Loaded[Identity])
      continue;
    Order.push_back(Identity);
  }

  // A callback that poisons its attempt stops the graph where it ran; a
  // requested callback that throws is contained after the whole graph ran.
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

// ---------------------------------------------------------------------------
// Canonical enumeration, from reflection and from the model.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// The real State, driven through the public surface only.
// ---------------------------------------------------------------------------

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

// One module's registration callback. Every identity declares its own namespace
// and one constant inside it, so a published module is observable through the
// real virtual machine and an unpublished one leaves its path absent.
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
    // A deliberately invalid segment is a deterministic staging failure whose
    // result this callback drops, which still poisons the whole load.
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

// ---------------------------------------------------------------------------
// One generated scenario.
// ---------------------------------------------------------------------------

struct ScenarioRequest final {
  ModelManifest Manifest;
  std::size_t ThrowKind = 0;
};

struct Scenario final {
  std::vector<ModelManifest> Definitions;
  std::vector<ScenarioRequest> Requests;
  // The identity whose callback poisons its attempt, or `IdentityCount` when no
  // callback misbehaves.
  std::size_t PoisonIdentity = IdentityCount;
};

[[nodiscard]] ModelManifest MakeModelManifest(ByteCursor &Cursor,
                                              std::size_t Identity,
                                              std::size_t Version) {
  ModelManifest Model;
  Model.Identity = Identity;
  Model.Version = Version;

  // Dependencies are generated in ascending identity order, which is exactly
  // the canonical order a manifest normalizes them into. Any identity may
  // depend on any other, so generated graphs include cycles.
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

  // Available definitions, generated per identity so a graph usually has
  // something to select and sometimes has nothing at all. Two definitions of
  // one identity and version would make the winner depend on provision order,
  // so each identity and version appears at most once here and the
  // unequal-definition conflict is probed explicitly later.
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
    // Either the exact available definition, or an independently generated one
    // that may collide unequally with what is already available.
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

// ---------------------------------------------------------------------------
// What the real State must observe after every operation.
// ---------------------------------------------------------------------------

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

    // A published module owns its namespace path; an unpublished one leaves it
    // exactly as absent as it was before the attempt.
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
  // Availability is pure Luna-side metadata: it runs no callback and publishes
  // nothing at all.
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
  // Dependency callbacks run dependency-first in canonical order inside the one
  // transaction, and an idempotent repeat reruns none of them.
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
      // Every rejection carries the canonical dependency path that reached it.
      const std::string Path = Expected.Resolution.PathText();
      RC_ASSERT(!Path.empty());
      RC_ASSERT(Message.find("(dependency path: " + Path + ")") !=
                std::string::npos);
    }
  }

  // Only a published load derives a new committed bundle. Every other outcome
  // keeps the exact immutable symbol, reflection, and type generation the
  // attempt started from.
  if (Expected.Outcome != ExpectedOutcome::Published)
    RC_ASSERT(Hooks::GenerationsOf(Owner) == Generations);

  ApplyLoad(Model, Request, Expected);
  VerifyObservations(Owner, Registry, Model, EntryDepth, Observed);
}

// Drives one generated scenario through the public surface and returns the
// canonical module enumeration it published.
[[nodiscard]] std::string RunScenario(const Scenario &Case,
                                      bool ReversedProvision) {
  Luna::State Owner;
  RC_ASSERT(Owner.IsReady());
  Luna::BindingRegistry Registry = Owner.Bindings();

  const auto Depth = Hooks::ObserveRootStackDepth(Owner);
  RC_ASSERT(Depth.has_value());
  const int EntryDepth = *Depth;

  // One committed binding every load in this scenario must preserve.
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

  // Re-registering a loaded module: the identical definition is idempotent, a
  // same-version unequal definition conflicts, and a different version of a
  // loaded identity conflicts too.
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

  // Every published module is usable through the real virtual machine, the
  // pre-load binding still dispatches, and the State still accepts work.
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
  // **Validates: Requirements 10.3, 10.4, 10.5, 10.6, 10.7, 10.8, 10.10**
  // clang-format off
  // Feature: reflection-driven-binding-system, Property 23: Load-once module resolution follows the semantic-version transaction model
  const bool Passed = rc::check(
      // clang-format on
      "Load-once module resolution follows the semantic-version transaction "
      "model",
      [](const std::vector<std::uint8_t> &Shape) {
        ByteCursor Cursor(Shape);
        const Scenario Case = MakeScenario(Cursor);
        RC_ASSERT(!Case.Requests.empty());

        // Availability never depends on provision order, so the same scenario
        // provided forward and reversed must publish the same canonically
        // ordered graph.
        const std::string Forward = RunScenario(Case, false);
        const std::string Reversed = RunScenario(Case, true);
        RC_ASSERT(Forward == Reversed);
      });

  return Passed ? 0 : 1;
}
