// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/state/state.hpp>

#include "state/module/lifecycle.hpp"
#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/transaction/lifecycle_publication.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::Detail::LifecycleCacheEntry;
using Luna::Detail::LifecycleCacheKind;
using Luna::Detail::LifecycleCommitAttempt;
using Luna::Detail::LifecycleCommitObservation;
using Luna::Detail::LifecycleOperation;
using Luna::Detail::LifecyclePublishStatus;
using Luna::Detail::LifecycleStageStatus;
using Luna::Detail::StateFaultPoint;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "lifecycle publication check failed: " << Description << '\n';
}

[[nodiscard]] Luna::SemanticVersion Version(std::string_view Text) {
  const auto Parsed = Luna::SemanticVersion::TryParse(Text);
  return Parsed ? *Parsed : Luna::SemanticVersion();
}

[[nodiscard]] Luna::ModuleManifest Manifest(std::string Identity,
                                            std::string_view VersionText) {
  std::vector<Luna::ModuleExport> Exports;
  Luna::ModuleExport Namespace;
  Namespace.Kind = Luna::SymbolKind::Namespace;
  Namespace.Name = "Physics";
  Exports.push_back(std::move(Namespace));

  auto Created = Luna::ModuleManifest::TryCreate(
      std::move(Identity), Version(VersionText), {},
      std::string("Rigid body physics."), std::move(Exports));
  return Created ? std::move(*Created) : Luna::ModuleManifest();
}

[[nodiscard]] int Impulse(int Magnitude) { return Magnitude * 2; }

struct LoadedState final {
  Luna::State Owner;
  Luna::ModuleManifest Loaded = Manifest("studio.physics", "1.2.0");

  LoadedState() {
    Luna::BindingRegistry Registry = Owner.Bindings();
    const auto Result =
        Registry.RegisterModule(Loaded, [](Luna::NamespaceBuilder &Builder) {
          Luna::NamespaceBuilder Scope = Builder.RegisterNamespace("Physics");
          static_cast<void>(Scope.RegisterConstant("Gravity", 9));
          static_cast<void>(Scope.RegisterFunction("Impulse", &Impulse));
        });
    Check(Result.IsSuccess(), "the test module loads");
  }
};

[[nodiscard]] LifecycleCommitAttempt UnloadRequest() {
  LifecycleCommitAttempt Request;
  Request.Staged.Plan.Operation = LifecycleOperation::Unload;
  Request.Staged.Plan.Identity = "studio.physics";
  Request.Staged.Plan.DynamicLifecycleEnabled = true;
  Request.Staged.Plan.RemovedSubjects = {"Physics"};
  Request.ProbedPaths = {"Physics.Gravity", "Physics.Impulse"};
  return Request;
}

[[nodiscard]] LifecycleCommitAttempt ReplacementRequest() {
  LifecycleCommitAttempt Request;
  Request.Staged.Plan.Operation = LifecycleOperation::Replacement;
  Request.Staged.Plan.Identity = "studio.physics";
  Request.Staged.Plan.DynamicLifecycleEnabled = true;
  Request.Staged.Plan.Replacement = Manifest("studio.physics", "1.3.0");
  Request.Staged.Plan.RemovedSubjects = {"Physics.Gravity"};
  Request.Staged.Plan.RetainedPaths = {"Physics.Impulse"};
  Request.Staged.RunCallback = true;
  Request.ProbedPaths = {"Physics.Gravity", "Physics.Impulse"};
  return Request;
}

[[nodiscard]] bool Contains(const std::vector<std::string> &Values,
                            std::string_view Wanted) {
  return std::find(Values.begin(), Values.end(), Wanted) != Values.end();
}

[[nodiscard]] bool Mentions(const std::string &Text, std::string_view Wanted) {
  return Text.find(Wanted) != std::string::npos;
}

