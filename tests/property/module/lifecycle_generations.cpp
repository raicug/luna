// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/state/state.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/module/lifecycle.hpp"
#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/transaction/lifecycle_publication.hpp"
#include "state/transaction/lifecycle_staging.hpp"
#include "state/userdata/identity.hpp"

#include <rapidcheck.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::Detail::LifecycleAffectedKind;
using Luna::Detail::LifecycleAffectedKindText;
using Luna::Detail::LifecycleAnalysis;
using Luna::Detail::LifecycleBlockerKind;
using Luna::Detail::LifecycleBlockerKindText;
using Luna::Detail::LifecycleCacheEntry;
using Luna::Detail::LifecycleCacheKind;
using Luna::Detail::LifecycleCacheKindText;
using Luna::Detail::LifecycleCommitAttempt;
using Luna::Detail::LifecycleCommitObservation;
using Luna::Detail::LifecycleOperation;
using Luna::Detail::LifecyclePublishStatus;
using Luna::Detail::LifecycleRequest;
using Luna::Detail::LifecycleRetainedGeneration;
using Luna::Detail::LifecycleRootedReference;
using Luna::Detail::LifecycleStageStatus;
using Luna::Detail::LifecycleSubject;
using Luna::Detail::LifecycleSymbol;
using Luna::Detail::LifecycleUserdataPolicy;
using Luna::Detail::LifecycleUserdataValue;
using Luna::Detail::OwnershipModel;
using Luna::Detail::OwnershipModelText;
using Luna::Detail::StateFaultPoint;

class ByteCursor final {
public:
  explicit ByteCursor(std::span<const std::uint8_t> Bytes) noexcept
      : BytesValue(Bytes) {}

  [[nodiscard]] std::uint8_t Next() noexcept {
    const std::size_t Index = IndexValue++;
    if (BytesValue.empty())
      return static_cast<std::uint8_t>(Index * 41U + 7U);
    return BytesValue[Index % BytesValue.size()];
  }

  [[nodiscard]] std::size_t Pick(std::size_t Count) noexcept {
    return Count == 0 ? 0 : static_cast<std::size_t>(Next()) % Count;
  }

  [[nodiscard]] bool Flag() noexcept { return Pick(2) == 0; }

  [[nodiscard]] bool Rare() noexcept { return Pick(4) == 0; }

private:
  std::span<const std::uint8_t> BytesValue;
  std::size_t IndexValue = 0;
};

struct ModulePoolEntry final {
  std::string_view Identity;
  std::string_view Namespace;
  std::string_view Constant;
  std::string_view Function;
  std::string_view Version;
};

constexpr std::size_t ModuleCount = 4;
constexpr std::size_t CoreModule = 0;
constexpr std::size_t PhysicsModule = 1;
constexpr std::size_t RenderModule = 2;
constexpr std::size_t InterfaceModule = 3;

constexpr std::array<ModulePoolEntry, ModuleCount> ModulePool{
    {{"studio.core", "Core", "Level", "Scale", "1.0.0"},
     {"studio.physics", "Physics", "Gravity", "Impulse", "1.2.0"},
     {"studio.render", "Render", "Layers", "Draw", "2.0.0"},
     {"studio.interface", "Interface", "Depth", "Paint", "1.0.0"}}};

constexpr std::array<std::string_view, 2> ReplacementVersions{
    {"1.2.0", "1.3.0"}};

struct SymbolPoolEntry final {
  Luna::SymbolKind Kind;
  Luna::SymbolKind Alternate;
  std::string_view Name;
  std::string_view Signature;
  std::uint8_t TypeSeed;
  std::string_view Ownership;
};

constexpr std::size_t OwnedSymbolCount = 4;

constexpr std::array<SymbolPoolEntry, OwnedSymbolCount> SymbolPool{
    {{Luna::SymbolKind::Namespace, Luna::SymbolKind::Module, "Physics", "", 1,
      ""},
     {Luna::SymbolKind::OverloadSet, Luna::SymbolKind::Method,
      "Physics.Impulse", "(int) -> int", 2, ""},
     {Luna::SymbolKind::Class, Luna::SymbolKind::Enumeration, "Physics.Body",
      "", 3, "lua_owned|default|"},
     {Luna::SymbolKind::Method, Luna::SymbolKind::Property,
      "Physics.Body.Length", "() -> double", 4, "borrowed|default|"}}};

constexpr std::string_view AddedSymbolName = "Physics.Torque";
constexpr Luna::SymbolKind AddedSymbolKind = Luna::SymbolKind::OverloadSet;
constexpr std::string_view UserdataClassName = "Physics.Body";
constexpr std::uint64_t UserdataNonce = 7;
constexpr std::string_view RootedDetail =
    "a native lifetime handle roots this userdata";

enum class CompatMode : std::uint8_t {
  Retained,
  Removed,
  ChangedKind,
  ChangedTypeId,
  ChangedDescriptor,
  ChangedSignature,
  ChangedOwnership
};

constexpr std::size_t CompatModeCount = 7;

struct CachePoolEntry final {
  LifecycleCacheKind Kind;
  std::string_view Subject;
};

constexpr std::size_t SubjectCacheCount = 7;

constexpr std::array<CachePoolEntry, SubjectCacheCount> SubjectCachePool{
    {{LifecycleCacheKind::FrozenLookup, "Physics.Impulse"},
     {LifecycleCacheKind::FrozenLookup, "Render.Draw"},
     {LifecycleCacheKind::FrozenNamespace, "Physics"},
     {LifecycleCacheKind::FrozenModule, "studio.physics"},
     {LifecycleCacheKind::FrozenMetatable, "Physics.Body"},
     {LifecycleCacheKind::LazyMemberValue, "<lazy member values>"},
     {LifecycleCacheKind::NativeIdentity, "Physics.Body#7"}}};

constexpr std::size_t PlanCacheCount = 4;

constexpr std::array<CachePoolEntry, PlanCacheCount> PlanCachePool{
    {{LifecycleCacheKind::FrozenLookup, "Physics.Impulse"},
     {LifecycleCacheKind::FrozenMetatable, "Physics.Body"},
     {LifecycleCacheKind::LazyMemberValue, "<lazy member values>"},
     {LifecycleCacheKind::NativeIdentity, "Physics.Body#7"}}};

constexpr std::size_t OwnershipCount = 3;

constexpr std::array<OwnershipModel, OwnershipCount> OwnershipPool{
    {OwnershipModel::LuaOwned, OwnershipModel::Borrowed,
     OwnershipModel::Shared}};

constexpr std::string_view PlanRemovedTypeName = "Physics.Body";

struct BlockerPoolEntry final {
  LifecycleBlockerKind Kind;
  std::string_view Subject;
  std::string_view Detail;
};

constexpr std::size_t PlanBlockerCount = 3;

constexpr std::array<BlockerPoolEntry, PlanBlockerCount> PlanBlockerPool{
    {{LifecycleBlockerKind::DependentModule, "studio.render",
      "the dependent module remains loaded"},
     {LifecycleBlockerKind::LiveUserdata, "Physics.Body#7",
      "live userdata prevents removing class 'Physics.Body'"},
     {LifecycleBlockerKind::RootedReference, "Physics.Body#7",
      "a rooted reference cannot be invalidated safely"}}};

struct FaultPoolEntry final {
  bool Injected;
  StateFaultPoint Point;
};

constexpr std::size_t FaultCount = 11;

