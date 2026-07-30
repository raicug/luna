// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/state/state.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/module/lifecycle.hpp"
#include "state/testing/test_hooks.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::Detail::CompareAffected;
using Luna::Detail::CompareBlocker;
using Luna::Detail::LifecycleAffectedItem;
using Luna::Detail::LifecycleAffectedKind;
using Luna::Detail::LifecycleAnalysis;
using Luna::Detail::LifecycleBlocker;
using Luna::Detail::LifecycleBlockerKind;
using Luna::Detail::LifecycleCacheKind;
using Luna::Detail::LifecycleOperation;
using Luna::Detail::LifecycleRequest;
using Luna::Detail::LifecycleRetainedGeneration;
using Luna::Detail::LifecycleRootedReference;
using Luna::Detail::LifecycleSubject;
using Luna::Detail::LifecycleSymbol;
using Luna::Detail::LifecycleUserdataValue;
using Luna::Detail::OwnershipModel;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "lifecycle closure check failed: " << Description << '\n';
}

[[nodiscard]] Luna::SemanticVersion Version(std::string_view Text) {
  const auto Parsed = Luna::SemanticVersion::TryParse(Text);
  return Parsed ? *Parsed : Luna::SemanticVersion();
}

[[nodiscard]] Luna::ModuleDependency Requires(std::string Identity,
                                              std::string_view Constraint) {
  Luna::ModuleDependency Declared;
  Declared.Identity = std::move(Identity);
  if (const auto Parsed = Luna::VersionConstraint::TryParse(Constraint))
    Declared.Constraints.push_back(*Parsed);
  return Declared;
}

[[nodiscard]] Luna::ModuleManifest
Manifest(std::string Identity, std::string_view VersionText,
         std::vector<Luna::ModuleDependency> Dependencies = {}) {
  auto Created = Luna::ModuleManifest::TryCreate(
      std::move(Identity), Version(VersionText), std::move(Dependencies),
      std::string("Test module."), {});
  return Created ? std::move(*Created) : Luna::ModuleManifest();
}

[[nodiscard]] Luna::TypeId TypeFrom(std::uint8_t Seed) {
  Luna::TypeId::Storage Bytes{};
  Bytes[0] = Seed;
  return Luna::TypeId::FromBytes(Bytes);
}

[[nodiscard]] LifecycleSymbol
Symbol(Luna::SymbolKind Kind, std::string QualifiedName, std::string Signature,
       std::uint8_t TypeSeed, std::string Ownership = std::string()) {
  LifecycleSymbol Declared;
  Declared.Kind = Kind;
  Declared.QualifiedName = std::move(QualifiedName);
  Declared.Signature = std::move(Signature);
  Declared.Type = TypeFrom(TypeSeed);
  Declared.Descriptor =
      Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Int32);
  Declared.OwnershipText = std::move(Ownership);
  Declared.ModuleIdentity = "studio.physics";
  return Declared;
}

[[nodiscard]] std::vector<LifecycleSymbol> PhysicsSymbols() {
  std::vector<LifecycleSymbol> Symbols;
  Symbols.push_back(Symbol(Luna::SymbolKind::Namespace, "Physics", "", 1));
  Symbols.push_back(Symbol(Luna::SymbolKind::OverloadSet, "Physics.Impulse",
                           "(int) -> int", 2));
  Symbols.push_back(Symbol(Luna::SymbolKind::Class, "Physics.Body", "", 3,
                           "lua_owned|default|"));
  Symbols.push_back(Symbol(Luna::SymbolKind::Method, "Physics.Body.Length",
                           "() -> double", 4, "borrowed|default|"));
  return Symbols;
}