void CheckNothingWasPublished(const LifecycleCommitObservation &Observed,
                              std::string_view Description) {
  Check(!Observed.Publication.IsPublished &&
            !Observed.Publication.JournalCommitted,
        Description);
  Check(Observed.GenerationAfter == Observed.GenerationBefore, Description);
  Check(Observed.ReflectionGenerationAfter ==
            Observed.ReflectionGenerationBefore,
        Description);
  Check(Observed.DispatchGenerationAfter == Observed.DispatchGenerationBefore,
        Description);
  Check(Observed.ModuleCountAfter == Observed.ModuleCountBefore, Description);
  Check(Observed.SymbolCountAfter == Observed.SymbolCountBefore, Description);
  Check(Observed.ModuleStillLoaded, Description);
  Check(Observed.ProbedPathKindsAfter == Observed.ProbedPathKindsBefore,
        Description);
  Check(Observed.ReflectionIdentitiesAfter ==
            Observed.ReflectionIdentitiesBefore,
        Description);
  Check(Observed.StackDepthAfter == Observed.StackDepthBefore, Description);
  Check(!Observed.TransactionCommitted && Observed.TransactionPoisoned,
        Description);
  Check(Observed.LifecycleJournalRetainersAfter == 0, Description);
}

void CheckLoadOnlyStateRefusesPublication() {
  LoadedState Fixture;
  LifecycleCommitAttempt Request = UnloadRequest();
  Request.PublishWithoutDynamicLifecycle = true;

  const auto Observed = Hooks::PublishLifecycleAttempt(Fixture.Owner, Request);
  Check(Observed.Publication.Status ==
            LifecyclePublishStatus::UnsupportedDynamicMode,
        "a State without dynamic lifecycle refuses publication");
  Check(Mentions(Observed.Publication.Diagnostic, "load-only") &&
            Mentions(Observed.Publication.Diagnostic, "studio.physics"),
        "the load-only refusal is deterministic and names the module");
  CheckNothingWasPublished(Observed,
                           "a load-only refusal publishes nothing at all");
}

void CheckUnstagedPublicationIsRefused() {
  LoadedState Fixture;
  LifecycleCommitAttempt Request = UnloadRequest();
  Request.PublishWithoutStaging = true;

  const auto Observed = Hooks::PublishLifecycleAttempt(Fixture.Owner, Request);
  Check(Observed.Publication.Status == LifecyclePublishStatus::NothingStaged,
        "publication without a complete staged generation is refused");
  CheckNothingWasPublished(Observed,
                           "a refused publication leaves everything active");
}

void CheckInjectedPublicationFaultKeepsEveryGeneration() {
  LoadedState Fixture;
  Hooks::InjectFault(Fixture.Owner,
                     StateFaultPoint::LifecycleGenerationPublication);

  const auto Observed =
      Hooks::PublishLifecycleAttempt(Fixture.Owner, UnloadRequest());
  Check(Observed.Staging.Status == LifecycleStageStatus::Prepared,
        "the attempt stages completely before publication fails");
  Check(Observed.Publication.Status ==
            LifecyclePublishStatus::GenerationFailure,
        "an injected publication failure is reported deterministically");
  Check(Observed.Staging.IsRolledBack && Observed.Staging.RestoredEveryEntry &&
            Observed.Staging.RestoredEntryStackDepth,
        "a failed publication restores every journalled effect");
  CheckNothingWasPublished(
      Observed, "a failed publication keeps the previous generations active");
}

