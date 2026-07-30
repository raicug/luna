// clang-format off
#include <luna/luna.hpp>

#include "state/module/lifecycle.hpp"
#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/transaction/lifecycle_publication.hpp"

#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::Detail::LifecycleAffectedKind;
using Luna::Detail::LifecycleAnalysis;
using Luna::Detail::LifecycleBlockerKind;
using Luna::Detail::LifecycleCacheEntry;
using Luna::Detail::LifecycleCacheKind;
using Luna::Detail::LifecycleCommitAttempt;
using Luna::Detail::LifecycleOperation;
using Luna::Detail::LifecyclePublishStatus;
using Luna::Detail::LifecycleRequest;
using Luna::Detail::LifecycleUserdataPolicy;
using Luna::Detail::StateFaultPoint;

int FailureCount = 0;
std::size_t HeavyReads = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "module lifecycle integration check failed: " << Description
            << '\n';
}

[[nodiscard]] int StackDepth(const Luna::State &Owner) {
  const std::optional<int> Depth = Hooks::ObserveRootStackDepth(Owner);
  return Depth ? *Depth : -1;
}

[[nodiscard]] bool Mentions(std::string_view Text, std::string_view Wanted) {
  return Text.find(Wanted) != std::string_view::npos;
}

[[nodiscard]] bool Contains(const std::vector<std::string> &Values,
                            std::string_view Wanted) {
  for (const std::string &Value : Values) {
    if (Value == Wanted)
      return true;
  }
  return false;
}

[[nodiscard]] Luna::ModuleManifest Manifest(std::string_view VersionText) {
  auto Created = Luna::ModuleManifest::TryCreate(
      std::string("studio.physics"),
      Luna::SemanticVersion::TryParse(VersionText)
          .value_or(Luna::SemanticVersion()),
      {}, std::string("Rigid body physics."), {});
  return Created ? std::move(*Created) : Luna::ModuleManifest();
}

struct Body final {
  int Mass = 4;

  [[nodiscard]] int Heavy() const {
    ++HeavyReads;
    return Mass * 3;
  }
};

[[nodiscard]] int Impulse(int Magnitude) { return Magnitude * 2; }

void ConfigureSurface(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Physics = Builder.RegisterNamespace("Physics");
  static_cast<void>(Physics.RegisterConstant("Gravity", 9));
  static_cast<void>(Physics.RegisterFunction("Impulse", &Impulse));
  Luna::NamespaceBuilder Solver = Physics.RegisterNamespace("Solver");
  static_cast<void>(Solver.RegisterConstant("Iterations", 4));
}

void ConfigureSurfaceWithClass(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Physics = Builder.RegisterNamespace("Physics");
  static_cast<void>(Physics.RegisterConstant("Gravity", 9));
  static_cast<void>(Physics.RegisterFunction("Impulse", &Impulse));
  Luna::ClassBuilder<Body> Class = Physics.RegisterClass<Body>(
      "Body", Luna::StableTypeKey("studio.physics.Body"));
  static_cast<void>(
      Class.Constructor<>()
          .Field("Mass", &Body::Mass)
          .Property("Heavy", Luna::PropertyPolicy::Lazy(), &Body::Heavy));
}

[[nodiscard]] std::string
Declarations(const Luna::ReflectionSnapshot &Snapshot) {
  const Luna::GeneratedArtifact Artifact =
      Luna::GenerateDeclarations(Snapshot, Luna::DeclarationOptions());
  return Artifact.IsComplete() ? Artifact.Bytes() : std::string();
}