constexpr std::array<FaultPoolEntry, FaultCount> FaultPool{
    {{false, StateFaultPoint::LifecyclePublication},
     {true, StateFaultPoint::LifecycleCallback},
     {true, StateFaultPoint::LifecycleModuleStaging},
     {true, StateFaultPoint::BindingPathJournal},
     {true, StateFaultPoint::LifecycleTypeStaging},
     {true, StateFaultPoint::LifecycleMigration},
     {true, StateFaultPoint::LifecycleReflectionStaging},
     {true, StateFaultPoint::LifecycleCachePreparation},
     {true, StateFaultPoint::LifecycleDispatchStaging},
     {true, StateFaultPoint::LifecyclePublication},
     {true, StateFaultPoint::LifecycleGenerationPublication}}};

} // namespace
namespace {

[[nodiscard]] bool ScopeContains(std::string_view Scope,
                                 std::string_view Candidate) noexcept {
  if (Scope.empty() || Candidate.size() < Scope.size())
    return false;
  if (Candidate.compare(0, Scope.size(), Scope) != 0)
    return false;
  if (Candidate.size() == Scope.size())
    return true;
  return Candidate[Scope.size()] == '.';
}

[[nodiscard]] bool IsCallableSymbol(Luna::SymbolKind Kind) noexcept {
  switch (Kind) {
  case Luna::SymbolKind::OverloadSet:
  case Luna::SymbolKind::FunctionCandidate:
  case Luna::SymbolKind::Constructor:
  case Luna::SymbolKind::Factory:
  case Luna::SymbolKind::Method:
  case Luna::SymbolKind::StaticMethod:
  case Luna::SymbolKind::Operator:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool IsClassSymbol(Luna::SymbolKind Kind) noexcept {
  switch (Kind) {
  case Luna::SymbolKind::Class:
  case Luna::SymbolKind::Constructor:
  case Luna::SymbolKind::Factory:
  case Luna::SymbolKind::Method:
  case Luna::SymbolKind::StaticMethod:
  case Luna::SymbolKind::Property:
  case Luna::SymbolKind::Field:
  case Luna::SymbolKind::Operator:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool IsMemberSymbol(Luna::SymbolKind Kind) noexcept {
  switch (Kind) {
  case Luna::SymbolKind::Constructor:
  case Luna::SymbolKind::Factory:
  case Luna::SymbolKind::Method:
  case Luna::SymbolKind::StaticMethod:
  case Luna::SymbolKind::Property:
  case Luna::SymbolKind::Field:
  case Luna::SymbolKind::Operator:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] LifecycleAffectedKind
ModelAffectedKind(Luna::SymbolKind Kind) noexcept {
  switch (Kind) {
  case Luna::SymbolKind::Namespace:
  case Luna::SymbolKind::Module:
    return LifecycleAffectedKind::Namespace;
  case Luna::SymbolKind::Class:
  case Luna::SymbolKind::Enumeration:
  case Luna::SymbolKind::Type:
    return LifecycleAffectedKind::Type;
  default:
    break;
  }
  return IsCallableSymbol(Kind) ? LifecycleAffectedKind::Function
                                : LifecycleAffectedKind::ReflectionRecord;
}

[[nodiscard]] std::string OwningClassOf(std::string_view QualifiedName) {
  const std::size_t Separator = QualifiedName.rfind('.');
  if (Separator == std::string_view::npos)
    return std::string();
  return std::string(QualifiedName.substr(0, Separator));
}

[[nodiscard]] std::string AlteredSignature(const SymbolPoolEntry &Entry) {
  return std::string(Entry.Signature).append(" -- altered");
}

[[nodiscard]] std::string AlteredOwnership(const SymbolPoolEntry &Entry) {
  return std::string(Entry.Ownership).append("shared");
}

[[nodiscard]] std::string UserdataSubjectText() {
  return std::string(UserdataClassName)
      .append("#")
      .append(std::to_string(UserdataNonce));
}

[[nodiscard]] Luna::SemanticVersion Version(std::string_view Text) {
  const auto Parsed = Luna::SemanticVersion::TryParse(Text);
  return Parsed ? *Parsed : Luna::SemanticVersion();
}

[[nodiscard]] Luna::ModuleDependency Requires(std::string_view Identity,
                                              std::string_view Constraint) {
  Luna::ModuleDependency Declared;
  Declared.Identity = std::string(Identity);
  if (const auto Parsed = Luna::VersionConstraint::TryParse(Constraint))
    Declared.Constraints.push_back(*Parsed);
  return Declared;
}

[[nodiscard]] Luna::ModuleManifest
Manifest(std::string_view Identity, std::string_view VersionText,
         std::string_view NamespaceName,
         std::vector<Luna::ModuleDependency> Dependencies) {
  std::vector<Luna::ModuleExport> Exports;
  Luna::ModuleExport Exported;
  Exported.Kind = Luna::SymbolKind::Namespace;
  Exported.Name = std::string(NamespaceName);
  Exports.push_back(std::move(Exported));

  auto Created = Luna::ModuleManifest::TryCreate(
      std::string(Identity), Version(VersionText), std::move(Dependencies),
      std::string("A generated lifecycle module."), std::move(Exports));
  return Created ? std::move(*Created) : Luna::ModuleManifest();
}

[[nodiscard]] std::string
ConstraintText(const std::vector<Luna::VersionConstraint> &Constraints) {
  std::string Text;
  for (const Luna::VersionConstraint &Constraint : Constraints) {
    if (!Text.empty())
      Text.append(", ");
    Text.append(Constraint.ToString());
  }
  return Text.empty() ? std::string("no constraint") : Text;
}

[[nodiscard]] std::string JoinPath(const std::vector<std::string> &Path) {
  std::string Text;
  for (const std::string &Step : Path) {
    if (!Text.empty())
      Text.append(" -> ");
    Text.append(Step);
  }
  return Text;
}

[[nodiscard]] std::string Join(const std::vector<std::string> &Values,
                               std::string_view Separator) {
  std::string Text;
  for (const std::string &Value : Values) {
    if (!Text.empty())
      Text.append(Separator);
    Text.append(Value);
  }
  return Text;
}

[[nodiscard]] bool Contains(const std::vector<std::string> &Values,
                            std::string_view Wanted) {
  return std::find(Values.begin(), Values.end(), Wanted) != Values.end();
}

[[nodiscard]] bool Contains(const std::set<std::string> &Values,
                            const std::string &Wanted) {
  return Values.find(Wanted) != Values.end();
}

[[nodiscard]] bool OverlapsAny(const std::set<std::string> &Names,
                               std::string_view Candidate) {
  for (const std::string &Name : Names) {
    if (Name == Candidate || ScopeContains(Name, Candidate) ||
        ScopeContains(Candidate, Name))
      return true;
  }
  return false;
}

[[nodiscard]] std::vector<std::string>
CacheText(const std::vector<LifecycleCacheEntry> &Entries) {
  std::vector<std::string> Text;
  Text.reserve(Entries.size());
  for (const LifecycleCacheEntry &Entry : Entries)
    Text.push_back(std::string(LifecycleCacheKindText(Entry.Kind))
                       .append("|")
                       .append(Entry.Subject));
  return Text;
}

} // namespace
namespace {

struct Scenario final {
  bool LoadCore = false;
  bool LoadRender = false;
  bool LoadInterface = false;
  bool PhysicsRequiresCore = false;
  bool RenderRequiresPhysics = false;
  bool InterfaceRequiresRender = false;
  bool DependentPinsExactVersion = false;

  bool DynamicEnabled = true;
  bool Replacing = false;
  bool UnknownIdentity = false;
  std::size_t ReplacementVersion = 0;
  std::size_t ManifestFlaw = 0;
  std::size_t ReplacementRequirement = 0;

  std::array<CompatMode, OwnedSymbolCount> Compat{};
  bool AddSymbol = false;
  std::size_t Userdata = 0;
  std::size_t UserdataOwnership = 0;
  bool UserdataPublished = true;
  bool Rooted = false;
  std::size_t SubjectCacheMask = 0;
  bool SupersededGeneration = false;

  std::size_t PlanShape = 0;
  std::size_t PlanBlockerMask = 0;
  std::size_t PlanCacheMask = 0;
  std::size_t Fault = 0;
  bool RunCallback = false;
  bool CallbackFails = false;
  bool CallbackThrows = false;
  bool PublishWithoutDynamic = false;
  bool PublishWithoutStaging = false;
  bool RetainInvocation = false;
  std::size_t PlanUserdataPolicy = 0;
  bool PlanRemovesType = false;
};

[[nodiscard]] Scenario MakeScenario(ByteCursor &Cursor) {
  Scenario Case;

  Case.LoadCore = Cursor.Flag();
  Case.LoadRender = Cursor.Flag();
  Case.LoadInterface = Case.LoadRender && Cursor.Flag();
  Case.PhysicsRequiresCore = Case.LoadCore && Cursor.Flag();
  Case.RenderRequiresPhysics = Case.LoadRender && Cursor.Pick(3) != 0;
  Case.InterfaceRequiresRender = Case.LoadInterface && Cursor.Pick(3) != 0;
  Case.DependentPinsExactVersion = Cursor.Flag();

  Case.DynamicEnabled = Cursor.Pick(8) != 0;
  Case.Replacing = Cursor.Pick(3) != 0;
  Case.UnknownIdentity = Cursor.Pick(8) == 0;
  Case.ReplacementVersion = Cursor.Pick(ReplacementVersions.size());
  Case.ManifestFlaw = Cursor.Pick(6) < 4 ? 0 : Cursor.Pick(2) + 1;
  Case.ReplacementRequirement = Cursor.Pick(4) < 2 ? 0 : Cursor.Pick(2) + 1;

  for (std::size_t Index = 0; Index < OwnedSymbolCount; ++Index) {
    const std::size_t Chosen =
        Cursor.Pick(2) == 0 ? 0 : Cursor.Pick(CompatModeCount);
    Case.Compat[Index] = static_cast<CompatMode>(Chosen);
  }
  Case.AddSymbol = Cursor.Flag();
  Case.Userdata = Cursor.Pick(4);
  Case.UserdataOwnership = Cursor.Pick(OwnershipCount);
  Case.UserdataPublished = Cursor.Pick(4) != 0;
  Case.Rooted = Cursor.Flag();
  Case.SubjectCacheMask = Cursor.Pick(1U << SubjectCacheCount);
  Case.SupersededGeneration = Cursor.Flag();

  Case.PlanShape = Cursor.Pick(3);
  Case.PlanBlockerMask =
      Cursor.Pick(5) == 0 ? Cursor.Pick(1U << PlanBlockerCount) : 0;
  Case.PlanCacheMask = Cursor.Pick(1U << PlanCacheCount);
  Case.Fault = Cursor.Pick(5) < 3 ? 0 : Cursor.Pick(FaultCount);
  Case.RunCallback = Cursor.Flag();
  Case.CallbackFails = Cursor.Rare();
  Case.CallbackThrows = Cursor.Rare();
  Case.PublishWithoutDynamic = Cursor.Pick(8) == 0;
  Case.PublishWithoutStaging = Cursor.Pick(8) == 0;
  Case.RetainInvocation = Cursor.Flag();
  Case.PlanUserdataPolicy = Cursor.Pick(2) == 0 ? 0 : Cursor.Pick(4) + 1;
  Case.PlanRemovesType = Cursor.Flag();
  return Case;
}

struct ScenarioModules final {
  std::array<bool, ModuleCount> Loaded{};
  std::array<Luna::ModuleManifest, ModuleCount> Manifests;
};

[[nodiscard]] std::vector<Luna::ModuleDependency>
PhysicsDependencies(const Scenario &Case) {
  std::vector<Luna::ModuleDependency> Dependencies;
  if (Case.PhysicsRequiresCore)
    Dependencies.push_back(
        Requires(ModulePool[CoreModule].Identity, ">=1.0.0"));
  return Dependencies;
}

[[nodiscard]] ScenarioModules BuildModules(const Scenario &Case) {
  ScenarioModules Graph;
  Graph.Loaded[CoreModule] = Case.LoadCore;
  Graph.Loaded[PhysicsModule] = true;
  Graph.Loaded[RenderModule] = Case.LoadRender;
  Graph.Loaded[InterfaceModule] = Case.LoadInterface;

  Graph.Manifests[CoreModule] =
      Manifest(ModulePool[CoreModule].Identity, ModulePool[CoreModule].Version,
               ModulePool[CoreModule].Namespace, {});
  Graph.Manifests[PhysicsModule] = Manifest(
      ModulePool[PhysicsModule].Identity, ModulePool[PhysicsModule].Version,
      ModulePool[PhysicsModule].Namespace, PhysicsDependencies(Case));

  std::vector<Luna::ModuleDependency> RenderDependencies;
  if (Case.RenderRequiresPhysics)
    RenderDependencies.push_back(
        Requires(ModulePool[PhysicsModule].Identity,
                 Case.DependentPinsExactVersion ? "=1.2.0" : ">=1.0.0"));
  Graph.Manifests[RenderModule] = Manifest(
      ModulePool[RenderModule].Identity, ModulePool[RenderModule].Version,
      ModulePool[RenderModule].Namespace, std::move(RenderDependencies));

  std::vector<Luna::ModuleDependency> InterfaceDependencies;
  if (Case.InterfaceRequiresRender)
    InterfaceDependencies.push_back(
        Requires(ModulePool[RenderModule].Identity, ">=1.0.0"));
  Graph.Manifests[InterfaceModule] = Manifest(
      ModulePool[InterfaceModule].Identity, ModulePool[InterfaceModule].Version,
      ModulePool[InterfaceModule].Namespace, std::move(InterfaceDependencies));
  return Graph;
}

[[nodiscard]] std::string RequestIdentity(const Scenario &Case) {
  return Case.UnknownIdentity ? std::string("studio.absent")
                              : std::string(ModulePool[PhysicsModule].Identity);
}

[[nodiscard]] Luna::ModuleManifest ReplacementManifest(const Scenario &Case) {
  if (Case.ManifestFlaw == 1)
    return Luna::ModuleManifest();

  std::vector<Luna::ModuleDependency> Dependencies = PhysicsDependencies(Case);
  if (Case.ReplacementRequirement == 1)
    Dependencies.push_back(Requires("studio.audio", ">=1.0.0"));
  if (Case.ReplacementRequirement == 2)
    Dependencies.push_back(
        Requires(ModulePool[CoreModule].Identity, ">=9.0.0"));

  const std::size_t Named =
      Case.ManifestFlaw == 2 ? RenderModule : PhysicsModule;
  return Manifest(ModulePool[Named].Identity,
                  ReplacementVersions[Case.ReplacementVersion],
                  ModulePool[Named].Namespace, std::move(Dependencies));
}

struct DeclaredSymbol final {
  std::string Name;
  Luna::SymbolKind Kind = Luna::SymbolKind::Namespace;
};

[[nodiscard]] std::vector<DeclaredSymbol>
DeclaredSymbols(const Scenario &Case) {
  std::vector<DeclaredSymbol> Declared;
  if (!Case.Replacing)
    return Declared;

  for (std::size_t Index = 0; Index < OwnedSymbolCount; ++Index) {
    if (Case.Compat[Index] == CompatMode::Removed)
      continue;
    const SymbolPoolEntry &Entry = SymbolPool[Index];
    DeclaredSymbol Symbol;
    Symbol.Name = std::string(Entry.Name);
    Symbol.Kind = Case.Compat[Index] == CompatMode::ChangedKind
                      ? Entry.Alternate
                      : Entry.Kind;
    Declared.push_back(std::move(Symbol));
  }
  if (Case.AddSymbol) {
    DeclaredSymbol Added;
    Added.Name = std::string(AddedSymbolName);
    Added.Kind = AddedSymbolKind;
    Declared.push_back(std::move(Added));
  }
  std::sort(Declared.begin(), Declared.end(),
            [](const DeclaredSymbol &Left, const DeclaredSymbol &Right) {
              return Left.Name < Right.Name;
            });
  return Declared;
}

[[nodiscard]] Luna::TypeId TypeFrom(std::uint8_t Seed) {
  Luna::TypeId::Storage Bytes{};
  Bytes[0] = Seed;
  return Luna::TypeId::FromBytes(Bytes);
}

[[nodiscard]] LifecycleSymbol OwnedSymbol(std::size_t Index) {
  const SymbolPoolEntry &Entry = SymbolPool[Index];
  LifecycleSymbol Declared;
  Declared.Kind = Entry.Kind;
  Declared.QualifiedName = std::string(Entry.Name);
  Declared.Signature = std::string(Entry.Signature);
  Declared.Type = TypeFrom(Entry.TypeSeed);
  Declared.Descriptor =
      Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Int32);
  Declared.OwnershipText = std::string(Entry.Ownership);
  Declared.ModuleIdentity = std::string(ModulePool[PhysicsModule].Identity);
  return Declared;
}

[[nodiscard]] LifecycleSymbol ReplacementSymbol(const Scenario &Case,
                                                std::size_t Index) {
  const SymbolPoolEntry &Entry = SymbolPool[Index];
  LifecycleSymbol Declared = OwnedSymbol(Index);
  switch (Case.Compat[Index]) {
  case CompatMode::ChangedKind:
    Declared.Kind = Entry.Alternate;
    break;
  case CompatMode::ChangedTypeId:
    Declared.Type = TypeFrom(static_cast<std::uint8_t>(Entry.TypeSeed + 32U));
    break;
  case CompatMode::ChangedDescriptor:
    Declared.Descriptor =
        Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Double);
    break;
  case CompatMode::ChangedSignature:
    Declared.Signature = AlteredSignature(Entry);
    break;
  case CompatMode::ChangedOwnership:
    Declared.OwnershipText = AlteredOwnership(Entry);
    break;
  default:
    break;
  }
  return Declared;
}

[[nodiscard]] LifecycleRequest BuildRequest(const Scenario &Case) {
  LifecycleRequest Request;
  Request.Operation = Case.Replacing ? LifecycleOperation::Replacement
                                     : LifecycleOperation::Unload;
  Request.Identity = RequestIdentity(Case);
  if (!Case.Replacing)
    return Request;

  Request.Replacement = ReplacementManifest(Case);
  for (std::size_t Index = 0; Index < OwnedSymbolCount; ++Index) {
    if (Case.Compat[Index] == CompatMode::Removed)
      continue;
    Request.ReplacementSymbols.push_back(ReplacementSymbol(Case, Index));
  }
  if (Case.AddSymbol) {
    LifecycleSymbol Added;
    Added.Kind = AddedSymbolKind;
    Added.QualifiedName = std::string(AddedSymbolName);
    Added.Signature = "(int) -> int";
    Added.Type = TypeFrom(64);
    Added.Descriptor =
        Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Int32);
    Added.ModuleIdentity = std::string(ModulePool[PhysicsModule].Identity);
    Request.ReplacementSymbols.push_back(std::move(Added));
  }
  return Request;
}

[[nodiscard]] LifecycleSubject BuildSubject(const Scenario &Case,
                                            const ScenarioModules &Graph) {
  LifecycleSubject Subject;
  Subject.DynamicLifecycleEnabled = Case.DynamicEnabled;

  for (std::size_t Index = 0; Index < ModuleCount; ++Index) {
    if (Graph.Loaded[Index])
      Subject.LoadedModules.push_back(Graph.Manifests[Index]);
  }

  for (std::size_t Index = 0; Index < OwnedSymbolCount; ++Index)
    Subject.Symbols.push_back(OwnedSymbol(Index));

  LifecycleSymbol Foreign;
  Foreign.Kind = Luna::SymbolKind::OverloadSet;
  Foreign.QualifiedName = "Render.Draw";
  Foreign.Signature = "(int) -> int";
  Foreign.Type = TypeFrom(96);
  Foreign.Descriptor =
      Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Int32);
  Foreign.ModuleIdentity = std::string(ModulePool[RenderModule].Identity);
  Subject.Symbols.push_back(std::move(Foreign));

  Subject.DispatchSlots.push_back({1, "Physics.Impulse", true});
  Subject.DispatchSlots.push_back({2, "Physics.Body.Length", true});
  Subject.DispatchSlots.push_back({3, "Render.Draw", true});

  if (Case.Userdata != 0) {
    LifecycleUserdataValue Value;
    Value.ClassQualifiedName = std::string(UserdataClassName);
    Value.Type = TypeFrom(3);
    Value.Nonce = UserdataNonce;
    Value.Ownership = OwnershipPool[Case.UserdataOwnership];
    Value.IsPublished = Case.UserdataPublished;
    Value.RemainsValid = Case.Userdata == 1;
    Value.MigrationAvailable = Case.Userdata == 2;
    Subject.LiveUserdata.push_back(std::move(Value));
  }

  for (std::size_t Index = 0; Index < SubjectCacheCount; ++Index) {
    if ((Case.SubjectCacheMask & (1U << Index)) == 0)
      continue;
    Subject.Caches.push_back({SubjectCachePool[Index].Kind,
                              std::string(SubjectCachePool[Index].Subject)});
  }

  if (Case.Rooted) {
    LifecycleRootedReference Rooted;
    Rooted.Subject = UserdataSubjectText();
    Rooted.Detail = std::string(RootedDetail);
    Subject.RootedReferences.push_back(std::move(Rooted));
  }

  LifecycleRetainedGeneration Current;
  Current.Number = 4;
  Current.IsCurrent = true;
  Current.Invocations = 1;
  Subject.RetainedGenerations.push_back(Current);
  if (Case.SupersededGeneration) {
    LifecycleRetainedGeneration Superseded;
    Superseded.Number = 3;
    Superseded.LifecycleJournals = 1;
    Subject.RetainedGenerations.push_back(Superseded);
  }
  return Subject;
}

} // namespace
namespace {

struct ModelBlocker final {
  LifecycleBlockerKind Kind = LifecycleBlockerKind::UnsupportedDynamicMode;
  std::string Subject;
  std::string Detail;
  std::vector<std::string> Path;