void CheckUnloadPublishesOneNewGeneration() {
  LoadedState Fixture;
  LifecycleCommitAttempt Request = UnloadRequest();
  Request.SourceBeforePublication = "StaleImpulse = Physics.Impulse";
  Request.SourceAfterPublication = "local Result = StaleImpulse(2)";

  const auto Observed = Hooks::PublishLifecycleAttempt(Fixture.Owner, Request);
  Check(Observed.SourceBeforeSucceeded,
        "a script may retain a closure before the unload");
  Check(Observed.Publication.Status == LifecyclePublishStatus::Published &&
            Observed.Publication.IsPublished,
        "a permitted unload publishes one new generation");
  Check(Observed.GenerationAfter == Observed.GenerationBefore + 1 &&
            Observed.ReflectionGenerationAfter ==
                Observed.ReflectionGenerationBefore + 1 &&
            Observed.DispatchGenerationAfter ==
                Observed.DispatchGenerationBefore + 1,
        "module, reflection, and dispatch generations advance exactly once");
  Check(Observed.Publication.PublishedGeneration == Observed.GenerationAfter &&
            Observed.Publication.PublishedDispatchGeneration ==
                Observed.DispatchGenerationAfter,
        "the publication reports the generations it published");
  Check(Observed.ModuleCountAfter + 1 == Observed.ModuleCountBefore &&
            !Observed.ModuleStillLoaded,
        "the unloaded module leaves the published module graph");
  Check(Observed.SymbolCountAfter < Observed.SymbolCountBefore,
        "the published symbol table drops the unloaded closure");
  Check(Observed.ProbedPathKindsBefore ==
            std::vector<std::string>{"number", "function"},
        "the module's values are published before the unload");
  Check(Observed.ProbedPathKindsAfter ==
            std::vector<std::string>{"absent", "absent"},
        "an unloaded table path resolves to nothing afterward");
  Check(Contains(Observed.Publication.ClearedPaths, "Physics.Impulse") &&
            Contains(Observed.Publication.ClearedPaths, "Physics.Gravity"),
        "publication clears every removed table path");
  Check(Contains(Observed.Publication.UnavailableSlots, "Physics.Impulse"),
        "the removed callable keeps an immutable unavailable entry");
  Check(!Observed.SourceAfterSucceeded &&
            Mentions(Observed.SourceAfterDiagnostic, "Unavailable binding") &&
            Mentions(Observed.SourceAfterDiagnostic, "Physics.Impulse"),
        "a stale closure for a removed symbol fails deterministically");
  Check(Observed.TransactionCommitted && !Observed.TransactionPoisoned &&
            Observed.Publication.JournalCommitted,
        "the outermost transaction commits exactly one lifecycle publication");
  Check(Observed.OwnershipRecordsAfter == Observed.OwnershipRecordsBefore &&
            Observed.NamespaceOwnershipsAfter ==
                Observed.NamespaceOwnershipsBefore &&
            Observed.LifecycleGenerationAfter ==
                Observed.LifecycleGenerationBefore,
        "unaffected ownership stays exactly as it was");
  Check(Observed.LifecycleJournalRetainersAfter == 0,
        "the lifecycle journal releases the previous dispatch generation");
  Check(Observed.StackDepthAfter == Observed.StackDepthBefore,
        "publication restores the root stack depth exactly");

  for (const std::string &Identity : Observed.ReflectionIdentitiesAfter) {
    Check(Contains(Observed.ReflectionIdentitiesBefore, Identity),
          "every retained reflection record keeps its canonical identity");
    Check(!Mentions(Identity, "Physics"),
          "no record of the unloaded module survives publication");
  }
  Check(Observed.ReflectionIdentitiesAfter.size() <
            Observed.ReflectionIdentitiesBefore.size(),
        "the published reflection generation drops the unloaded records");

  Luna::BindingRegistry Registry = Fixture.Owner.Bindings();
  Check(Registry.RegisterFunction("Later", &Impulse).IsSuccess() &&
            Fixture.Owner.Execute("assert(Later(3) == 6)").IsSuccess(),
        "the State keeps registering and executing after the unload");
}

void CheckReplacementKeepsRetainedClosuresCallable() {
  LoadedState Fixture;
  LifecycleCommitAttempt Request = ReplacementRequest();
  Request.SourceBeforePublication = "StaleImpulse = Physics.Impulse";
  Request.SourceAfterPublication =
      "local Result = StaleImpulse(21)\n"
      "if Result ~= 42 then error('the retained closure changed') end";

  const auto Observed = Hooks::PublishLifecycleAttempt(Fixture.Owner, Request);
  Check(Observed.Publication.Status == LifecyclePublishStatus::Published,
        "a compatible replacement publishes one new generation");
  Check(Observed.ModuleCountAfter == Observed.ModuleCountBefore &&
            Observed.ModuleStillLoaded &&
            Observed.LoadedVersionAfter == "1.3.0",
        "the replacement version becomes the loaded one");
  Check(Contains(Observed.Publication.RetainedSlots, "Physics.Impulse") &&
            Observed.Publication.UnavailableSlots.empty(),
        "a compatibly retained callable keeps its permanent dispatch slot");
  Check(Observed.SourceAfterSucceeded,
        "a stale closure for a retained symbol resolves the new generation");
  Check(Observed.ProbedPathKindsAfter ==
            std::vector<std::string>{"absent", "function"},
        "only the removed path leaves the virtual machine");
  Check(Observed.DispatchGenerationAfter ==
            Observed.DispatchGenerationBefore + 1,
        "one new dispatch generation serves calls beginning afterward");

  const std::string Retained("Physics.Impulse=");
  const auto Before =
      std::find_if(Observed.ReflectionIdentitiesBefore.begin(),
                   Observed.ReflectionIdentitiesBefore.end(),
                   [&Retained](const std::string &Identity) {
                     return Identity.compare(0, Retained.size(), Retained) == 0;
                   });
  Check(Before != Observed.ReflectionIdentitiesBefore.end() &&
            Contains(Observed.ReflectionIdentitiesAfter, *Before),
        "a compatibly retained symbol keeps its canonical reflection identity");
}

