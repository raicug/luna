// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/state/state.hpp>

#include "state/module/lifecycle.hpp"
#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/transaction/lifecycle_staging.hpp"

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
using Luna::Detail::LifecycleAttempt;
using Luna::Detail::LifecycleAttemptObservation;
using Luna::Detail::LifecycleBlocker;
using Luna::Detail::LifecycleBlockerKind;
using Luna::Detail::LifecycleCacheEntry;
using Luna::Detail::LifecycleCacheKind;
using Luna::Detail::LifecycleOperation;
using Luna::Detail::LifecycleStageStatus;
using Luna::Detail::LifecycleUserdataPolicy;
using Luna::Detail::StateFaultPoint;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "lifecycle staging check failed: " << Description << '\n';
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

[[nodiscard]] LifecycleAttempt UnloadAttempt() {
  LifecycleAttempt Attempt;
  Attempt.Plan.Operation = LifecycleOperation::Unload;
  Attempt.Plan.Identity = "studio.physics";
  Attempt.Plan.DynamicLifecycleEnabled = true;
  Attempt.Plan.RemovedSubjects = {"Physics"};
  return Attempt;
}

[[nodiscard]] bool Staged(const LifecycleAttemptObservation &Observed,
                          std::string_view Text) {
  return std::find(Observed.Staging.Staged.begin(),
                   Observed.Staging.Staged.end(),
                   Text) != Observed.Staging.Staged.end();
}

[[nodiscard]] std::size_t
StagedOfKind(const LifecycleAttemptObservation &Observed,
             std::string_view Prefix) {
  std::size_t Count = 0;
  for (const std::string &Line : Observed.Staging.Staged) {
    if (Line.compare(0, Prefix.size(), Prefix) == 0)
      ++Count;
  }
  return Count;
}

void CheckPreviousGenerationSurvived(
    const LifecycleAttemptObservation &Observed, std::string_view Description) {
  Check(Observed.GenerationAfter == Observed.Staging.PreviousGeneration,
        Description);
  Check(Observed.ReflectionGenerationAfter ==
            Observed.Staging.PreviousReflectionGeneration,
        Description);
  Check(Observed.DispatchGenerationAfter ==
            Observed.Staging.PreviousDispatchGeneration,
        Description);
  Check(Observed.ModuleCountAfter == Observed.Staging.PreviousModuleCount,
        Description);
  Check(Observed.SymbolCountAfter == Observed.Staging.PreviousSymbolCount,
        Description);
  Check(Observed.ModuleStillLoaded, Description);
  Check(Observed.PathKindsAfter == Observed.PathKindsWhileStaged, Description);
  Check(Observed.StackDepthAfter == Observed.Staging.EntryStackDepth,
        Description);
  Check(Observed.SupersededDispatchGenerations == 0, Description);
  Check(Observed.LifecycleJournalRetainersAfter == 0, Description);
}

void CheckLoadOnlyStateRefusesWithoutMutation() {
  LoadedState Fixture;
  LifecycleAttempt Attempt = UnloadAttempt();
  Attempt.Plan.DynamicLifecycleEnabled = false;

  const auto Observed = Hooks::PrepareLifecycleAttempt(Fixture.Owner, Attempt);
  Check(Observed.Staging.Status == LifecycleStageStatus::ValidationFailure,
        "a load-only State refuses the request");
  Check(Observed.Staging.Diagnostic.find("load-only") != std::string::npos,
        "the load-only refusal is deterministic and names the mode");
  Check(Observed.Staging.Diagnostic.find("studio.physics") != std::string::npos,
        "the refusal names the module");
  Check(!Observed.Staging.CompletedStaging && Observed.Staging.Staged.empty() &&
            Observed.Staging.JournalledEntries == 0,
        "a refused request stages nothing at all");
  Check(Observed.TransactionPoisoned,
        "the outer transaction keeps the first deterministic diagnostic");
  CheckPreviousGenerationSurvived(
      Observed, "a load-only refusal mutates nothing whatsoever");
}