[[nodiscard]] LifecycleSubject LoadedSubject() {
  LifecycleSubject Subject;
  Subject.DynamicLifecycleEnabled = true;
  Subject.LoadedModules.push_back(Manifest("studio.physics", "1.2.0"));
  Subject.LoadedModules.push_back(Manifest(
      "studio.gameplay", "2.0.0", {Requires("studio.physics", ">=1.2.0")}));
  Subject.LoadedModules.push_back(
      Manifest("studio.ui", "1.0.0", {Requires("studio.gameplay", ">=2.0.0")}));

  Subject.Symbols = PhysicsSymbols();
  LifecycleSymbol Foreign =
      Symbol(Luna::SymbolKind::OverloadSet, "Gameplay.Spawn", "() -> ()", 5);
  Foreign.ModuleIdentity = "studio.gameplay";
  Subject.Symbols.push_back(std::move(Foreign));

  Subject.DispatchSlots.push_back({1, "Physics.Impulse", true});
  Subject.DispatchSlots.push_back({2, "Physics.Body.Length", true});
  Subject.DispatchSlots.push_back({3, "Gameplay.Spawn", true});

  LifecycleUserdataValue Body;
  Body.ClassQualifiedName = "Physics.Body";
  Body.Type = TypeFrom(3);
  Body.Nonce = 7;
  Body.Ownership = OwnershipModel::LuaOwned;
  Body.IsPublished = true;
  Subject.LiveUserdata.push_back(std::move(Body));

  Subject.Caches.push_back(
      {LifecycleCacheKind::FrozenLookup, "Physics.Impulse"});
  Subject.Caches.push_back(
      {LifecycleCacheKind::FrozenLookup, "Gameplay.Spawn"});
  Subject.Caches.push_back({LifecycleCacheKind::FrozenNamespace, "Physics"});
  Subject.Caches.push_back(
      {LifecycleCacheKind::FrozenModule, "studio.physics"});
  Subject.Caches.push_back(
      {LifecycleCacheKind::LazyMemberValue, "<lazy member values>"});

  LifecycleRootedReference Rooted;
  Rooted.Subject = "Physics.Body#7";
  Rooted.Detail = "a native lifetime handle roots this userdata";
  Subject.RootedReferences.push_back(std::move(Rooted));

  LifecycleRetainedGeneration Current;
  Current.Number = 4;
  Current.IsCurrent = true;
  Current.Invocations = 1;
  Subject.RetainedGenerations.push_back(Current);

  LifecycleRetainedGeneration Superseded;
  Superseded.Number = 3;
  Superseded.LifecycleJournals = 1;
  Subject.RetainedGenerations.push_back(Superseded);
  return Subject;
}

[[nodiscard]] LifecycleRequest Unload(std::string Identity) {
  LifecycleRequest Request;
  Request.Operation = LifecycleOperation::Unload;
  Request.Identity = std::move(Identity);
  return Request;
}

[[nodiscard]] LifecycleRequest
Replace(std::string_view VersionText, std::vector<LifecycleSymbol> Symbols,
        std::vector<Luna::ModuleDependency> Dependencies = {}) {
  LifecycleRequest Request;
  Request.Operation = LifecycleOperation::Replacement;
  Request.Identity = "studio.physics";
  Request.Replacement =
      Manifest("studio.physics", VersionText, std::move(Dependencies));
  Request.ReplacementSymbols = std::move(Symbols);
  return Request;
}

[[nodiscard]] const LifecycleBlocker *Find(const LifecycleAnalysis &Analysis,
                                           LifecycleBlockerKind Kind,
                                           std::string_view Subject) {
  for (const LifecycleBlocker &Blocker : Analysis.Blockers) {
    if (Blocker.Kind == Kind && Blocker.Subject == Subject)
      return &Blocker;
  }
  return nullptr;
}

[[nodiscard]] bool Contains(const std::vector<std::string> &Values,
                            std::string_view Candidate) {
  return std::find(Values.begin(), Values.end(), Candidate) != Values.end();
}

[[nodiscard]] std::optional<std::string>
DetailOf(const LifecycleAnalysis &Analysis, LifecycleAffectedKind Kind,
         std::string_view Subject) {
  for (const LifecycleAffectedItem &Item : Analysis.Affected.All()) {
    if (Item.Kind == Kind && Item.Subject == Subject)
      return Item.Detail;
  }
  return std::nullopt;
}