  [[nodiscard]] std::string Text() const {
    std::string Result(LifecycleBlockerKindText(Kind));
    Result.push_back('|');
    Result.append(Subject);
    Result.push_back('|');
    Result.append(Detail);
    const std::string Joined = JoinPath(Path);
    if (!Joined.empty()) {
      Result.push_back('|');
      Result.append(Joined);
    }
    return Result;
  }
};

[[nodiscard]] bool BlockerPrecedes(const ModelBlocker &Left,
                                   const ModelBlocker &Right) {
  const auto LeftKind = static_cast<std::uint8_t>(Left.Kind);
  const auto RightKind = static_cast<std::uint8_t>(Right.Kind);
  if (LeftKind != RightKind)
    return LeftKind < RightKind;
  if (Left.Subject != Right.Subject)
    return Left.Subject < Right.Subject;
  if (Left.Detail != Right.Detail)
    return Left.Detail < Right.Detail;
  return JoinPath(Left.Path) < JoinPath(Right.Path);
}

[[nodiscard]] bool BlockerEquals(const ModelBlocker &Left,
                                 const ModelBlocker &Right) {
  return Left.Kind == Right.Kind && Left.Subject == Right.Subject &&
         Left.Detail == Right.Detail && Left.Path == Right.Path;
}

struct ModelAffected final {
  LifecycleAffectedKind Kind = LifecycleAffectedKind::Function;
  std::uint64_t Ordinal = 0;
  std::string Subject;
  std::string Detail;