[[nodiscard]] std::string IdentityOf(const Luna::ReflectionSnapshot &Snapshot,
                                     std::string_view QualifiedName) {
  const Luna::ReflectionRecord Record = Snapshot.Find(QualifiedName);
  return Record.IsValid() ? Record.Id().ToString() : std::string();
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "module lifecycle source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

void CheckReplacementRepublishesTheModuleThroughTheMachine() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);
  Check(
      Registry.RegisterModule(Manifest("1.2.0"), &ConfigureSurface).IsSuccess(),
      "the representative module loads through the real machine");
  Check(Succeeds(Owner, "assert(Physics.Gravity == 9)\n"
                        "assert(Physics.Impulse(4) == 8)\n"
                        "assert(Physics.Solver.Iterations == 4)"),
        "the whole module surface runs before the replacement");

  const Luna::ReflectionSnapshot Retained = Registry.Reflection();
  const std::string RetainedArtifact = Declarations(Retained);
  const std::string ImpulseIdentity = IdentityOf(Retained, "Physics.Impulse");
  Check(!RetainedArtifact.empty() && !ImpulseIdentity.empty(),
        "the generated declarations and canonical identities exist before");

  LifecycleCommitAttempt Request;
  Request.Staged.Plan.Operation = LifecycleOperation::Replacement;
  Request.Staged.Plan.Identity = "studio.physics";
  Request.Staged.Plan.DynamicLifecycleEnabled = true;
  Request.Staged.Plan.Replacement = Manifest("1.3.0");
  Request.Staged.Plan.RemovedSubjects = {"Physics.Gravity"};
  Request.Staged.Plan.RetainedPaths = {"Physics.Impulse",
                                       "Physics.Solver.Iterations"};
  Request.Staged.RunCallback = true;
  Request.ProbedPaths = {"Physics.Gravity", "Physics.Impulse",
                         "Physics.Solver.Iterations"};
  Request.SourceBeforePublication = "StaleImpulse = Physics.Impulse";
  Request.SourceAfterPublication =
      "if StaleImpulse(21) ~= 42 then error('the retained call changed') end";

  const auto Observed = Hooks::PublishLifecycleAttempt(Owner, Request);
  Check(Observed.Publication.Status == LifecyclePublishStatus::Published,
        "the compatible replacement publishes through the real machine");
  Check(Observed.ModuleStillLoaded && Observed.LoadedVersionAfter == "1.3.0",
        "the replacement version is the loaded one afterward");
  Check(Observed.ProbedPathKindsBefore ==
            std::vector<std::string>{"number", "function", "number"},
        "every probed path holds its published value before the replacement");
  Check(Observed.ProbedPathKindsAfter ==
            std::vector<std::string>{"absent", "function", "number"},
        "only the removed path leaves the virtual machine");
  Check(Observed.SourceBeforeSucceeded && Observed.SourceAfterSucceeded,
        "a closure retained by script resolves the new generation");
  Check(Observed.GenerationAfter == Observed.GenerationBefore + 1 &&
            Observed.ReflectionGenerationAfter ==
                Observed.ReflectionGenerationBefore + 1 &&
            Observed.DispatchGenerationAfter ==
                Observed.DispatchGenerationBefore + 1,
        "exactly one module, reflection, and dispatch generation is published");
  Check(Contains(Observed.Publication.RetainedSlots, "Physics.Impulse") &&
            Observed.Publication.UnavailableSlots.empty(),
        "a compatibly retained callable keeps its permanent slot");
  Check(Contains(Observed.Publication.ClearedPaths, "Physics.Gravity"),
        "publication clears exactly the removed table path");
  Check(Observed.StackDepthAfter == Observed.StackDepthBefore &&
            StackDepth(Owner) == EntryDepth,
        "the replacement restores the root stack exactly");

  Check(Retained.Find("Physics.Gravity").IsValid() &&
            Declarations(Retained) == RetainedArtifact,
        "a snapshot retained before the replacement never changes");
  const Luna::ReflectionSnapshot Published = Registry.Reflection();
  Check(!Published.Find("Physics.Gravity").IsValid(),
        "the published reflection generation drops the removed record");
  Check(IdentityOf(Published, "Physics.Impulse") == ImpulseIdentity,
        "a compatibly retained symbol keeps its canonical identity");
  const std::string PublishedArtifact = Declarations(Published);
  Check(!PublishedArtifact.empty() && PublishedArtifact != RetainedArtifact &&
            !Mentions(PublishedArtifact, "Gravity"),
        "generated artifacts follow the published generation exactly");

  Check(Registry.RegisterFunction("Later", &Impulse).IsSuccess() &&
            Succeeds(Owner, "assert(Later(3) == 6)"),
        "the State keeps registering and executing after the replacement");
  Check(StackDepth(Owner) == EntryDepth,
        "the recovered State stays exactly balanced");
}

