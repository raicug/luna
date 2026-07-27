// clang-format off
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/module/lifecycle.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Luna::Detail::CompareBlocker;
using Luna::Detail::LifecycleAffectedKind;
using Luna::Detail::LifecycleAnalysis;
using Luna::Detail::LifecycleBlocker;
using Luna::Detail::LifecycleBlockerKind;
using Luna::Detail::LifecycleBlockerKindText;
using Luna::Detail::LifecycleCacheEntry;
using Luna::Detail::LifecycleCacheKind;
using Luna::Detail::LifecycleOperation;
using Luna::Detail::LifecycleRequest;
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
  std::cerr << "lifecycle matrix check failed: " << Description << '\n';
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
      std::string("Matrix module."), {});
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

[[nodiscard]] std::vector<LifecycleSymbol> WithoutClassSymbols() {
  std::vector<LifecycleSymbol> Symbols;
  Symbols.push_back(Symbol(Luna::SymbolKind::Namespace, "Physics", "", 1));
  Symbols.push_back(Symbol(Luna::SymbolKind::OverloadSet, "Physics.Impulse",
                           "(int) -> int", 2));
  return Symbols;
}

[[nodiscard]] LifecycleUserdataValue LiveBody(bool MigrationAvailable,
                                              bool RemainsValid) {
  LifecycleUserdataValue Body;
  Body.ClassQualifiedName = "Physics.Body";
  Body.Type = TypeFrom(3);
  Body.Nonce = 7;
  Body.Ownership = OwnershipModel::LuaOwned;
  Body.IsPublished = true;
  Body.MigrationAvailable = MigrationAvailable;
  Body.RemainsValid = RemainsValid;
  return Body;
}

[[nodiscard]] LifecycleSubject LoadedSubject() {
  LifecycleSubject Subject;
  Subject.DynamicLifecycleEnabled = true;
  Subject.LoadedModules.push_back(Manifest("studio.physics", "1.2.0"));
  Subject.LoadedModules.push_back(Manifest(
      "studio.gameplay", "2.0.0", {Requires("studio.physics", ">=1.2.0")}));
  Subject.LoadedModules.push_back(
      Manifest("studio.ui", "1.0.0", {Requires("studio.gameplay", ">=2.0.0")}));
  Subject.LoadedModules.push_back(Manifest(
      "studio.tools", "3.1.0", {Requires("studio.physics", ">=1.0.0")}));

  Subject.Symbols = PhysicsSymbols();
  Subject.DispatchSlots.push_back({1, "Physics.Impulse", true});
  Subject.DispatchSlots.push_back({2, "Physics.Body.Length", true});
  return Subject;
}

[[nodiscard]] LifecycleRequest Unload(std::string Identity) {
  LifecycleRequest Request;
  Request.Operation = LifecycleOperation::Unload;
  Request.Identity = std::move(Identity);
  return Request;
}

[[nodiscard]] LifecycleRequest Replace(Luna::ModuleManifest Replacement,
                                       std::vector<LifecycleSymbol> Symbols) {
  LifecycleRequest Request;
  Request.Operation = LifecycleOperation::Replacement;
  Request.Identity = "studio.physics";
  Request.Replacement = std::move(Replacement);
  Request.ReplacementSymbols = std::move(Symbols);
  return Request;
}

[[nodiscard]] const LifecycleBlocker *Find(const LifecycleAnalysis &Analysis,
                                           LifecycleBlockerKind Kind) {
  for (const LifecycleBlocker &Blocker : Analysis.Blockers) {
    if (Blocker.Kind == Kind)
      return &Blocker;
  }
  return nullptr;
}

[[nodiscard]] bool Mentions(std::string_view Text, std::string_view Wanted) {
  return Text.find(Wanted) != std::string_view::npos;
}

[[nodiscard]] bool IsCanonicallyOrdered(const LifecycleAnalysis &Analysis) {
  return std::is_sorted(
      Analysis.Blockers.begin(), Analysis.Blockers.end(),
      [](const LifecycleBlocker &Left, const LifecycleBlocker &Right) {
        return CompareBlocker(Left, Right) == std::strong_ordering::less;
      });
}

struct BlockerCase final {
  LifecycleBlockerKind Kind = LifecycleBlockerKind::UnsupportedDynamicMode;
  LifecycleSubject Subject;
  LifecycleRequest Request;
};