  [[nodiscard]] std::string Text() const {
    std::string Result(LifecycleAffectedKindText(Kind));
    Result.push_back('|');
    Result.append(Subject);
    Result.push_back('|');
    Result.append(Detail);
    return Result;
  }
};

[[nodiscard]] bool AffectedPrecedes(const ModelAffected &Left,
                                    const ModelAffected &Right) {
  const auto LeftKind = static_cast<std::uint8_t>(Left.Kind);
  const auto RightKind = static_cast<std::uint8_t>(Right.Kind);
  if (LeftKind != RightKind)
    return LeftKind < RightKind;
  if (Left.Ordinal != Right.Ordinal)
    return Left.Ordinal < Right.Ordinal;
  if (Left.Subject != Right.Subject)
    return Left.Subject < Right.Subject;
  return Left.Detail < Right.Detail;
}

[[nodiscard]] bool AffectedEquals(const ModelAffected &Left,
                                  const ModelAffected &Right) {
  return Left.Kind == Right.Kind && Left.Ordinal == Right.Ordinal &&
         Left.Subject == Right.Subject && Left.Detail == Right.Detail;
}

struct ModelDependent final {
  std::size_t Index = 0;
  std::vector<std::string> Path;
};

[[nodiscard]] std::vector<ModelDependent>
DependentsOf(const Scenario &Case, const ScenarioModules &Graph) {
  std::vector<ModelDependent> Dependents;
  if (Case.UnknownIdentity)
    return Dependents;

  if (!Graph.Loaded[RenderModule] || !Case.RenderRequiresPhysics)
    return Dependents;

  ModelDependent Render;
  Render.Index = RenderModule;
  Render.Path = {Graph.Manifests[RenderModule].Key(),
                 Graph.Manifests[PhysicsModule].Key()};
  Dependents.push_back(std::move(Render));

  if (Graph.Loaded[InterfaceModule] && Case.InterfaceRequiresRender) {
    ModelDependent Interface;
    Interface.Index = InterfaceModule;
    Interface.Path = {Graph.Manifests[InterfaceModule].Key(),
                      Graph.Manifests[RenderModule].Key(),
                      Graph.Manifests[PhysicsModule].Key()};
    Dependents.push_back(std::move(Interface));
  }
  return Dependents;
}

struct ExpectedAnalysis final {
  std::vector<std::string> Blockers;
  std::vector<std::string> Affected;
  std::vector<std::string> Removed;
  std::vector<std::string> Retained;
  std::vector<std::string> RemovedTypes;
  std::vector<std::string> Caches;