void CheckCanonicalOrder(const LifecycleAnalysis &Analysis,
                         std::string_view Description) {
  Check(std::is_sorted(
            Analysis.Blockers.begin(), Analysis.Blockers.end(),
            [](const LifecycleBlocker &Left, const LifecycleBlocker &Right) {
              return CompareBlocker(Left, Right) == std::strong_ordering::less;
            }),
        Description);
  Check(std::is_sorted(
            Analysis.Affected.All().begin(), Analysis.Affected.All().end(),
            [](const LifecycleAffectedItem &Left,
               const LifecycleAffectedItem &Right) {
              return CompareAffected(Left, Right) == std::strong_ordering::less;
            }),
        Description);
}
void CheckUnloadClosureAndBlockers() {
  const LifecycleSubject Subject = LoadedSubject();
  const LifecycleAnalysis Analysis =
      Luna::Detail::AnalyzeLifecycleRequest(Unload("studio.physics"), Subject);

  Check(!Analysis.IsPermitted(), "an unload with blockers is refused");
  Check(!Analysis.HasBlocker(LifecycleBlockerKind::UnsupportedDynamicMode) &&
            !Analysis.HasBlocker(LifecycleBlockerKind::UnknownModule),
        "a loaded module of a dynamic State reaches closure analysis");

  const LifecycleAffectedKind Kinds[] = {
      LifecycleAffectedKind::Function,
      LifecycleAffectedKind::Namespace,
      LifecycleAffectedKind::Type,
      LifecycleAffectedKind::Userdata,
      LifecycleAffectedKind::ReflectionRecord,
      LifecycleAffectedKind::Cache,
      LifecycleAffectedKind::Closure,
      LifecycleAffectedKind::DependentModule,
      LifecycleAffectedKind::RootedReference,
      LifecycleAffectedKind::RetainedGeneration};
  for (const LifecycleAffectedKind Kind : Kinds) {
    Check(Analysis.Affected.CountOfKind(Kind) > 0,
          std::string("the closure identifies every affected category: ")
              .append(Luna::Detail::LifecycleAffectedKindText(Kind)));
  }

  Check(Analysis.Affected.Contains(LifecycleAffectedKind::Function,
                                   "Physics.Impulse") &&
            Analysis.Affected.Contains(LifecycleAffectedKind::Namespace,
                                       "Physics") &&
            Analysis.Affected.Contains(LifecycleAffectedKind::Type,
                                       "Physics.Body"),
        "module-owned functions, namespaces, and types join the closure");
  Check(Analysis.Affected.Contains(LifecycleAffectedKind::Userdata,
                                   "Physics.Body#7"),
        "live userdata of an affected class joins the closure");
  Check(Analysis.Affected.Contains(LifecycleAffectedKind::Closure,
                                   "Physics.Impulse") &&
            DetailOf(Analysis, LifecycleAffectedKind::Closure,
                     "Physics.Impulse") == "becomes an unavailable entry",
        "the closure identifies dispatch closures that become unavailable");
  Check(Analysis.Affected.Contains(LifecycleAffectedKind::DependentModule,
                                   "studio.gameplay") &&
            Analysis.Affected.Contains(LifecycleAffectedKind::DependentModule,
                                       "studio.ui"),
        "direct and transitive dependents join the closure");
  Check(Analysis.Affected.Contains(LifecycleAffectedKind::RetainedGeneration,
                                   "generation 4") &&
            Analysis.Affected.Contains(
                LifecycleAffectedKind::RetainedGeneration, "generation 3"),
        "retained dispatch generations join the closure");

  Check(!Analysis.Affected.Contains(LifecycleAffectedKind::Function,
                                    "Gameplay.Spawn") &&
            !Analysis.Affected.Contains(LifecycleAffectedKind::Closure,
                                        "Gameplay.Spawn") &&
            !Analysis.Affected.Contains(LifecycleAffectedKind::Cache,
                                        "Gameplay.Spawn"),
        "symbols and caches owned by other modules stay outside the closure");

  Check(Analysis.RemovedSubjects.size() == 4 &&
            Contains(Analysis.RemovedSubjects, "Physics") &&
            Contains(Analysis.RemovedSubjects, "Physics.Body.Length") &&
            Analysis.RetainedSubjects.empty(),
        "an unload removes every module-owned symbol");
  Check(Contains(Analysis.RemovedTypes, "Physics.Body"),
        "removed class types are identified before mutation");
  Check(Analysis.InvalidatedCaches.size() == 4,
        "every cache of the closure is scheduled for invalidation");

  Check(Analysis.HasBlocker(LifecycleBlockerKind::DependentModule),
        "dependent modules block an unload");
  const LifecycleBlocker *Transitive =
      Find(Analysis, LifecycleBlockerKind::DependentModule, "studio.ui");
  Check(Transitive != nullptr && Transitive->DependencyPath.size() == 3,
        "a dependent blocker names the full dependency path");
  Check(Transitive != nullptr &&
            Transitive->PathText().find(" -> ") != std::string::npos,
        "dependency paths are rendered canonically");
  Check(Find(Analysis, LifecycleBlockerKind::LiveUserdata, "Physics.Body#7") !=
            nullptr,
        "live userdata of a removed class blocks the unload");
  Check(Find(Analysis, LifecycleBlockerKind::RootedReference,
             "Physics.Body#7") != nullptr,
        "a rooted reference to a removed subject blocks the unload");
  Check(Analysis.Message().find("live-userdata") != std::string::npos,
        "the lifecycle message names its blockers");
  CheckCanonicalOrder(Analysis, "unload diagnostics are canonically ordered");
}