[[nodiscard]] std::vector<BlockerCase> EveryBlockerCase() {
  std::vector<BlockerCase> Cases;

  {
    LifecycleSubject Subject = LoadedSubject();
    Subject.DynamicLifecycleEnabled = false;
    Cases.push_back({LifecycleBlockerKind::UnsupportedDynamicMode,
                     std::move(Subject), Unload("studio.physics")});
  }
  Cases.push_back({LifecycleBlockerKind::UnknownModule, LoadedSubject(),
                   Unload("studio.audio")});
  {
    LifecycleRequest Request;
    Request.Operation = LifecycleOperation::Replacement;
    Request.Identity = "studio.physics";
    Cases.push_back({LifecycleBlockerKind::InvalidReplacementManifest,
                     LoadedSubject(), std::move(Request)});
  }
  Cases.push_back({LifecycleBlockerKind::IdentityMismatch, LoadedSubject(),
                   Replace(Manifest("studio.audio", "1.0.0"), {})});
  Cases.push_back({LifecycleBlockerKind::MissingDependency, LoadedSubject(),
                   Replace(Manifest("studio.physics", "1.3.0",
                                    {Requires("studio.audio", ">=1.0.0")}),
                           PhysicsSymbols())});
  Cases.push_back({LifecycleBlockerKind::UnsatisfiedDependencyConstraint,
                   LoadedSubject(),
                   Replace(Manifest("studio.physics", "1.3.0",
                                    {Requires("studio.gameplay", ">=3.0.0")}),
                           PhysicsSymbols())});
  Cases.push_back({LifecycleBlockerKind::DependentModule, LoadedSubject(),
                   Unload("studio.physics")});
  {
    std::vector<LifecycleSymbol> Symbols = PhysicsSymbols();
    Symbols[3] = Symbol(Luna::SymbolKind::Property, "Physics.Body.Length",
                        "() -> double", 4, "borrowed|default|");
    Cases.push_back(
        {LifecycleBlockerKind::IncompatibleDeclaration, LoadedSubject(),
         Replace(Manifest("studio.physics", "1.3.0"), std::move(Symbols))});
  }
  {
    std::vector<LifecycleSymbol> Symbols = PhysicsSymbols();
    Symbols[2] = Symbol(Luna::SymbolKind::Class, "Physics.Body", "", 9,
                        "lua_owned|default|");
    Cases.push_back(
        {LifecycleBlockerKind::IncompatibleType, LoadedSubject(),
         Replace(Manifest("studio.physics", "1.3.0"), std::move(Symbols))});
  }
  {
    std::vector<LifecycleSymbol> Symbols = PhysicsSymbols();
    Symbols[1] = Symbol(Luna::SymbolKind::OverloadSet, "Physics.Impulse",
                        "(double) -> int", 2);
    Cases.push_back(
        {LifecycleBlockerKind::IncompatibleCallableSignature, LoadedSubject(),
         Replace(Manifest("studio.physics", "1.3.0"), std::move(Symbols))});
  }
  {
    std::vector<LifecycleSymbol> Symbols = PhysicsSymbols();
    Symbols[2] = Symbol(Luna::SymbolKind::Class, "Physics.Body", "", 3,
                        "borrowed|default|");
    Cases.push_back(
        {LifecycleBlockerKind::IncompatibleClassOwnership, LoadedSubject(),
         Replace(Manifest("studio.physics", "1.3.0"), std::move(Symbols))});
  }
  {
    LifecycleSubject Subject = LoadedSubject();
    Subject.LiveUserdata.push_back(LiveBody(true, true));
    Cases.push_back(
        {LifecycleBlockerKind::LiveUserdata, std::move(Subject),
         Replace(Manifest("studio.physics", "1.3.0"), WithoutClassSymbols())});
  }
  {
    LifecycleSubject Subject = LoadedSubject();
    Subject.LiveUserdata.push_back(LiveBody(false, false));
    Cases.push_back(
        {LifecycleBlockerKind::UnavailableUserdataMigration, std::move(Subject),
         Replace(Manifest("studio.physics", "1.3.0"), PhysicsSymbols())});
  }
  {
    LifecycleSubject Subject = LoadedSubject();
    Subject.LiveUserdata.push_back(LiveBody(true, true));
    LifecycleRootedReference Rooted;
    Rooted.Subject = "Physics.Body#7";
    Rooted.Detail = "a native lifetime handle roots this userdata";
    Subject.RootedReferences.push_back(std::move(Rooted));
    Cases.push_back({LifecycleBlockerKind::RootedReference, std::move(Subject),
                     Unload("studio.physics")});
  }
  return Cases;
}