  [[nodiscard]] std::string Digest() const {
    std::string Text("blockers=");
    Text.append(Join(Blockers, ";")).append("|affected=");
    Text.append(Join(Affected, ";")).append("|removed=");
    Text.append(Join(Removed, ";")).append("|retained=");
    Text.append(Join(Retained, ";")).append("|types=");
    Text.append(Join(RemovedTypes, ";")).append("|caches=");
    Text.append(Join(Caches, ";"));
    return Text;
  }
};

} // namespace
namespace {

[[nodiscard]] ExpectedAnalysis
ExpectedAnalysisOf(const Scenario &Case, const ScenarioModules &Graph,
                   const LifecycleRequest &Request) {
  std::vector<ModelBlocker> Blockers;
  std::vector<ModelAffected> Affected;

  const std::string Identity = Request.Identity;
  const bool KnownModule = !Case.UnknownIdentity;
  const bool Replacing = Case.Replacing;

  if (!Case.DynamicEnabled)
    Blockers.push_back({LifecycleBlockerKind::UnsupportedDynamicMode,
                        Identity,
                        "dynamic module lifecycle is unsupported for this "
                        "State, which remains load-only",
                        {}});
  if (!KnownModule)
    Blockers.push_back({LifecycleBlockerKind::UnknownModule,
                        Identity,
                        "the module is not loaded",
                        {}});

  bool Describes = Replacing;
  if (Replacing && !Request.Replacement.IsValid()) {
    Describes = false;
    Blockers.push_back({LifecycleBlockerKind::InvalidReplacementManifest,
                        Identity,
                        std::string("the replacement manifest is invalid (")
                            .append(Luna::ModuleManifestStatusText(
                                Request.Replacement.Status()))
                            .append(")"),
                        {}});
  } else if (Replacing && Request.Replacement.Identity() != Identity) {
    Describes = false;
    Blockers.push_back({LifecycleBlockerKind::IdentityMismatch,
                        Request.Replacement.Identity(),
                        std::string("the replacement manifest "
                                    "declares a module other than '")
                            .append(Identity)
                            .append("'"),
                        {}});
  }

  if (Describes) {
    std::vector<const Luna::ModuleDependency *> Required;
    for (const Luna::ModuleDependency &Dependency :
         Request.Replacement.Dependencies())
      Required.push_back(&Dependency);
    std::sort(Required.begin(), Required.end(),
              [](const Luna::ModuleDependency *Left,
                 const Luna::ModuleDependency *Right) {
                return Left->Identity < Right->Identity;
              });

    for (const Luna::ModuleDependency *Dependency : Required) {
      const Luna::ModuleManifest *Provider = nullptr;
      for (std::size_t Index = 0; Index < ModuleCount; ++Index) {
        if (Graph.Loaded[Index] &&
            Graph.Manifests[Index].Identity() == Dependency->Identity)
          Provider = &Graph.Manifests[Index];
      }
      if (Provider == nullptr) {
        Blockers.push_back({LifecycleBlockerKind::MissingDependency,
                            Dependency->Identity,
                            "the replacement requires "
                            "a module that is not "
                            "loaded",
                            {Request.Replacement.Key(), Dependency->Identity}});
        continue;
      }
      for (const Luna::VersionConstraint &Constraint :
           Dependency->Constraints) {
        if (Constraint.IsSatisfiedBy(Provider->Version()))
          continue;
        Blockers.push_back(
            {LifecycleBlockerKind::UnsatisfiedDependencyConstraint,
             Dependency->Identity,
             std::string("the replacement "
                         "requires ")
                 .append(ConstraintText(Dependency->Constraints))
                 .append(" but version ")
                 .append(Provider->Version().ToString())
                 .append(" is loaded"),
             {Request.Replacement.Key(), Provider->Key()}});
        break;
      }
    }
  }

  for (const ModelDependent &Dependent : DependentsOf(Case, Graph)) {
    const std::string DependentIdentity =
        Graph.Manifests[Dependent.Index].Identity();
    Affected.push_back({LifecycleAffectedKind::DependentModule, 0,
                        DependentIdentity, JoinPath(Dependent.Path)});
    if (!Replacing) {
      Blockers.push_back(
          {LifecycleBlockerKind::DependentModule, DependentIdentity,
           "the dependent module remains loaded", Dependent.Path});
      continue;
    }
    if (!Describes)
      continue;
    const Luna::ModuleDependency *Requirement =
        Graph.Manifests[Dependent.Index].FindDependency(Identity);
    if (Requirement == nullptr)
      continue;
    for (const Luna::VersionConstraint &Constraint : Requirement->Constraints) {
      if (Constraint.IsSatisfiedBy(Request.Replacement.Version()))
        continue;
      Blockers.push_back({LifecycleBlockerKind::UnsatisfiedDependencyConstraint,
                          DependentIdentity,
                          std::string("the dependent module requires ")
                              .append(ConstraintText(Requirement->Constraints))
                              .append(" of '")
                              .append(Identity)
                              .append("' but the replacement "
                                      "declares version ")
                              .append(Request.Replacement.Version().ToString()),
                          Dependent.Path});
      break;
    }
  }

  std::set<std::string> OwnedNames;
  std::set<std::string> RemovedNames;
  std::set<std::string> RetainedNames;
  std::set<std::string> IncompatibleNames;
  std::set<std::string> RemovedClasses;
  std::set<std::string> IncompatibleClasses;
  std::set<std::string> RemovedTypes;

  if (KnownModule) {
    for (std::size_t Index = 0; Index < OwnedSymbolCount; ++Index) {
      const SymbolPoolEntry &Entry = SymbolPool[Index];
      const std::string Name(Entry.Name);
      const CompatMode Mode = Case.Compat[Index];
      OwnedNames.insert(Name);

      const bool Present = Replacing && Mode != CompatMode::Removed;
      const bool Removed = !Present;
      bool Incompatible = false;

      if (Present && Mode == CompatMode::ChangedKind) {
        Incompatible = true;
        Blockers.push_back({LifecycleBlockerKind::IncompatibleDeclaration,
                            Name,
                            std::string("the replacement declares '")
                                .append(Luna::SymbolKindText(Entry.Alternate))
                                .append("' instead of '")
                                .append(Luna::SymbolKindText(Entry.Kind))
                                .append("'"),
                            {}});
      }
      if (Present && Mode == CompatMode::ChangedTypeId) {
        Incompatible = true;
        Blockers.push_back({LifecycleBlockerKind::IncompatibleType,
                            Name,
                            "the canonical type identity changed",
                            {}});
      } else if (Present && Mode == CompatMode::ChangedDescriptor) {
        Incompatible = true;
        Blockers.push_back({LifecycleBlockerKind::IncompatibleType,
                            Name,
                            "the canonical type descriptor changed",
                            {}});
      }
      if (Present && Mode == CompatMode::ChangedSignature &&
          IsCallableSymbol(Entry.Kind)) {
        Incompatible = true;
        Blockers.push_back({LifecycleBlockerKind::IncompatibleCallableSignature,
                            Name,
                            std::string("the replacement declares signature '")
                                .append(AlteredSignature(Entry))
                                .append("' instead of '")
                                .append(Entry.Signature)
                                .append("'"),
                            {}});
      }
      if (Present && Mode == CompatMode::ChangedOwnership &&
          IsClassSymbol(Entry.Kind)) {
        Incompatible = true;
        Blockers.push_back({LifecycleBlockerKind::IncompatibleClassOwnership,
                            Name,
                            std::string("the replacement declares ownership '")
                                .append(AlteredOwnership(Entry))
                                .append("' instead of '")
                                .append(Entry.Ownership)
                                .append("'"),
                            {}});
      }

      const std::string Standing =
          Removed ? "removed"
                  : (Incompatible ? "incompatible" : "retained compatibly");
      Affected.push_back({ModelAffectedKind(Entry.Kind), 0, Name, Standing});
      Affected.push_back({LifecycleAffectedKind::ReflectionRecord, 0, Name,
                          std::string(Luna::SymbolKindText(Entry.Kind))
                              .append(" ")
                              .append(Standing)});

      if (Removed) {
        RemovedNames.insert(Name);
        if (ModelAffectedKind(Entry.Kind) == LifecycleAffectedKind::Type)
          RemovedTypes.insert(Name);
      } else {
        RetainedNames.insert(Name);
        if (Incompatible)
          IncompatibleNames.insert(Name);
      }

      const std::string ClassName =
          Entry.Kind == Luna::SymbolKind::Class
              ? Name
              : (IsMemberSymbol(Entry.Kind) ? OwningClassOf(Name)
                                            : std::string());
      if (ClassName.empty())
        continue;
      if (Removed && (Entry.Kind == Luna::SymbolKind::Class || !Replacing))
        RemovedClasses.insert(ClassName);
      else if (Removed || Incompatible)
        IncompatibleClasses.insert(ClassName);
    }
  }

  for (const DeclaredSymbol &Symbol : DeclaredSymbols(Case)) {
    if (Contains(OwnedNames, Symbol.Name))
      continue;
    Affected.push_back(
        {ModelAffectedKind(Symbol.Kind), 0, Symbol.Name, "added"});
    Affected.push_back(
        {LifecycleAffectedKind::ReflectionRecord, 0, Symbol.Name,
         std::string(Luna::SymbolKindText(Symbol.Kind)).append(" added")});
  }

  const std::array<std::pair<std::uint64_t, std::string_view>, 3> Slots{
      {{1, "Physics.Impulse"}, {2, "Physics.Body.Length"}, {3, "Render.Draw"}}};
  for (const auto &Described : Slots) {
    bool Matched = false;
    bool Unavailable = false;
    for (const std::string &Name : OwnedNames) {
      if (Name != Described.second && !ScopeContains(Name, Described.second))
        continue;
      Matched = true;
      if (Contains(RemovedNames, Name))
        Unavailable = true;
    }
    if (!Matched)
      continue;
    Affected.push_back({LifecycleAffectedKind::Closure, Described.first,
                        std::string(Described.second),
                        Unavailable ? "becomes an unavailable entry"
                                    : "resolves the published generation"});
  }

  bool AffectsUserdata = false;
  if (Case.Userdata != 0 &&
      OverlapsAny(OwnedNames, std::string(UserdataClassName))) {
    AffectsUserdata = true;
    const bool RemainsValid = Case.Userdata == 1;
    const bool MigrationAvailable = Case.Userdata == 2;
    std::string Detail(
        OwnershipModelText(OwnershipPool[Case.UserdataOwnership]));
    Detail.append(Case.UserdataPublished ? ", published" : ", unpublished");
    if (RemainsValid)
      Detail.append(", remains valid");
    if (MigrationAvailable)
      Detail.append(", migration available");
    Affected.push_back({LifecycleAffectedKind::Userdata, UserdataNonce,
                        UserdataSubjectText(), std::move(Detail)});

    const std::string ClassName(UserdataClassName);
    if (Contains(RemovedClasses, ClassName) ||
        Contains(RemovedNames, ClassName)) {
      Blockers.push_back({LifecycleBlockerKind::LiveUserdata,
                          UserdataSubjectText(),
                          std::string("live userdata prevents removing class '")
                              .append(ClassName)
                              .append("'"),
                          {}});
    } else if (Contains(IncompatibleClasses, ClassName) ||
               Contains(IncompatibleNames, ClassName)) {
      if (!MigrationAvailable)
        Blockers.push_back(
            {LifecycleBlockerKind::UnavailableUserdataMigration,
             UserdataSubjectText(),
             std::string("no migration is available for incompatible class '")
                 .append(ClassName)
                 .append("'"),
             {}});
    } else if (!RemainsValid && !MigrationAvailable) {
      Blockers.push_back(
          {LifecycleBlockerKind::UnavailableUserdataMigration,
           UserdataSubjectText(),
           std::string("no continued-validity or migration policy is "
                       "declared for class '")
               .append(ClassName)
               .append("'"),
           {}});
    }
  }

  if (Case.Rooted) {
    const std::string Referenced(UserdataClassName);
    if (OverlapsAny(OwnedNames, Referenced)) {
      Affected.push_back({LifecycleAffectedKind::RootedReference, 0,
                          UserdataSubjectText(), std::string(RootedDetail)});
      if (Contains(RemovedNames, Referenced) ||
          Contains(RemovedClasses, Referenced) ||
          Contains(IncompatibleNames, Referenced) ||
          Contains(IncompatibleClasses, Referenced))
        Blockers.push_back({LifecycleBlockerKind::RootedReference,
                            UserdataSubjectText(),
                            std::string(RootedDetail),
                            {}});
    }
  }

  std::vector<LifecycleCacheEntry> Invalidated;
  for (std::size_t Index = 0; Index < SubjectCacheCount; ++Index) {
    if ((Case.SubjectCacheMask & (1U << Index)) == 0)
      continue;
    const CachePoolEntry &Entry = SubjectCachePool[Index];
    const std::string CacheSubject(Entry.Subject);
    bool Matched =
        CacheSubject == Identity || OverlapsAny(OwnedNames, CacheSubject);
    if (!Matched && AffectsUserdata &&
        (Entry.Kind == LifecycleCacheKind::LazyMemberValue ||
         Entry.Kind == LifecycleCacheKind::NativeIdentity))
      Matched = true;
    if (!Matched)
      continue;
    Affected.push_back({LifecycleAffectedKind::Cache, 0, CacheSubject,
                        std::string(LifecycleCacheKindText(Entry.Kind))});
    Invalidated.push_back({Entry.Kind, CacheSubject});
  }
  std::sort(
      Invalidated.begin(), Invalidated.end(),
      [](const LifecycleCacheEntry &Left, const LifecycleCacheEntry &Right) {
        if (Left.Kind != Right.Kind)
          return static_cast<std::uint8_t>(Left.Kind) <
                 static_cast<std::uint8_t>(Right.Kind);
        return Left.Subject < Right.Subject;
      });

  if (KnownModule) {
    Affected.push_back({LifecycleAffectedKind::RetainedGeneration, 4,
                        "generation 4",
                        "current; invocations=1, userdata_cleanups=0, "
                        "lifecycle_journals=0"});
    if (Case.SupersededGeneration)
      Affected.push_back({LifecycleAffectedKind::RetainedGeneration, 3,
                          "generation 3",
                          "superseded; invocations=0, userdata_cleanups=0, "
                          "lifecycle_journals=1"});
  }

  std::sort(Blockers.begin(), Blockers.end(), BlockerPrecedes);
  Blockers.erase(std::unique(Blockers.begin(), Blockers.end(), BlockerEquals),
                 Blockers.end());
  std::sort(Affected.begin(), Affected.end(), AffectedPrecedes);
  Affected.erase(std::unique(Affected.begin(), Affected.end(), AffectedEquals),
                 Affected.end());

  ExpectedAnalysis Expected;
  for (const ModelBlocker &Blocker : Blockers)
    Expected.Blockers.push_back(Blocker.Text());
  for (const ModelAffected &Item : Affected)
    Expected.Affected.push_back(Item.Text());
  Expected.Removed.assign(RemovedNames.begin(), RemovedNames.end());
  Expected.Retained.assign(RetainedNames.begin(), RetainedNames.end());
  Expected.RemovedTypes.assign(RemovedTypes.begin(), RemovedTypes.end());
  Expected.Caches = CacheText(Invalidated);
  return Expected;
}

[[nodiscard]] std::string VerifyAnalysis(const Scenario &Case,
                                         const ScenarioModules &Graph,
                                         bool Reversed) {
  LifecycleRequest Request = BuildRequest(Case);
  LifecycleSubject Subject = BuildSubject(Case, Graph);
  if (Reversed) {
    std::reverse(Request.ReplacementSymbols.begin(),
                 Request.ReplacementSymbols.end());
    std::reverse(Subject.LoadedModules.begin(), Subject.LoadedModules.end());
    std::reverse(Subject.Symbols.begin(), Subject.Symbols.end());
    std::reverse(Subject.DispatchSlots.begin(), Subject.DispatchSlots.end());
    std::reverse(Subject.Caches.begin(), Subject.Caches.end());
    std::reverse(Subject.RetainedGenerations.begin(),
                 Subject.RetainedGenerations.end());
  }

  const LifecycleAnalysis Analysis =
      Luna::Detail::AnalyzeLifecycleRequest(Request, Subject);
  const ExpectedAnalysis Expected = ExpectedAnalysisOf(Case, Graph, Request);

  RC_ASSERT(Analysis.BlockerText() == Expected.Blockers);
  RC_ASSERT(Analysis.Affected.Text() == Expected.Affected);
  RC_ASSERT(Analysis.RemovedSubjects == Expected.Removed);
  RC_ASSERT(Analysis.RetainedSubjects == Expected.Retained);
  RC_ASSERT(Analysis.RemovedTypes == Expected.RemovedTypes);
  RC_ASSERT(CacheText(Analysis.InvalidatedCaches) == Expected.Caches);
  RC_ASSERT(Analysis.IsPermitted() == Expected.Blockers.empty());

  ExpectedAnalysis Observed;
  Observed.Blockers = Analysis.BlockerText();
  Observed.Affected = Analysis.Affected.Text();
  Observed.Removed = Analysis.RemovedSubjects;
  Observed.Retained = Analysis.RetainedSubjects;
  Observed.RemovedTypes = Analysis.RemovedTypes;
  Observed.Caches = CacheText(Analysis.InvalidatedCaches);
  return Observed.Digest();
}

} // namespace
namespace {

[[nodiscard]] int Doubling(int Value) {
  return Value * 2;
}

struct ModuleCallback final {
  std::size_t Index = 0;