void CheckOneBlockerRefusesBeforeAnythingIsStaged() {
  LoadedState Fixture;
  LifecycleAttempt Attempt = UnloadAttempt();
  LifecycleBlocker Live;
  Live.Kind = LifecycleBlockerKind::LiveUserdata;
  Live.Subject = "Physics.Body#7";
  Live.Detail = "one live value cannot be invalidated safely";
  Attempt.Blockers.push_back(std::move(Live));

  const auto Observed = Hooks::PrepareLifecycleAttempt(Fixture.Owner, Attempt);
  Check(Observed.Staging.Status == LifecycleStageStatus::ValidationFailure,
        "a single blocker refuses the whole operation");
  Check(Observed.Staging.Diagnostic.find("live-userdata") != std::string::npos,
        "the blocker diagnostic is reported in canonical order");
  Check(Observed.Staging.Staged.empty() &&
            Observed.Staging.JournalledEntries == 0,
        "a blocked request stages nothing");
  CheckPreviousGenerationSurvived(Observed,
                                  "a blocked request mutates nothing");
}

void CheckUnloadStagesEveryCategoryUnpublished() {
  LoadedState Fixture;
  LifecycleAttempt Attempt = UnloadAttempt();
  LifecycleUserdataPolicy Migrating;
  Migrating.Subject = "Physics.Body#1";
  Migrating.ClassQualifiedName = "Physics.Body";
  Migrating.MigrationAvailable = true;
  Attempt.Plan.LiveUserdata.push_back(std::move(Migrating));

  LifecycleUserdataPolicy Continuing;
  Continuing.Subject = "Physics.Body#2";
  Continuing.ClassQualifiedName = "Physics.Body";
  Continuing.RemainsValid = true;
  Attempt.Plan.LiveUserdata.push_back(std::move(Continuing));

  LifecycleCacheEntry Lookup;
  Lookup.Kind = LifecycleCacheKind::FrozenLookup;
  Lookup.Subject = "Physics.Impulse";
  Attempt.Plan.InvalidatedCaches.push_back(std::move(Lookup));

  LifecycleCacheEntry Identity;
  Identity.Kind = LifecycleCacheKind::NativeIdentity;
  Identity.Subject = "Physics.Body#1";
  Attempt.Plan.InvalidatedCaches.push_back(std::move(Identity));

  const auto Observed = Hooks::PrepareLifecycleAttempt(Fixture.Owner, Attempt);
  Check(Observed.Staging.Status == LifecycleStageStatus::Prepared &&
            Observed.Staging.CompletedStaging,
        "the unload stages completely");

  Check(Staged(Observed, "module_graph|studio.physics@1.2.0|removed"),
        "the module graph stages the removed module");
  Check(Observed.Staging.StagedModuleCount + 1 ==
            Observed.Staging.PreviousModuleCount,
        "the staged module graph drops exactly the unloaded module");
  Check(Staged(Observed, "table_path|Physics.Impulse|removed") &&
            Staged(Observed, "table_path|Physics.Gravity|removed"),
        "every canonical table path of the closure is staged");
  Check(Staged(Observed, "userdata_action|Physics.Body#1|migrate") &&
            Staged(Observed, "userdata_action|Physics.Body#2|remain_valid"),
        "each live value stages its own explicit action");
  Check(Observed.Staging.StagedUserdataActions == 2,
        "no live value is left without an action");
  Check(Staged(Observed, "reflection|Physics.Impulse|removed") &&
            Staged(Observed, "reflection|Physics|removed"),
        "the reflection records of the closure are staged");
  Check(Staged(Observed, "cache|Physics.Impulse|frozen_lookup") &&
            Staged(Observed, "cache|Physics.Body#1|native_identity"),
        "the cache invalidation set is staged");
  Check(Observed.Staging.StagedCacheEntries == 2,
        "every named cache entry is staged");
  Check(StagedOfKind(Observed, "unavailable_slot|") == 1 &&
            Staged(Observed, "unavailable_slot|Physics.Impulse|slot 1"),
        "the removed callable path stages an unavailable slot");
  Check(Observed.Staging.StagedUnavailableSlots == 1 &&
            Observed.Staging.StagedDispatchTargets == 0,
        "an unload retains no dispatch target");

  Check(Observed.Staging.StagedGeneration ==
            Observed.Staging.PreviousGeneration + 1,
        "the candidate generation set is one successor");
  Check(Observed.Staging.StagedReflectionGeneration ==
            Observed.Staging.PreviousReflectionGeneration + 1,
        "the staged reflection generation is one successor");
  Check(Observed.Staging.StagedDispatchGeneration ==
            Observed.Staging.PreviousDispatchGeneration + 1,
        "the staged dispatch generation is one successor");
  Check(Observed.Staging.StagedTypeGeneration ==
            Observed.Staging.PreviousTypeGeneration,
        "a request that removes no type keeps the type generation");
  Check(Observed.Staging.StagedSymbolCount <
            Observed.Staging.PreviousSymbolCount,
        "the staged symbol table drops the closure's symbols");

  Check(Observed.GenerationWhileStaged == Observed.Staging.PreviousGeneration &&
            Observed.ReflectionGenerationWhileStaged ==
                Observed.Staging.PreviousReflectionGeneration &&
            Observed.DispatchGenerationWhileStaged ==
                Observed.Staging.PreviousDispatchGeneration &&
            Observed.ModuleCountWhileStaged ==
                Observed.Staging.PreviousModuleCount,
        "an ordinary query never observes pending lifecycle data");
  Check(Observed.StackDepthWhileStaged == Observed.Staging.EntryStackDepth,
        "staging leaves the root stack at its entry depth");
  Check(std::find(Observed.PathKindsWhileStaged.begin(),
                  Observed.PathKindsWhileStaged.end(),
                  std::string("absent")) == Observed.PathKindsWhileStaged.end(),
        "every staged path still holds its published value");

  Check(Observed.Staging.IsRolledBack && Observed.Staging.RestoredEveryEntry &&
            Observed.Staging.RestoredEntryStackDepth,
        "the attempt restores every journalled effect");
  Check(Observed.Staging.RestorationOrder.size() ==
            Observed.Staging.JournalledEntries,
        "restoration visits every journalled entry");
  Check(Observed.Staging.JournalledPaths ==
                StagedOfKind(Observed, "table_path|") &&
            Observed.Staging.JournalledOverlays +
                    Observed.Staging.JournalledPaths ==
                Observed.Staging.JournalledEntries,
        "the journal records every staged path and every staged overlay");
  Check(Observed.Staging.JournalledPaths ==
            Observed.Staging.PriorValueKinds.size(),
        "every journalled path recorded its exact prior value");
  CheckPreviousGenerationSurvived(
      Observed, "a rolled-back unload leaves the previous generation active");
}