void CheckLoadOnlyAndUnknownModule() {
  LifecycleSubject Subject = LoadedSubject();
  Subject.DynamicLifecycleEnabled = false;
  const LifecycleAnalysis Analysis =
      Luna::Detail::AnalyzeLifecycleRequest(Unload("studio.physics"), Subject);
  Check(!Analysis.IsPermitted() &&
            Analysis.HasBlocker(LifecycleBlockerKind::UnsupportedDynamicMode),
        "a State without dynamic lifecycle support refuses an unload");
  Check(Analysis.Blockers.front().Kind ==
            LifecycleBlockerKind::UnsupportedDynamicMode,
        "the unsupported-mode blocker sorts first");

  Subject.DynamicLifecycleEnabled = true;
  const LifecycleAnalysis Unknown =
      Luna::Detail::AnalyzeLifecycleRequest(Unload("studio.audio"), Subject);
  Check(Unknown.HasBlocker(LifecycleBlockerKind::UnknownModule) &&
            Unknown.Affected.IsEmpty() && Unknown.RemovedSubjects.empty(),
        "an unknown module produces a blocker and an empty closure");
}

void CheckCompatibleReplacement() {
  LifecycleSubject Subject = LoadedSubject();
  Subject.LiveUserdata[0].RemainsValid = true;

  std::vector<LifecycleSymbol> Replacement = PhysicsSymbols();
  Replacement.push_back(Symbol(Luna::SymbolKind::OverloadSet, "Physics.Torque",
                               "(int) -> int", 6));
  const LifecycleAnalysis Analysis = Luna::Detail::AnalyzeLifecycleRequest(
      Replace("1.3.0", Replacement), Subject);

  Check(
      Analysis.IsPermitted(),
      "a compatible replacement with a declared userdata policy is permitted");
  Check(Analysis.RemovedSubjects.empty() &&
            Analysis.RetainedSubjects.size() == 4,
        "a compatible replacement retains every module-owned symbol");
  Check(Analysis.Affected.Contains(LifecycleAffectedKind::Function,
                                   "Physics.Torque") &&
            DetailOf(Analysis, LifecycleAffectedKind::Function,
                     "Physics.Torque") == "added",
        "symbols the replacement adds join the closure");
  Check(DetailOf(Analysis, LifecycleAffectedKind::Closure, "Physics.Impulse") ==
            "resolves the published generation",
        "a compatibly retained closure resolves the published generation");
  Check(DetailOf(Analysis, LifecycleAffectedKind::Function,
                 "Physics.Impulse") == "retained compatibly",
        "retained symbols are reported as compatible");
  CheckCanonicalOrder(Analysis,
                      "replacement diagnostics are canonically ordered");

  LifecycleSubject Undeclared = LoadedSubject();
  const LifecycleAnalysis Missing = Luna::Detail::AnalyzeLifecycleRequest(
      Replace("1.3.0", PhysicsSymbols()), Undeclared);
  Check(Find(Missing, LifecycleBlockerKind::UnavailableUserdataMigration,
             "Physics.Body#7") != nullptr,
        "live userdata without a declared policy blocks a replacement");
}