void CheckEveryBlockerCategoryIsRefusedCanonically() {
  const std::vector<BlockerCase> Cases = EveryBlockerCase();
  Check(Cases.size() == 14,
        "the matrix covers one case for every blocker category");

  for (const BlockerCase &Injected : Cases) {
    const std::string Named(LifecycleBlockerKindText(Injected.Kind));
    const LifecycleAnalysis Analysis = Luna::Detail::AnalyzeLifecycleRequest(
        Injected.Request, Injected.Subject);
    Check(!Analysis.IsPermitted() && Analysis.HasBlocker(Injected.Kind),
          "the matrix refuses the request it targets: " + Named);
    const LifecycleBlocker *Blocker = Find(Analysis, Injected.Kind);
    Check(Blocker != nullptr && !Blocker->Subject.empty() &&
              !Blocker->Detail.empty(),
          "every blocker names one subject and one reason: " + Named);
    Check(Blocker != nullptr && Mentions(Blocker->Text(), Named) &&
              Mentions(Blocker->Text(), Blocker->Subject),
          "every blocker renders its canonical category and subject: " + Named);
    Check(Blocker != nullptr && Mentions(Blocker->Message(), Named),
          "every blocker message names its canonical category: " + Named);
    Check(Mentions(Analysis.Message(), Named),
          "the analysis message names every blocker it found: " + Named);
    Check(IsCanonicallyOrdered(Analysis),
          "blockers stay canonically ordered: " + Named);
    Check(Analysis.BlockerText().size() == Analysis.Blockers.size(),
          "every blocker is rendered once: " + Named);
  }
}

void CheckDependentPathsNameEveryDependencyEdge() {
  const LifecycleSubject Subject = LoadedSubject();
  const LifecycleAnalysis Analysis =
      Luna::Detail::AnalyzeLifecycleRequest(Unload("studio.physics"), Subject);

  struct Expected final {
    std::string_view Identity;
    std::size_t Length;
  };
  const Expected Dependents[] = {
      {"studio.gameplay", 2}, {"studio.tools", 2}, {"studio.ui", 3}};

  std::size_t Found = 0;
  for (const Expected &Dependent : Dependents) {
    const LifecycleBlocker *Blocker = nullptr;
    for (const LifecycleBlocker &Candidate : Analysis.Blockers) {
      if (Candidate.Kind == LifecycleBlockerKind::DependentModule &&
          Candidate.Subject == Dependent.Identity)
        Blocker = &Candidate;
    }
    const std::string Named(Dependent.Identity);
    Check(Blocker != nullptr, "every dependent blocks the unload: " + Named);
    if (Blocker == nullptr)
      continue;
    ++Found;
    Check(Blocker->DependencyPath.size() == Dependent.Length,
          "each dependency path names every edge it traversed: " + Named);
    Check(Mentions(Blocker->DependencyPath.front(), Dependent.Identity),
          "each dependency path starts at its dependent: " + Named);
    Check(Mentions(Blocker->DependencyPath.back(), "studio.physics"),
          "each dependency path ends at the unloaded module: " + Named);
    Check(Mentions(Blocker->PathText(), " -> ") &&
              Mentions(Blocker->Text(), Blocker->PathText()),
          "each rendered blocker carries its whole path: " + Named);
    Check(Analysis.Affected.Contains(LifecycleAffectedKind::DependentModule,
                                     Dependent.Identity),
          "each dependent joins the closure as well: " + Named);
  }
  Check(Found == 3, "direct and transitive dependents are all reported");
}