void CheckStagingRetainsThePreviousDispatchGeneration() {
  LoadedState Fixture;
  const auto Observed =
      Hooks::PrepareLifecycleAttempt(Fixture.Owner, UnloadAttempt());
  Check(Observed.Staging.LifecycleJournalRetainers == 1,
        "the lifecycle journal claims the previous dispatch generation");
  Check(Observed.LifecycleJournalRetainersAfter == 0,
        "the claim is given back once no undo can remain");
  Check(Observed.Staging.PreviousDispatchRetained,
        "the previous dispatch generation stays readable while staged");
}

void CheckReplacementStagesRetainedDispatchTargets() {
  LoadedState Fixture;
  LifecycleAttempt Attempt;
  Attempt.Plan.Operation = LifecycleOperation::Replacement;
  Attempt.Plan.Identity = "studio.physics";
  Attempt.Plan.DynamicLifecycleEnabled = true;
  Attempt.Plan.Replacement = Manifest("studio.physics", "1.3.0");
  Attempt.Plan.RemovedSubjects = {"Physics.Gravity"};
  Attempt.Plan.RetainedPaths = {"Physics.Impulse"};
  Attempt.RunCallback = true;

  const auto Observed = Hooks::PrepareLifecycleAttempt(Fixture.Owner, Attempt);
  Check(Observed.Staging.Status == LifecycleStageStatus::Prepared,
        "a compatible replacement stages completely");
  Check(Staged(Observed, "module_graph|studio.physics@1.3.0|installed"),
        "the staged module graph installs the replacement");
  Check(Observed.Staging.StagedModuleCount ==
            Observed.Staging.PreviousModuleCount,
        "a replacement keeps the module count");
  Check(Staged(Observed, "dispatch_target|Physics.Impulse|slot 1") &&
            Observed.Staging.StagedDispatchTargets == 1,
        "a retained path keeps its permanent dispatch slot");
  Check(Observed.Staging.StagedUnavailableSlots == 0,
        "a retained path is never made unavailable");
  Check(Staged(Observed, "table_path|Physics.Impulse|retained") &&
            Staged(Observed, "table_path|Physics.Gravity|removed"),
        "the replacement stages retained and removed paths separately");
  CheckPreviousGenerationSurvived(
      Observed,
      "a rolled-back replacement leaves the previous generation active");
}