void CheckIncompatibleReplacement() {
  LifecycleSubject Subject = LoadedSubject();

  std::vector<LifecycleSymbol> Replacement;
  Replacement.push_back(Symbol(Luna::SymbolKind::Namespace, "Physics", "", 1));
  Replacement.push_back(Symbol(Luna::SymbolKind::OverloadSet, "Physics.Impulse",
                               "(double) -> int", 9));
  Replacement.push_back(Symbol(Luna::SymbolKind::Class, "Physics.Body", "", 3,
                               "borrowed|default|"));
  Replacement.push_back(Symbol(Luna::SymbolKind::Property,
                               "Physics.Body.Length", "() -> double", 4,
                               "borrowed|default|"));

  const LifecycleAnalysis Analysis = Luna::Detail::AnalyzeLifecycleRequest(
      Replace("1.3.0", Replacement), Subject);

  Check(!Analysis.IsPermitted(), "an incompatible replacement is refused");
  Check(Analysis.HasBlocker(LifecycleBlockerKind::IncompatibleType),
        "a changed canonical type identity is refused");
  Check(
      Analysis.HasBlocker(LifecycleBlockerKind::IncompatibleCallableSignature),
      "a changed callable signature is refused");
  Check(Analysis.HasBlocker(LifecycleBlockerKind::IncompatibleClassOwnership),
        "changed class ownership is refused");
  Check(Analysis.HasBlocker(LifecycleBlockerKind::IncompatibleDeclaration),
        "a changed reflected declaration kind is refused");
  Check(Find(Analysis, LifecycleBlockerKind::UnavailableUserdataMigration,
             "Physics.Body#7") != nullptr,
        "live userdata of an incompatible class requires migration");
  Check(Find(Analysis, LifecycleBlockerKind::RootedReference,
             "Physics.Body#7") != nullptr,
        "a rooted reference to an incompatible class is refused");
  Check(DetailOf(Analysis, LifecycleAffectedKind::Type, "Physics.Body") ==
            "incompatible",
        "the closure reports incompatible subjects");
  CheckCanonicalOrder(Analysis,
                      "incompatible diagnostics are canonically ordered");

  Subject.LiveUserdata[0].MigrationAvailable = true;
  const LifecycleAnalysis Migrating = Luna::Detail::AnalyzeLifecycleRequest(
      Replace("1.3.0", Replacement), Subject);
  Check(
      !Migrating.HasBlocker(LifecycleBlockerKind::UnavailableUserdataMigration),
      "an available migration answers the userdata policy requirement");
  Check(Migrating.HasBlocker(LifecycleBlockerKind::IncompatibleType),
        "migration never excuses an incompatible declaration");

  std::vector<LifecycleSymbol> WithoutClass;
  WithoutClass.push_back(Symbol(Luna::SymbolKind::Namespace, "Physics", "", 1));
  WithoutClass.push_back(Symbol(Luna::SymbolKind::OverloadSet,
                                "Physics.Impulse", "(int) -> int", 2));
  const LifecycleAnalysis Removed = Luna::Detail::AnalyzeLifecycleRequest(
      Replace("1.3.0", WithoutClass), LoadedSubject());
  Check(Find(Removed, LifecycleBlockerKind::LiveUserdata, "Physics.Body#7") !=
            nullptr,
        "a replacement that removes a class with live userdata is refused");
  Check(Contains(Removed.RemovedSubjects, "Physics.Body") &&
            Contains(Removed.RemovedTypes, "Physics.Body") &&
            Contains(Removed.RetainedSubjects, "Physics.Impulse"),
        "a replacement separates removed and retained subjects");
}
void CheckDependencyValidation() {
  const LifecycleSubject Subject = LoadedSubject();

  const LifecycleAnalysis Missing = Luna::Detail::AnalyzeLifecycleRequest(
      Replace("1.3.0", PhysicsSymbols(), {Requires("studio.audio", ">=1.0.0")}),
      Subject);
  const LifecycleBlocker *Absent =
      Find(Missing, LifecycleBlockerKind::MissingDependency, "studio.audio");
  Check(Absent != nullptr && Absent->DependencyPath.size() == 2,
        "a replacement requiring an unloaded module is refused with a path");

  const LifecycleAnalysis Unsatisfied = Luna::Detail::AnalyzeLifecycleRequest(
      Replace("1.3.0", PhysicsSymbols(),
              {Requires("studio.gameplay", ">=3.0.0")}),
      Subject);
  Check(Find(Unsatisfied, LifecycleBlockerKind::UnsatisfiedDependencyConstraint,
             "studio.gameplay") != nullptr,
        "a replacement constraint no loaded version satisfies is refused");

  LifecycleSubject Pinned = LoadedSubject();
  Pinned.LoadedModules[1] = Manifest("studio.gameplay", "2.0.0",
                                     {Requires("studio.physics", "=1.2.0")});
  const LifecycleAnalysis Dependent = Luna::Detail::AnalyzeLifecycleRequest(
      Replace("1.3.0", PhysicsSymbols()), Pinned);
  const LifecycleBlocker *Refused =
      Find(Dependent, LifecycleBlockerKind::UnsatisfiedDependencyConstraint,
           "studio.gameplay");
  Check(Refused != nullptr && !Refused->DependencyPath.empty(),
        "a dependent constraint the replacement version breaks is refused");

  const LifecycleAnalysis Mismatched = Luna::Detail::AnalyzeLifecycleRequest(
      [] {
        LifecycleRequest Request;
        Request.Operation = LifecycleOperation::Replacement;
        Request.Identity = "studio.physics";
        Request.Replacement = Manifest("studio.audio", "1.0.0");
        return Request;
      }(),
      Subject);
  Check(Mismatched.HasBlocker(LifecycleBlockerKind::IdentityMismatch),
        "a replacement manifest for another module is refused");

  const LifecycleAnalysis Invalid = Luna::Detail::AnalyzeLifecycleRequest(
      [] {
        LifecycleRequest Request;
        Request.Operation = LifecycleOperation::Replacement;
        Request.Identity = "studio.physics";
        return Request;
      }(),
      Subject);
  Check(Invalid.HasBlocker(LifecycleBlockerKind::InvalidReplacementManifest),
        "an invalid replacement manifest is refused");
  Check(!Invalid.HasBlocker(LifecycleBlockerKind::MissingDependency),
        "an invalid manifest is never validated for dependencies");
}