void CheckUnloadRemovesTheWholeSurfaceThroughTheMachine() {
  HeavyReads = 0;
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);
  Check(Registry.RegisterModule(Manifest("1.2.0"), &ConfigureSurfaceWithClass)
            .IsSuccess(),
        "a module of functions, namespaces, and classes loads");
  Check(Succeeds(Owner, "Value = Physics.Body.New()\n"
                        "assert(Value.Mass == 4)\n"
                        "assert(Value.Heavy == 12)\n"
                        "assert(Value.Heavy == 12)"),
        "a script constructs one userdata and reads its lazy value");
  Check(HeavyReads == 1 && Hooks::LiveLazyMemberCacheEntryCount(Owner) > 0,
        "the lazy value is cached for the published generation");

  const std::size_t IdentitiesBefore = Hooks::CachedIdentityCount(Owner);
  const LifecycleAnalysis Described = Hooks::AnalyzeLifecycleRequest(Owner, [] {
    LifecycleRequest Wanted;
    Wanted.Operation = LifecycleOperation::Unload;
    Wanted.Identity = "studio.physics";
    return Wanted;
  }());
  Check(!Described.IsPermitted() &&
            Described.HasBlocker(LifecycleBlockerKind::UnsupportedDynamicMode),
        "a load-only State refuses the unload of a real module");
  Check(Described.Affected.CountOfKind(LifecycleAffectedKind::Function) > 0 &&
            Described.Affected.CountOfKind(LifecycleAffectedKind::Namespace) >
                0 &&
            Described.Affected.CountOfKind(LifecycleAffectedKind::Type) > 0 &&
            Described.Affected.CountOfKind(
                LifecycleAffectedKind::ReflectionRecord) > 0 &&
            Described.Affected.CountOfKind(LifecycleAffectedKind::Closure) > 0,
        "the closure of a real State names every affected category");
  Check(Hooks::ModuleIsLoaded(Owner, "studio.physics") &&
            Succeeds(Owner, "assert(Physics.Impulse(2) == 4)"),
        "the refused request mutated nothing at all");

  LifecycleCommitAttempt Request;
  Request.Staged.Plan.Operation = LifecycleOperation::Unload;
  Request.Staged.Plan.Identity = "studio.physics";
  Request.Staged.Plan.DynamicLifecycleEnabled = true;
  Request.Staged.Plan.RemovedSubjects = {"Physics"};
  LifecycleUserdataPolicy Continuing;
  Continuing.Subject = "Physics.Body#1";
  Continuing.ClassQualifiedName = "Physics.Body";
  Continuing.RemainsValid = true;
  Request.Staged.Plan.LiveUserdata.push_back(std::move(Continuing));
  LifecycleCacheEntry Lazy;
  Lazy.Kind = LifecycleCacheKind::LazyMemberValue;
  Lazy.Subject = "<lazy member values>";
  Request.Staged.Plan.InvalidatedCaches.push_back(std::move(Lazy));
  LifecycleCacheEntry Identity;
  Identity.Kind = LifecycleCacheKind::NativeIdentity;
  Identity.Subject = "Physics.Body#1";
  Request.Staged.Plan.InvalidatedCaches.push_back(std::move(Identity));
  Request.ProbedPaths = {"Physics.Impulse", "Physics.Body"};
  Request.SourceBeforePublication = "StaleImpulse = Physics.Impulse";
  Request.SourceAfterPublication = "StaleImpulse(2)";

  const auto Observed = Hooks::PublishLifecycleAttempt(Owner, Request);
  Check(Observed.Publication.Status == LifecyclePublishStatus::Published,
        "the permitted unload publishes through the real machine");
  Check(Observed.ProbedPathKindsAfter ==
            std::vector<std::string>{"absent", "absent"},
        "every unloaded table path leaves the virtual machine");
  Check(!Observed.ModuleStillLoaded &&
            Observed.ModuleCountAfter + 1 == Observed.ModuleCountBefore,
        "the unloaded module leaves the published module graph");
  Check(Observed.SourceBeforeSucceeded && !Observed.SourceAfterSucceeded &&
            Mentions(Observed.SourceAfterDiagnostic, "Unavailable binding") &&
            Mentions(Observed.SourceAfterDiagnostic, "Physics.Impulse"),
        "a stale closure for a removed symbol fails deterministically");
  Check(Observed.Publication.DroppedLazyEntries > 0 &&
            Hooks::LazyMemberCacheEntryCount(Owner) == 0,
        "the lazy value cache of the closure is invalidated");
  Check(Observed.Publication.DroppedIdentityEntries +
                Observed.Publication.RetainedIdentityEntries ==
            IdentitiesBefore,
        "every cached native identity is either dropped or retained");
  Check(Observed.Publication.InvalidatedCachesBeforePublication,
        "no cache survives into the generation it cannot answer for");
  Check(Succeeds(Owner, "assert(type(Value) == 'userdata')"),
        "a value the plan declared as continuing stays a live userdata");
  Check(Observed.StackDepthAfter == Observed.StackDepthBefore &&
            StackDepth(Owner) == EntryDepth,
        "the unload restores the root stack exactly");
  Check(Registry.RegisterFunction("Later", &Impulse).IsSuccess() &&
            Succeeds(Owner, "assert(Later(3) == 6)"),
        "the State keeps registering and executing after the unload");
}