void CheckReplacementRejectsAForeignManifest() {
  LoadedState Fixture;
  LifecycleAttempt Attempt;
  Attempt.Plan.Operation = LifecycleOperation::Replacement;
  Attempt.Plan.Identity = "studio.physics";
  Attempt.Plan.DynamicLifecycleEnabled = true;
  Attempt.Plan.Replacement = Manifest("studio.render", "2.0.0");

  const auto Observed = Hooks::PrepareLifecycleAttempt(Fixture.Owner, Attempt);
  Check(Observed.Staging.Status == LifecycleStageStatus::ValidationFailure,
        "a replacement manifest for another module is refused");
  Check(Observed.Staging.Staged.empty(), "the refusal stages nothing");
  CheckPreviousGenerationSurvived(Observed,
                                  "a refused replacement mutates nothing");
}

void CheckUnknownModuleIsRefused() {
  LoadedState Fixture;
  LifecycleAttempt Attempt = UnloadAttempt();
  Attempt.Plan.Identity = "studio.absent";

  const auto Observed = Hooks::PrepareLifecycleAttempt(Fixture.Owner, Attempt);
  Check(Observed.Staging.Status == LifecycleStageStatus::ValidationFailure,
        "an unloaded module identity is refused");
  Check(Observed.Staging.Diagnostic.find("not loaded") != std::string::npos,
        "the refusal explains that the module is not loaded");
  Check(Observed.ModuleCountAfter == Observed.Staging.PreviousModuleCount,
        "nothing is removed from the module graph");
}

void CheckCallbackFailureAndExceptionRestoreEverything() {
  {
    LoadedState Fixture;
    LifecycleAttempt Attempt = UnloadAttempt();
    Attempt.Plan.Operation = LifecycleOperation::Replacement;
    Attempt.Plan.Replacement = Manifest("studio.physics", "1.3.0");
    Attempt.RunCallback = true;
    Attempt.CallbackFails = true;

    const auto Observed =
        Hooks::PrepareLifecycleAttempt(Fixture.Owner, Attempt);
    Check(Observed.Staging.Status == LifecycleStageStatus::CallbackFailure,
          "a refusing replacement callback fails the attempt");
    Check(Observed.Staging.Staged.empty(), "a callback failure stages nothing");
    CheckPreviousGenerationSurvived(
        Observed, "a callback failure leaves the previous generation active");
  }
  {
    LoadedState Fixture;
    LifecycleAttempt Attempt = UnloadAttempt();
    Attempt.Plan.Operation = LifecycleOperation::Replacement;
    Attempt.Plan.Replacement = Manifest("studio.physics", "1.3.0");
    Attempt.RunCallback = true;
    Attempt.CallbackThrows = true;

    const auto Observed =
        Hooks::PrepareLifecycleAttempt(Fixture.Owner, Attempt);
    Check(Observed.Staging.Status == LifecycleStageStatus::CallbackFailure &&
              Observed.Staging.Diagnostic.find("threw") != std::string::npos,
          "nothing a replacement callback throws crosses the boundary");
    CheckPreviousGenerationSurvived(
        Observed, "a throwing callback leaves the previous generation active");
  }
}

void CheckMigrationPolicyIsNeverAssumed() {
  LoadedState Fixture;
  LifecycleAttempt Attempt = UnloadAttempt();
  LifecycleUserdataPolicy Unavailable;
  Unavailable.Subject = "Physics.Body#3";
  Unavailable.ClassQualifiedName = "Physics.Body";
  Attempt.Plan.LiveUserdata.push_back(std::move(Unavailable));

  const auto Observed = Hooks::PrepareLifecycleAttempt(Fixture.Owner, Attempt);
  Check(Observed.Staging.Status == LifecycleStageStatus::MigrationFailure,
        "a value with neither migration nor continued validity refuses");
  Check(Observed.Staging.Diagnostic.find("Physics.Body#3") != std::string::npos,
        "the refusal names the live value");
  Check(Observed.Staging.RestoredEveryEntry &&
            Observed.Staging.RestoredEntryStackDepth,
        "a migration refusal restores every journalled effect");
  CheckPreviousGenerationSurvived(
      Observed, "a migration refusal leaves the previous generation active");
}