void CheckRegistrationOrderIndependence() {
  const LifecycleSubject Subject = LoadedSubject();
  LifecycleSubject Permuted = Subject;
  std::reverse(Permuted.Symbols.begin(), Permuted.Symbols.end());
  std::reverse(Permuted.Caches.begin(), Permuted.Caches.end());
  std::reverse(Permuted.DispatchSlots.begin(), Permuted.DispatchSlots.end());
  std::reverse(Permuted.LoadedModules.begin(), Permuted.LoadedModules.end());
  std::reverse(Permuted.RetainedGenerations.begin(),
               Permuted.RetainedGenerations.end());

  const LifecycleAnalysis First =
      Luna::Detail::AnalyzeLifecycleRequest(Unload("studio.physics"), Subject);
  const LifecycleAnalysis Second =
      Luna::Detail::AnalyzeLifecycleRequest(Unload("studio.physics"), Permuted);
  Check(First.Affected.Text() == Second.Affected.Text(),
        "the closure ignores subject ordering");
  Check(First.BlockerText() == Second.BlockerText(),
        "blocker diagnostics ignore subject ordering");
  Check(First.RemovedSubjects == Second.RemovedSubjects &&
            First.RemovedTypes == Second.RemovedTypes,
        "removed subjects ignore subject ordering");
}