void CheckUserdataContinuationPolicyMatrix() {
  struct Policy final {
    bool Migration = false;
    bool RemainsValid = false;
    std::string_view Description;
  };
  const Policy Policies[] = {{false, false, "neither policy"},
                             {true, false, "an available migration"},
                             {false, true, "declared continued validity"},
                             {true, true, "both policies"}};

  for (const Policy &Declared : Policies) {
    const std::string Named(Declared.Description);

    LifecycleSubject Compatible = LoadedSubject();
    Compatible.LiveUserdata.push_back(
        LiveBody(Declared.Migration, Declared.RemainsValid));
    const LifecycleAnalysis Retained = Luna::Detail::AnalyzeLifecycleRequest(
        Replace(Manifest("studio.physics", "1.3.0"), PhysicsSymbols()),
        Compatible);
    const bool Permitted = Declared.Migration || Declared.RemainsValid;
    Check(Retained.IsPermitted() == Permitted,
          "a compatible replacement needs one explicit policy: " + Named);
    Check(Retained.HasBlocker(
              LifecycleBlockerKind::UnavailableUserdataMigration) != Permitted,
          "an undeclared policy is the only refusal reason: " + Named);
    Check(Retained.Affected.Contains(LifecycleAffectedKind::Userdata,
                                     "Physics.Body#7"),
          "the live value joins the closure whatever it declares: " + Named);

    std::vector<LifecycleSymbol> Incompatible = PhysicsSymbols();
    Incompatible[2] = Symbol(Luna::SymbolKind::Class, "Physics.Body", "", 9,
                             "lua_owned|default|");
    const LifecycleAnalysis Changed = Luna::Detail::AnalyzeLifecycleRequest(
        Replace(Manifest("studio.physics", "1.3.0"), std::move(Incompatible)),
        Compatible);
    Check(Changed.HasBlocker(
              LifecycleBlockerKind::UnavailableUserdataMigration) !=
              Declared.Migration,
          "an incompatible class accepts only a migration: " + Named);
    Check(!Changed.IsPermitted(),
          "an incompatible class is refused whatever the policy is: " + Named);

    const LifecycleAnalysis Removed = Luna::Detail::AnalyzeLifecycleRequest(
        Replace(Manifest("studio.physics", "1.3.0"), WithoutClassSymbols()),
        Compatible);
    Check(Removed.HasBlocker(LifecycleBlockerKind::LiveUserdata) &&
              !Removed.HasBlocker(
                  LifecycleBlockerKind::UnavailableUserdataMigration),
          "removing a class with live userdata is refused outright: " + Named);

    const LifecycleAnalysis Unloaded = Luna::Detail::AnalyzeLifecycleRequest(
        Unload("studio.physics"), Compatible);
    Check(Unloaded.HasBlocker(LifecycleBlockerKind::LiveUserdata),
          "an unload never continues a live value: " + Named);
  }
}

void CheckAffectedCachesFollowTheClosure() {
  LifecycleSubject Subject = LoadedSubject();
  Subject.Caches.push_back(
      {LifecycleCacheKind::FrozenLookup, "Physics.Impulse"});
  Subject.Caches.push_back({LifecycleCacheKind::FrozenLookup, "Ui.Present"});
  Subject.Caches.push_back(
      {LifecycleCacheKind::LazyMemberValue, "<lazy member values>"});
  Subject.Caches.push_back(
      {LifecycleCacheKind::NativeIdentity, "Physics.Body#7"});

  const LifecycleAnalysis Without =
      Luna::Detail::AnalyzeLifecycleRequest(Unload("studio.physics"), Subject);
  Check(Without.InvalidatedCaches.size() == 2 &&
            Without.Affected.Contains(LifecycleAffectedKind::Cache,
                                      "Physics.Impulse") &&
            Without.Affected.Contains(LifecycleAffectedKind::Cache,
                                      "Physics.Body#7"),
        "caches named after the closure's own subjects are invalidated");
  Check(!Without.Affected.Contains(LifecycleAffectedKind::Cache,
                                   "<lazy member values>"),
        "a lazy cache no affected value populated stays untouched");

  Subject.LiveUserdata.push_back(LiveBody(true, true));
  const LifecycleAnalysis Affected =
      Luna::Detail::AnalyzeLifecycleRequest(Unload("studio.physics"), Subject);
  Check(Affected.InvalidatedCaches.size() == 3,
        "an affected live value adds its lazy and identity caches");
  Check(Affected.Affected.Contains(LifecycleAffectedKind::Cache,
                                   "<lazy member values>") &&
            Affected.Affected.Contains(LifecycleAffectedKind::Cache,
                                       "Physics.Body#7"),
        "lazy values and native identities are invalidated by the closure");
  for (const LifecycleCacheEntry &Entry : Affected.InvalidatedCaches) {
    Check(Entry.Subject != "Ui.Present",
          "no cache of another module is ever invalidated");
  }
  Check(std::is_sorted(Affected.InvalidatedCaches.begin(),
                       Affected.InvalidatedCaches.end(),
                       [](const LifecycleCacheEntry &Left,
                          const LifecycleCacheEntry &Right) {
                         if (Left.Kind != Right.Kind)
                           return static_cast<std::uint8_t>(Left.Kind) <
                                  static_cast<std::uint8_t>(Right.Kind);
                         return Left.Subject < Right.Subject;
                       }),
        "the invalidation set is canonically ordered");
}

} // namespace

int RunLifecycleBlockerMatrixTests() {
  FailureCount = 0;
  CheckEveryBlockerCategoryIsRefusedCanonically();
  CheckDependentPathsNameEveryDependencyEdge();
  CheckUserdataContinuationPolicyMatrix();
  CheckAffectedCachesFollowTheClosure();
  return FailureCount == 0 ? 0 : 1;
}