void CheckEveryStagingFaultRestoresThePreviousGeneration() {
  struct Case final {
    StateFaultPoint Point;
    LifecycleStageStatus Expected;
    std::string_view Description;
  };

  const Case Cases[] = {
      {StateFaultPoint::LifecycleModuleStaging,
       LifecycleStageStatus::AllocationFailure, "module graph allocation"},
      {StateFaultPoint::BindingPathJournal,
       LifecycleStageStatus::InstallationFailure, "protected path journaling"},
      {StateFaultPoint::LifecycleTypeStaging,
       LifecycleStageStatus::AllocationFailure, "type generation staging"},
      {StateFaultPoint::LifecycleMigration,
       LifecycleStageStatus::MigrationFailure, "userdata migration staging"},
      {StateFaultPoint::LifecycleReflectionStaging,
       LifecycleStageStatus::AllocationFailure, "reflection staging"},
      {StateFaultPoint::LifecycleCachePreparation,
       LifecycleStageStatus::CacheFailure, "cache preparation"},
      {StateFaultPoint::LifecycleDispatchStaging,
       LifecycleStageStatus::PublicationFailure, "dispatch staging"},
      {StateFaultPoint::LifecyclePublication,
       LifecycleStageStatus::PublicationFailure, "publication preparation"},
  };

  for (const Case &Injected : Cases) {
    LoadedState Fixture;
    LifecycleAttempt Attempt = UnloadAttempt();

    Attempt.Plan.RemovedTypes = {"int"};
    LifecycleCacheEntry Lookup;
    Lookup.Kind = LifecycleCacheKind::FrozenLookup;
    Lookup.Subject = "Physics.Impulse";
    Attempt.Plan.InvalidatedCaches.push_back(std::move(Lookup));

    Hooks::InjectFault(Fixture.Owner, Injected.Point);
    const auto Observed =
        Hooks::PrepareLifecycleAttempt(Fixture.Owner, Attempt);
    Check(Observed.Staging.Status == Injected.Expected, Injected.Description);
    Check(!Observed.Staging.CompletedStaging && !Observed.Staging.IsPrepared,
          "a refused attempt keeps nothing staged");
    Check(Observed.Staging.RestoredEveryEntry &&
              Observed.Staging.RestoredEntryStackDepth,
          "the journal restores every entry in reverse order");
    CheckPreviousGenerationSurvived(Observed, Injected.Description);
  }
}

void CheckJournalRestorationRunsInReverseOrder() {
  LoadedState Fixture;
  LifecycleAttempt Attempt = UnloadAttempt();
  const auto Observed = Hooks::PrepareLifecycleAttempt(Fixture.Owner, Attempt);
  Check(Observed.Staging.Status == LifecycleStageStatus::Prepared,
        "the attempt stages before restoration is checked");

  std::vector<std::string> Expected = Observed.Staging.RestorationOrder;
  std::reverse(Expected.begin(), Expected.end());
  Check(Expected.size() == Observed.Staging.JournalledEntries,
        "restoration order covers the whole journal");

  Check(!Expected.empty() && Expected.front() == "studio.physics",
        "the module overlay is journalled first and restored last");
  Check(!Observed.Staging.RestorationOrder.empty() &&
            Observed.Staging.RestorationOrder.front() == "Physics.Impulse",
        "the last staged dispatch effect is the first one restored");
}

} // namespace

int RunLifecycleStagingTests() {
  FailureCount = 0;
  CheckLoadOnlyStateRefusesWithoutMutation();
  CheckOneBlockerRefusesBeforeAnythingIsStaged();
  CheckUnloadStagesEveryCategoryUnpublished();
  CheckStagingRetainsThePreviousDispatchGeneration();
  CheckReplacementStagesRetainedDispatchTargets();
  CheckReplacementRejectsAForeignManifest();
  CheckUnknownModuleIsRefused();
  CheckCallbackFailureAndExceptionRestoreEverything();
  CheckMigrationPolicyIsNeverAssumed();
  CheckEveryStagingFaultRestoresThePreviousGeneration();
  CheckJournalRestorationRunsInReverseOrder();
  return FailureCount == 0 ? 0 : 1;
}