[[nodiscard]] int Impulse(int Magnitude) { return Magnitude * 2; }

struct LoadedState final {
  Luna::State Owner;

  LoadedState() {
    Luna::BindingRegistry Registry = Owner.Bindings();
    const auto Result = Registry.RegisterModule(
        Manifest("studio.physics", "1.2.0"),
        [](Luna::NamespaceBuilder &Builder) {
          Luna::NamespaceBuilder Scope = Builder.RegisterNamespace("Physics");
          static_cast<void>(Scope.RegisterConstant("Gravity", 9));
          static_cast<void>(Scope.RegisterFunction("Impulse", &Impulse));
        });
    Check(Result.IsSuccess(), "the test module loads");
  }
};

void CheckDescribedStateSubject() {
  LoadedState Loaded;
  const LifecycleSubject Subject =
      Hooks::DescribeLifecycleSubject(Loaded.Owner);

  Check(Subject.FindLoaded("studio.physics") != nullptr,
        "the described subject lists the loaded module graph");
  Check(!Subject.DynamicLifecycleEnabled,
        "this milestone describes a load-only State");

  std::size_t Owned = 0;
  bool FoundCallable = false;
  for (const LifecycleSymbol &Declared : Subject.Symbols) {
    if (Declared.ModuleIdentity != "studio.physics")
      continue;
    ++Owned;
    if (Declared.QualifiedName == "Physics.Impulse")
      FoundCallable = true;
  }
  Check(Owned > 0 && FoundCallable,
        "described symbols carry their module provenance and qualified names");

  bool FoundSlot = false;
  for (const auto &Slot : Subject.DispatchSlots) {
    if (Slot.QualifiedName == "Physics.Impulse" && Slot.IsAvailable)
      FoundSlot = true;
  }
  Check(FoundSlot, "described dispatch slots name the installed callables");

  bool FoundCurrent = false;
  for (const LifecycleRetainedGeneration &Held : Subject.RetainedGenerations) {
    if (Held.IsCurrent)
      FoundCurrent = true;
  }
  Check(FoundCurrent, "the current dispatch generation is described");
  Check(Subject.LiveUserdata.empty(),
        "a State without exposed values reports no live userdata");

  const std::size_t Modules = Hooks::LoadedModuleCount(Loaded.Owner);
  const LifecycleAnalysis Analysis =
      Hooks::AnalyzeLifecycleRequest(Loaded.Owner, Unload("studio.physics"));
  Check(!Analysis.IsPermitted() &&
            Analysis.HasBlocker(LifecycleBlockerKind::UnsupportedDynamicMode),
        "a load-only State refuses an unload request");
  Check(Hooks::ModuleIsLoaded(Loaded.Owner, "studio.physics") &&
            Hooks::LoadedModuleCount(Loaded.Owner) == Modules,
        "refused analysis mutates nothing");
  Check(Loaded.Owner.Execute("assert(Physics.Gravity == 9)").IsSuccess(),
        "the module surface stays installed after a refused request");
}

} // namespace

int RunLifecycleClosureTests() {
  FailureCount = 0;
  CheckUnloadClosureAndBlockers();
  CheckLoadOnlyAndUnknownModule();
  CheckCompatibleReplacement();
  CheckIncompatibleReplacement();
  CheckDependencyValidation();
  CheckRegistrationOrderIndependence();
  CheckDescribedStateSubject();
  return FailureCount == 0 ? 0 : 1;
}