void CheckInFlightGenerationOutlivesPublication() {
  LoadedState Fixture;
  LifecycleCommitAttempt Request = UnloadRequest();
  Request.RetainInvocationGeneration = true;

  const auto Observed = Hooks::PublishLifecycleAttempt(Fixture.Owner, Request);
  Check(Observed.Publication.Status == LifecyclePublishStatus::Published,
        "the unload publishes while an invocation retains the old generation");
  Check(Observed.RetainedProbe == "Physics.Impulse" &&
            Observed.RetainedGenerationNumber ==
                Observed.DispatchGenerationBefore,
        "a retained invocation keeps the generation it began with");
  Check(Observed.RetainedGenerationResolvesOldTarget,
        "an invocation begun before publication is never retargeted");
  Check(Observed.RetainedDispatchGenerations == 1 &&
            Observed.Publication.PreviousDispatchRetained,
        "the previous dispatch generation stays alive while it is retained");
  Check(Observed.ReclaimedAfterRelease == 1,
        "the old generation is reclaimed only once nothing retains it");
  Check(Observed.LifecycleJournalRetainersAfter == 0,
        "publication releases the lifecycle journal claim");
}

void CheckAffectedCachesAreInvalidatedBeforePublication() {
  LoadedState Fixture;
  Luna::BindingRegistry Registry = Fixture.Owner.Bindings();
  Check(Registry.Freeze().IsSuccess(), "the fixture freezes its caches");
  Check(Hooks::ObserveFreezeCache(Fixture.Owner).Published,
        "the frozen cache is published before the unload");

  LifecycleCommitAttempt Request = UnloadRequest();
  LifecycleCacheEntry Lookup;
  Lookup.Kind = LifecycleCacheKind::FrozenLookup;
  Lookup.Subject = "Physics.Impulse";
  Request.Staged.Plan.InvalidatedCaches.push_back(std::move(Lookup));

  LifecycleCacheEntry Lazy;
  Lazy.Kind = LifecycleCacheKind::LazyMemberValue;
  Lazy.Subject = "Physics.Body";
  Request.Staged.Plan.InvalidatedCaches.push_back(std::move(Lazy));

  const auto Observed = Hooks::PublishLifecycleAttempt(Fixture.Owner, Request);
  Check(Observed.Publication.Status == LifecyclePublishStatus::Published,
        "the unload publishes after the affected caches are invalidated");
  Check(Contains(Observed.Publication.InvalidatedCaches,
                 "frozen_lookup|Physics.Impulse") &&
            Contains(Observed.Publication.InvalidatedCaches,
                     "lazy_member_value|Physics.Body"),
        "every named cache entry is invalidated in canonical order");
  Check(Observed.Publication.DroppedFrozenCaches &&
            !Hooks::ObserveFreezeCache(Fixture.Owner).Published,
        "an affected frozen cache generation is dropped");
  Check(Observed.Publication.InvalidatedCachesBeforePublication,
        "no cache survives into the replacement it cannot answer for");
}

} // namespace

int RunLifecyclePublicationTests() {
  FailureCount = 0;
  CheckLoadOnlyStateRefusesPublication();
  CheckUnstagedPublicationIsRefused();
  CheckInjectedPublicationFaultKeepsEveryGeneration();
  CheckUnloadPublishesOneNewGeneration();
  CheckReplacementKeepsRetainedClosuresCallable();
  CheckInFlightGenerationOutlivesPublication();
  CheckAffectedCachesAreInvalidatedBeforePublication();
  return FailureCount == 0 ? 0 : 1;
}