  void operator()(Luna::NamespaceBuilder &Builder) const {
    Luna::NamespaceBuilder Scope =
        Builder.RegisterNamespace(ModulePool[Index].Namespace);
    static_cast<void>(Scope.RegisterConstant(ModulePool[Index].Constant,
                                             static_cast<int>(Index) + 1));
    static_cast<void>(
        Scope.RegisterFunction(ModulePool[Index].Function, &Doubling));
  }
};

struct PlanPaths final {
  std::vector<std::string> RemovedSubjects;
  std::vector<std::string> RetainedPaths;
};

[[nodiscard]] PlanPaths PlanPathsOf(const Scenario &Case) {
  PlanPaths Paths;
  const std::size_t Shape = Case.Replacing ? Case.PlanShape : 0;
  switch (Shape) {
  case 1:
    Paths.RemovedSubjects = {"Physics.Gravity"};
    Paths.RetainedPaths = {"Physics.Impulse"};
    break;
  case 2:
    Paths.RetainedPaths = {"Physics.Impulse", "Physics.Gravity"};
    break;
  default:
    Paths.RemovedSubjects = {"Physics"};
    break;
  }
  return Paths;
}

[[nodiscard]] bool PlanRemovesPath(const PlanPaths &Paths,
                                   std::string_view Path) {
  if (Contains(Paths.RetainedPaths, Path))
    return false;
  for (const std::string &Subject : Paths.RemovedSubjects) {
    if (ScopeContains(Subject, Path))
      return true;
  }
  return false;
}

[[nodiscard]] bool IsRetainedScope(const PlanPaths &Paths,
                                   const std::string &Name) {
  for (const std::string &Retained : Paths.RetainedPaths) {
    if (ScopeContains(Name, Retained))
      return true;
  }
  return false;
}

// A staged plan declares continued validity for policy 1 and 3, an available
// migration for policy 2 and 3, and policy 4 declares neither so that staging
// must refuse the live value.
[[nodiscard]] bool PolicyRemainsValid(const Scenario &Case) {
  return Case.PlanUserdataPolicy == 1 || Case.PlanUserdataPolicy == 3;
}

[[nodiscard]] bool PolicyMigrates(const Scenario &Case) {
  return Case.PlanUserdataPolicy == 2 || Case.PlanUserdataPolicy == 3;
}

[[nodiscard]] bool PolicyRefused(const Scenario &Case) {
  return Case.PlanUserdataPolicy != 0 && !PolicyRemainsValid(Case) &&
         !PolicyMigrates(Case);
}

[[nodiscard]] bool FaultIs(const Scenario &Case, StateFaultPoint Point) {
  const FaultPoolEntry &Entry = FaultPool[Case.Fault];
  return Entry.Injected && Entry.Point == Point;
}

[[nodiscard]] LifecycleStageStatus ExpectedStageStatus(const Scenario &Case) {
  const bool Blocked = Case.PlanBlockerMask != 0;
  const bool ManifestRefused = Case.Replacing && Case.ManifestFlaw != 0;

  if (!Case.DynamicEnabled || Blocked || ManifestRefused)
    return LifecycleStageStatus::ValidationFailure;
  if (Case.RunCallback && (FaultIs(Case, StateFaultPoint::LifecycleCallback) ||
                           Case.CallbackFails || Case.CallbackThrows))
    return LifecycleStageStatus::CallbackFailure;
  if (FaultIs(Case, StateFaultPoint::LifecycleModuleStaging))
    return LifecycleStageStatus::AllocationFailure;
  if (FaultIs(Case, StateFaultPoint::BindingPathJournal))
    return LifecycleStageStatus::InstallationFailure;
  if (FaultIs(Case, StateFaultPoint::LifecycleTypeStaging))
    return LifecycleStageStatus::AllocationFailure;
  if (FaultIs(Case, StateFaultPoint::LifecycleMigration))
    return LifecycleStageStatus::MigrationFailure;
  if (PolicyRefused(Case))
    return LifecycleStageStatus::MigrationFailure;
  if (FaultIs(Case, StateFaultPoint::LifecycleReflectionStaging))
    return LifecycleStageStatus::AllocationFailure;
  if (FaultIs(Case, StateFaultPoint::LifecycleCachePreparation))
    return LifecycleStageStatus::CacheFailure;
  if (FaultIs(Case, StateFaultPoint::LifecycleDispatchStaging))
    return LifecycleStageStatus::PublicationFailure;
  if (FaultIs(Case, StateFaultPoint::LifecyclePublication))
    return LifecycleStageStatus::PublicationFailure;
  return LifecycleStageStatus::Prepared;
}

[[nodiscard]] LifecyclePublishStatus
ExpectedPublishStatus(const Scenario &Case, LifecycleStageStatus Stage) {
  if (!Case.DynamicEnabled || Case.PublishWithoutDynamic)
    return LifecyclePublishStatus::UnsupportedDynamicMode;
  if (Stage != LifecycleStageStatus::Prepared || Case.PublishWithoutStaging)
    return LifecyclePublishStatus::NothingStaged;
  if (FaultIs(Case, StateFaultPoint::LifecycleGenerationPublication))
    return LifecyclePublishStatus::GenerationFailure;
  return LifecyclePublishStatus::Published;
}

[[nodiscard]] std::vector<LifecycleCacheEntry>
PlanCaches(const Scenario &Case) {
  std::vector<LifecycleCacheEntry> Entries;
  for (std::size_t Index = 0; Index < PlanCacheCount; ++Index) {
    if ((Case.PlanCacheMask & (1U << Index)) == 0)
      continue;
    Entries.push_back(
        {PlanCachePool[Index].Kind, std::string(PlanCachePool[Index].Subject)});
  }
  return Entries;
}

[[nodiscard]] std::string RecordName(const std::string &Identity) {
  const std::size_t Marker = Identity.rfind('=');
  return Marker == std::string::npos ? Identity : Identity.substr(0, Marker);
}

[[nodiscard]] bool OwnedByPhysics(const std::string &Name) {
  return Name == "Physics" || Name.compare(0, 8, "Physics.") == 0;
}

// Every qualified name a publication must keep: the root
// registrations, the symbols of every other loaded module, and each
// retained path with all of the scopes it needs to stay reachable.
[[nodiscard]] std::vector<std::string>
SurvivingNames(const ScenarioModules &Graph, const PlanPaths &Paths) {
  std::vector<std::string> Names{"Baseline"};
  for (std::size_t Index = 0; Index < ModuleCount; ++Index) {
    if (!Graph.Loaded[Index] || Index == PhysicsModule)
      continue;
    const std::string Scope(ModulePool[Index].Namespace);
    Names.push_back(Scope);
    Names.push_back(Scope + "." + std::string(ModulePool[Index].Constant));
    Names.push_back(Scope + "." + std::string(ModulePool[Index].Function));
  }
  for (const std::string &Retained : Paths.RetainedPaths) {
    Names.push_back(Retained);
    for (std::size_t Position = Retained.find('.');
         Position != std::string::npos;
         Position = Retained.find('.', Position + 1))
      Names.push_back(Retained.substr(0, Position));
  }
  std::sort(Names.begin(), Names.end());
  Names.erase(std::unique(Names.begin(), Names.end()), Names.end());
  return Names;
}

[[nodiscard]] LifecycleCommitAttempt
BuildAttempt(const Scenario &Case, const PlanPaths &Paths, bool Reversed) {
  LifecycleCommitAttempt Request;
  Request.Staged.Plan.Operation = Case.Replacing
                                      ? LifecycleOperation::Replacement
                                      : LifecycleOperation::Unload;
  Request.Staged.Plan.Identity =
      std::string(ModulePool[PhysicsModule].Identity);
  Request.Staged.Plan.DynamicLifecycleEnabled = Case.DynamicEnabled;
  if (Case.Replacing)
    Request.Staged.Plan.Replacement = ReplacementManifest(Case);
  Request.Staged.Plan.RemovedSubjects = Paths.RemovedSubjects;
  Request.Staged.Plan.RetainedPaths = Paths.RetainedPaths;
  Request.Staged.Plan.InvalidatedCaches = PlanCaches(Case);

  if (Case.PlanUserdataPolicy != 0) {
    LifecycleUserdataPolicy Policy;
    Policy.Subject = UserdataSubjectText();
    Policy.ClassQualifiedName = std::string(UserdataClassName);
    Policy.RemainsValid = PolicyRemainsValid(Case);
    Policy.MigrationAvailable = PolicyMigrates(Case);
    Request.Staged.Plan.LiveUserdata.push_back(std::move(Policy));
  }
  if (Case.PlanRemovesType)
    Request.Staged.Plan.RemovedTypes = {std::string(PlanRemovedTypeName)};

  for (std::size_t Index = 0; Index < PlanBlockerCount; ++Index) {
    if ((Case.PlanBlockerMask & (1U << Index)) == 0)
      continue;
    Luna::Detail::LifecycleBlocker Blocker;
    Blocker.Kind = PlanBlockerPool[Index].Kind;
    Blocker.Subject = std::string(PlanBlockerPool[Index].Subject);
    Blocker.Detail = std::string(PlanBlockerPool[Index].Detail);
    Request.Staged.Blockers.push_back(std::move(Blocker));
  }

  Request.Staged.RunCallback = Case.RunCallback;
  Request.Staged.CallbackFails = Case.CallbackFails;
  Request.Staged.CallbackThrows = Case.CallbackThrows;
  Request.PublishWithoutDynamicLifecycle = Case.PublishWithoutDynamic;
  Request.PublishWithoutStaging = Case.PublishWithoutStaging;
  Request.RetainInvocationGeneration = Case.RetainInvocation;
  Request.SourceBeforePublication = "StaleImpulse = Physics.Impulse";
  Request.SourceAfterPublication =
      "local Result = StaleImpulse(21)\n"
      "if Result ~= 42 then error('the retained target changed') end";
  Request.ProbedPaths = {"Physics.Gravity", "Physics.Impulse", "Baseline"};

  if (Reversed) {
    std::reverse(Request.Staged.Plan.RemovedSubjects.begin(),
                 Request.Staged.Plan.RemovedSubjects.end());
    std::reverse(Request.Staged.Plan.RetainedPaths.begin(),
                 Request.Staged.Plan.RetainedPaths.end());
    std::reverse(Request.Staged.Plan.InvalidatedCaches.begin(),
                 Request.Staged.Plan.InvalidatedCaches.end());
    std::reverse(Request.Staged.Plan.RemovedTypes.begin(),
                 Request.Staged.Plan.RemovedTypes.end());
    std::reverse(Request.Staged.Blockers.begin(),
                 Request.Staged.Blockers.end());
  }
  return Request;
}

[[nodiscard]] std::string
ObservationDigest(const LifecycleCommitObservation &Observed) {
  std::vector<std::string> Fields;
  Fields.push_back(std::string("stage=").append(
      Luna::Detail::LifecycleStageStatusText(Observed.Staging.Status)));
  Fields.push_back(std::string("publish=")
                       .append(Luna::Detail::LifecyclePublishStatusText(
                           Observed.Publication.Status)));
  Fields.push_back(std::string("published=")
                       .append(Observed.Publication.IsPublished ? "1" : "0"));
  Fields.push_back(std::string("committed=")
                       .append(Observed.TransactionCommitted ? "1" : "0"));
  Fields.push_back(std::string("poisoned=")
                       .append(Observed.TransactionPoisoned ? "1" : "0"));
  Fields.push_back(std::string("generation=")
                       .append(std::to_string(Observed.GenerationAfter -
                                              Observed.GenerationBefore)));
  Fields.push_back(
      std::string("reflection=")
          .append(std::to_string(Observed.ReflectionGenerationAfter -
                                 Observed.ReflectionGenerationBefore)));
  Fields.push_back(
      std::string("dispatch=")
          .append(std::to_string(Observed.DispatchGenerationAfter -
                                 Observed.DispatchGenerationBefore)));
  Fields.push_back(std::string("modules=")
                       .append(std::to_string(Observed.ModuleCountBefore))
                       .append("->")
                       .append(std::to_string(Observed.ModuleCountAfter)));
  Fields.push_back(
      std::string("loaded=").append(Observed.ModuleStillLoaded ? "1" : "0"));
  Fields.push_back(std::string("version=").append(Observed.LoadedVersionAfter));
  Fields.push_back(
      std::string("probed=").append(Join(Observed.ProbedPathKindsAfter, ",")));
  Fields.push_back(std::string("cleared=")
                       .append(Join(Observed.Publication.ClearedPaths, ",")));
  Fields.push_back(
      std::string("unavailable=")
          .append(Join(Observed.Publication.UnavailableSlots, ",")));
  Fields.push_back(std::string("retained_slots=")
                       .append(Join(Observed.Publication.RetainedSlots, ",")));
  Fields.push_back(std::string("caches=").append(
      Join(Observed.Publication.InvalidatedCaches, ",")));
  Fields.push_back(std::string("source_before=")
                       .append(Observed.SourceBeforeSucceeded ? "1" : "0"));
  Fields.push_back(std::string("source_after=")
                       .append(Observed.SourceAfterSucceeded ? "1" : "0"));
  Fields.push_back(
      std::string("staged=").append(Join(Observed.Staging.Staged, ",")));
  Fields.push_back(std::string("probe=").append(Observed.RetainedProbe));
  Fields.push_back(
      std::string("resolves_old=")
          .append(Observed.RetainedGenerationResolvesOldTarget ? "1" : "0"));
  Fields.push_back(
      std::string("superseded=")
          .append(std::to_string(Observed.SupersededDispatchGenerations)));
  Fields.push_back(
      std::string("retained=")
          .append(std::to_string(Observed.RetainedDispatchGenerations)));
  Fields.push_back(std::string("reclaimed=")
                       .append(std::to_string(Observed.ReclaimedAfterRelease)));
  Fields.push_back(
      std::string("journal=")
          .append(std::to_string(Observed.LifecycleJournalRetainersAfter)));
  return Join(Fields, "|");
}

} // namespace
namespace {

[[nodiscard]] std::string VerifyPublication(const Scenario &Case,
                                            const ScenarioModules &Graph,
                                            bool Reversed) {
  Luna::State Owner;
  RC_ASSERT(Owner.IsReady());
  Luna::BindingRegistry Registry = Owner.Bindings();

  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  RC_ASSERT(EntryDepth.has_value());
  RC_ASSERT(Registry.RegisterFunction("Baseline", &Doubling).IsSuccess());

  std::vector<std::size_t> Provision;
  for (std::size_t Index = 0; Index < ModuleCount; ++Index) {
    if (Graph.Loaded[Index])
      Provision.push_back(Index);
  }
  if (Reversed)
    std::reverse(Provision.begin(), Provision.end());
  for (const std::size_t Index : Provision) {
    RC_ASSERT(
        Registry.ProvideModule(Graph.Manifests[Index], ModuleCallback{Index})
            .IsSuccess());
  }
  for (std::size_t Index = 0; Index < ModuleCount; ++Index) {
    if (!Graph.Loaded[Index] ||
        Hooks::ModuleIsLoaded(Owner, ModulePool[Index].Identity))
      continue;
    RC_ASSERT(
        Registry.RegisterModule(Graph.Manifests[Index], ModuleCallback{Index})
            .IsSuccess());
  }
  for (std::size_t Index = 0; Index < ModuleCount; ++Index)
    RC_ASSERT(Hooks::ModuleIsLoaded(Owner, ModulePool[Index].Identity) ==
              Graph.Loaded[Index]);

  const PlanPaths Paths = PlanPathsOf(Case);
  const LifecycleCommitAttempt Request = BuildAttempt(Case, Paths, Reversed);
  if (FaultPool[Case.Fault].Injected)
    Hooks::InjectFault(Owner, FaultPool[Case.Fault].Point);

  const std::size_t SupersededBefore =
      Hooks::SupersededDispatchGenerationCount(Owner);

  const LifecycleCommitObservation Observed =
      Hooks::PublishLifecycleAttempt(Owner, Request);

  const LifecycleStageStatus Stage = ExpectedStageStatus(Case);
  const LifecyclePublishStatus Publish = ExpectedPublishStatus(Case, Stage);
  const bool Published = Publish == LifecyclePublishStatus::Published;
  const bool ImpulseRemoved = PlanRemovesPath(Paths, "Physics.Impulse");
  const bool ImpulseRetained = Contains(Paths.RetainedPaths, "Physics.Impulse");

  RC_ASSERT(Observed.Staging.Status == Stage);
  RC_ASSERT(Observed.Publication.Status == Publish);
  RC_ASSERT(Observed.Publication.IsPublished == Published);
  RC_ASSERT(Observed.SourceBeforeSucceeded);
  RC_ASSERT(Observed.StackDepthAfter == Observed.StackDepthBefore);

  const std::vector<std::string> KindsBefore{"number", "function", "function"};
  RC_ASSERT(Observed.ProbedPathKindsBefore == KindsBefore);
  std::vector<std::string> KindsAfter;
  for (std::size_t Index = 0; Index < Request.ProbedPaths.size(); ++Index) {
    const bool Cleared =
        Published && PlanRemovesPath(Paths, Request.ProbedPaths[Index]);
    KindsAfter.push_back(Cleared ? std::string("absent") : KindsBefore[Index]);
  }
  RC_ASSERT(Observed.ProbedPathKindsAfter == KindsAfter);

  if (Published) {
    RC_ASSERT(Observed.TransactionCommitted);
    RC_ASSERT(!Observed.TransactionPoisoned);
    RC_ASSERT(Observed.Publication.JournalCommitted);
    RC_ASSERT(Observed.Staging.IsPrepared);

    RC_ASSERT(Observed.GenerationAfter == Observed.GenerationBefore + 1);
    RC_ASSERT(Observed.ReflectionGenerationAfter ==
              Observed.ReflectionGenerationBefore + 1);
    RC_ASSERT(Observed.DispatchGenerationAfter ==
              Observed.DispatchGenerationBefore + 1);
    RC_ASSERT(Observed.Publication.PublishedGeneration ==
              Observed.GenerationAfter);
    RC_ASSERT(Observed.Publication.PublishedReflectionGeneration ==
              Observed.ReflectionGenerationAfter);
    RC_ASSERT(Observed.Publication.PublishedDispatchGeneration ==
              Observed.DispatchGenerationAfter);
    RC_ASSERT(Observed.LifecycleGenerationAfter ==
              Observed.LifecycleGenerationBefore);
    RC_ASSERT(Observed.OwnershipRecordsAfter ==
              Observed.OwnershipRecordsBefore);
    RC_ASSERT(Observed.NamespaceOwnershipsAfter ==
              Observed.NamespaceOwnershipsBefore);

    if (Case.Replacing) {
      RC_ASSERT(Observed.ModuleCountAfter == Observed.ModuleCountBefore);
      RC_ASSERT(Observed.ModuleStillLoaded);
      const std::string Expected(ReplacementVersions[Case.ReplacementVersion]);
      RC_ASSERT(Observed.LoadedVersionAfter == Expected);
    } else {
      RC_ASSERT(Observed.ModuleCountAfter + 1 == Observed.ModuleCountBefore);
      RC_ASSERT(!Observed.ModuleStillLoaded);
    }

    for (const std::string &Cleared : Observed.Publication.ClearedPaths)
      RC_ASSERT(PlanRemovesPath(Paths, Cleared));
    for (const std::string &Retained : Paths.RetainedPaths)
      RC_ASSERT(!Contains(Observed.Publication.ClearedPaths, Retained));
    for (const std::string &Path : Request.ProbedPaths) {
      if (PlanRemovesPath(Paths, Path))
        RC_ASSERT(Contains(Observed.Publication.ClearedPaths, Path));
    }

    std::vector<std::string> Unavailable;
    if (ImpulseRemoved)
      Unavailable.push_back("Physics.Impulse");
    RC_ASSERT(Observed.Publication.UnavailableSlots == Unavailable);
    std::vector<std::string> RetainedSlots;
    if (ImpulseRetained)
      RetainedSlots.push_back("Physics.Impulse");
    RC_ASSERT(Observed.Publication.RetainedSlots == RetainedSlots);

    const std::vector<std::string> Caches = CacheText(PlanCaches(Case));
    RC_ASSERT(Observed.Publication.InvalidatedCaches == Caches);
    if (!Caches.empty())
      RC_ASSERT(Observed.Publication.InvalidatedCachesBeforePublication);
    RC_ASSERT(!Observed.Publication.DroppedFrozenCaches);

    RC_ASSERT(Observed.SourceAfterSucceeded == !ImpulseRemoved);
    if (ImpulseRemoved) {
      RC_ASSERT(Observed.SourceAfterDiagnostic.find("Unavailable binding") !=
                std::string::npos);
      RC_ASSERT(Observed.SourceAfterDiagnostic.find("Physics.Impulse") !=
                std::string::npos);
    }

    for (const std::string &Identity : Observed.ReflectionIdentitiesAfter)
      RC_ASSERT(Contains(Observed.ReflectionIdentitiesBefore, Identity));

    const std::vector<std::string> Survivors = SurvivingNames(Graph, Paths);
    for (const std::string &Identity : Observed.ReflectionIdentitiesBefore) {
      const std::string Name = RecordName(Identity);
      if (Contains(Survivors, Name))
        RC_ASSERT(Contains(Observed.ReflectionIdentitiesAfter, Identity));
      if (PlanRemovesPath(Paths, Name))
        RC_ASSERT(!Contains(Observed.ReflectionIdentitiesAfter, Identity));
    }
    if (Paths.RetainedPaths.empty())
      RC_ASSERT(Observed.SymbolCountAfter < Observed.SymbolCountBefore);

    RC_ASSERT(Observed.Publication.SupersededDispatchGeneration ==
              Observed.DispatchGenerationBefore);
    RC_ASSERT(Observed.SupersededDispatchGenerations >= 1);
    RC_ASSERT(Observed.ReclaimedAfterRelease >= 1);
    RC_ASSERT(Observed.LifecycleJournalRetainersAfter == 0);
    if (Case.RetainInvocation) {
      RC_ASSERT(Observed.RetainedGenerationNumber ==
                Observed.DispatchGenerationBefore);
      RC_ASSERT(Observed.RetainedDispatchGenerations >= 1);
      RC_ASSERT(Observed.Publication.PreviousDispatchRetained);
      if (ImpulseRemoved) {
        RC_ASSERT(Observed.RetainedProbe == "Physics.Impulse");
        RC_ASSERT(Observed.RetainedGenerationResolvesOldTarget);
      }
    } else {
      RC_ASSERT(!Observed.Publication.PreviousDispatchRetained);
      RC_ASSERT(Observed.RetainedProbe.empty());
    }
  } else {
    RC_ASSERT(!Observed.Publication.JournalCommitted);
    RC_ASSERT(!Observed.TransactionCommitted);
    RC_ASSERT(Observed.TransactionPoisoned);
    RC_ASSERT(!Observed.Staging.IsPrepared);

    RC_ASSERT(Observed.GenerationAfter == Observed.GenerationBefore);
    RC_ASSERT(Observed.ReflectionGenerationAfter ==
              Observed.ReflectionGenerationBefore);
    RC_ASSERT(Observed.DispatchGenerationAfter ==
              Observed.DispatchGenerationBefore);
    RC_ASSERT(Observed.SymbolCountAfter == Observed.SymbolCountBefore);
    RC_ASSERT(Observed.ModuleCountAfter == Observed.ModuleCountBefore);
    RC_ASSERT(Observed.ModuleStillLoaded);
    const std::string Loaded(ModulePool[PhysicsModule].Version);
    RC_ASSERT(Observed.LoadedVersionAfter == Loaded);
    RC_ASSERT(Observed.ReflectionIdentitiesAfter ==
              Observed.ReflectionIdentitiesBefore);
    RC_ASSERT(Observed.OwnershipRecordsAfter ==
              Observed.OwnershipRecordsBefore);
    RC_ASSERT(Observed.NamespaceOwnershipsAfter ==
              Observed.NamespaceOwnershipsBefore);
    RC_ASSERT(Observed.Publication.ClearedPaths.empty());
    RC_ASSERT(Observed.Publication.UnavailableSlots.empty());
    RC_ASSERT(Observed.Publication.InvalidatedCaches.empty());
    RC_ASSERT(Observed.SourceAfterSucceeded);
    RC_ASSERT(Observed.Publication.SupersededDispatchGeneration == 0);
    RC_ASSERT(!Observed.Publication.PreviousDispatchRetained);
    RC_ASSERT(Observed.SupersededDispatchGenerations == SupersededBefore);
    RC_ASSERT(Observed.LifecycleJournalRetainersAfter == 0);
    RC_ASSERT(!Observed.Publication.Diagnostic.empty() ||
              Observed.Staging.Status != LifecycleStageStatus::Prepared);
    if (Observed.Staging.JournalledEntries > 0) {
      RC_ASSERT(Observed.Staging.RestoredEveryEntry);
      RC_ASSERT(Observed.Staging.RestoredEntryStackDepth);
      RC_ASSERT(Observed.Staging.RestorationOrder.size() ==
                Observed.Staging.JournalledEntries);
    }
    if (!Case.RetainInvocation)
      RC_ASSERT(Observed.RetainedProbe.empty());
    else
      RC_ASSERT(Observed.RetainedGenerationNumber ==
                Observed.DispatchGenerationBefore);
  }

  if (Observed.Staging.Status == LifecycleStageStatus::ValidationFailure)
    RC_ASSERT(Observed.Staging.Staged.empty());

  // Complete staging records exactly one action per declared live value, keeps
  // the previous type generation when the plan removes no type, and derives one
  // successor generation when it does.
  if (Stage == LifecycleStageStatus::Prepared) {
    const std::size_t Policies = Case.PlanUserdataPolicy != 0 ? 1 : 0;
    RC_ASSERT(Observed.Staging.StagedUserdataActions == Policies);
    if (Policies != 0) {
      std::string Action("userdata_action|");
      Action.append(UserdataSubjectText()).append("|");
      Action.append(PolicyMigrates(Case) ? "migrate" : "remain_valid");
      RC_ASSERT(Contains(Observed.Staging.Staged, Action));
    }
    if (Case.PlanRemovesType)
      RC_ASSERT(Observed.Staging.StagedTypeGeneration ==
                Observed.Staging.PreviousTypeGeneration + 1);
    else
      RC_ASSERT(Observed.Staging.StagedTypeGeneration ==
                Observed.Staging.PreviousTypeGeneration);
    if (Published)
      RC_ASSERT(Observed.Publication.PublishedTypeGeneration ==
                Observed.Staging.StagedTypeGeneration);
  }

  // A refusal that never reached the injected stage leaves that fault pending,
  // so it is drained before the State is asked to keep working.
  if (FaultPool[Case.Fault].Injected) {
    const StateFaultPoint Point = FaultPool[Case.Fault].Point;
    while (Hooks::PendingFaults(Owner, Point) > 0)
      static_cast<void>(Hooks::ConsumeFault(Owner, Point));
  }

  RC_ASSERT(Registry.RegisterFunction("Recovered", &Doubling).IsSuccess());
  RC_ASSERT(Owner.Execute("assert(Recovered(4) == 8)").IsSuccess());
  RC_ASSERT(Owner.Execute("assert(Baseline(3) == 6)").IsSuccess());

  const auto FinalDepth = Hooks::ObserveRootStackDepth(Owner);
  RC_ASSERT(FinalDepth.has_value());
  RC_ASSERT(*FinalDepth == *EntryDepth);
  return ObservationDigest(Observed);
}

} // namespace

int RunDynamicModuleLifecycleProperties() {
  // clang-format off
  // Feature: reflection-driven-binding-system, Property 31: Module lifecycle publication follows the retained-generation state machine
  const bool Passed = rc::check(
      // clang-format on
      "Module lifecycle publication follows the retained-generation state "
      "machine",
      [](const std::vector<std::uint8_t> &Shape) {
        ByteCursor Cursor(Shape);
        const Scenario Case = MakeScenario(Cursor);
        const ScenarioModules Graph = BuildModules(Case);

        const std::string Analysis = VerifyAnalysis(Case, Graph, false);
        const std::string Permuted = VerifyAnalysis(Case, Graph, true);
        RC_ASSERT(Analysis == Permuted);

        const LifecycleStageStatus Stage = ExpectedStageStatus(Case);
        RC_TAG(std::string("staging: ")
                   .append(Luna::Detail::LifecycleStageStatusText(Stage)),
               std::string("publication: ")
                   .append(Luna::Detail::LifecyclePublishStatusText(
                       ExpectedPublishStatus(Case, Stage))));

        const std::string Published = VerifyPublication(Case, Graph, false);
        const std::string Reordered = VerifyPublication(Case, Graph, true);
        RC_ASSERT(Published == Reordered);
      });

  return Passed ? 0 : 1;
}