void CheckFailedLifecycleLeavesTheMachineUntouched() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);
  Check(Registry.RegisterModule(Manifest("1.2.0"), &ConfigureSurfaceWithClass)
            .IsSuccess(),
        "the module loads before the failing attempt");
  Check(Succeeds(Owner, "Value = Physics.Body.New()\n"
                        "assert(Value.Heavy == 12)"),
        "a script exposes one userdata with a cached lazy value");

  const std::string ArtifactBefore = Declarations(Registry.Reflection());
  const std::size_t LazyBefore = Hooks::LazyMemberCacheEntryCount(Owner);
  const std::size_t IdentitiesBefore = Hooks::CachedIdentityCount(Owner);

  Luna::NamespaceBuilder Pending = Registry.RegisterNamespace("Pending");
  static_cast<void>(Pending.RegisterConstant("Ready", 1));

  LifecycleCommitAttempt Request;
  Request.Staged.Plan.Operation = LifecycleOperation::Unload;
  Request.Staged.Plan.Identity = "studio.physics";
  Request.Staged.Plan.DynamicLifecycleEnabled = true;
  Request.Staged.Plan.RemovedSubjects = {"Physics"};
  LifecycleCacheEntry Lazy;
  Lazy.Kind = LifecycleCacheKind::LazyMemberValue;
  Lazy.Subject = "<lazy member values>";
  Request.Staged.Plan.InvalidatedCaches.push_back(std::move(Lazy));
  Request.ProbedPaths = {"Physics.Impulse", "Physics.Body"};

  Hooks::InjectFault(Owner, StateFaultPoint::LifecycleGenerationPublication);
  const auto Observed = Hooks::PublishLifecycleAttempt(Owner, Request);
  Check(Observed.Publication.Status ==
            LifecyclePublishStatus::GenerationFailure,
        "the injected publication failure is reported deterministically");
  Check(!Observed.Publication.IsPublished &&
            Observed.GenerationAfter == Observed.GenerationBefore &&
            Observed.ReflectionGenerationAfter ==
                Observed.ReflectionGenerationBefore &&
            Observed.DispatchGenerationAfter ==
                Observed.DispatchGenerationBefore,
        "the previous complete generation stays active");
  Check(Observed.ProbedPathKindsAfter == Observed.ProbedPathKindsBefore &&
            Observed.ModuleStillLoaded,
        "no table path and no module changed");
  Check(Observed.Staging.RestoredEveryEntry &&
            Observed.Staging.RestoredEntryStackDepth &&
            Observed.StackDepthAfter == Observed.StackDepthBefore,
        "the journal restores every effect and the exact stack depth");
  Check(Hooks::LazyMemberCacheEntryCount(Owner) == LazyBefore &&
            Hooks::CachedIdentityCount(Owner) == IdentitiesBefore,
        "a failed attempt invalidates no cache whatsoever");
  Check(Declarations(Registry.Reflection()) == ArtifactBefore,
        "generated artifacts are byte-identical after a failed attempt");
  Check(Succeeds(Owner, "assert(Physics.Impulse(4) == 8)\n"
                        "assert(Value.Mass == 4)\n"
                        "assert(Physics.Body.New().Heavy == 12)"),
        "the whole module surface still runs after the failed attempt");

  Check(Pending.Commit().IsSuccess() &&
            Succeeds(Owner, "assert(Pending.Ready == 1)"),
        "a scope captured before a failed lifecycle attempt still publishes");

  Luna::NamespaceBuilder Stale = Registry.RegisterNamespace("Stale");
  static_cast<void>(Stale.RegisterConstant("Value", 2));
  Check(Hooks::AdvanceLifecycleGeneration(Owner),
        "the lifecycle generation the scope belongs to is replaced");
  const Luna::RegistrationResult Refused = Stale.Commit();
  Check(!Refused.IsSuccess(), "a scope of a replaced generation is refused");
  Check(Refused.Diagnostic() != nullptr &&
            Mentions(Refused.Diagnostic()->Message(),
                     "replaced lifecycle generation"),
        "the refusal names the replaced lifecycle generation");
  Check(Succeeds(Owner, "assert(Stale == nil)"),
        "a refused scope publishes nothing into the virtual machine");
  Check(Registry.RegisterFunction("Fresh", &Impulse).IsSuccess() &&
            Succeeds(Owner, "assert(Fresh(5) == 10)"),
        "the State keeps working after every refusal");
  Check(StackDepth(Owner) == EntryDepth,
        "every refusal leaves the root stack exactly balanced");
}

} // namespace

int RunModuleLifecycleIntegrationTests() {
  FailureCount = 0;
  CheckReplacementRepublishesTheModuleThroughTheMachine();
  CheckUnloadRemovesTheWholeSurfaceThroughTheMachine();
  CheckFailedLifecycleLeavesTheMachineUntouched();
  return FailureCount == 0 ? 0 : 1;
}
