// clang-format off
#include "state/transaction/lifecycle_publication.hpp"

#include <luna/core/diagnostics/error_category.hpp>

#include "state/dispatch/generation.hpp"
#include "state/freeze/cache.hpp"
#include "state/module/registry.hpp"
#include "state/reflection/database.hpp"
#include "state/registration/store.hpp"
#include "state/testing/fault_injector.hpp"
#include "state/testing/fault_point.hpp"
#include "state/type/type_generation.hpp"
#include "state/userdata/identity_cache.hpp"
#include "state/userdata/lazy_cache.hpp"
#include "state/vm/owner.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] ErrorDiagnostic
Refusal(ErrorCategory Category, const LifecyclePlan &Plan, std::string Reason) {
  std::string Message("Cannot ");
  Message.append(LifecycleOperationText(Plan.Operation));
  Message.append(" module '");
  Message.append(Plan.Identity);
  Message.append("': ");
  Message.append(std::move(Reason));
  return ErrorDiagnostic::Create(Category, std::move(Message));
}

[[nodiscard]] bool IsFrozenCacheKind(LifecycleCacheKind Kind) noexcept {
  switch (Kind) {
  case LifecycleCacheKind::FrozenLookup:
  case LifecycleCacheKind::FrozenNamespace:
  case LifecycleCacheKind::FrozenModule:
  case LifecycleCacheKind::FrozenMetatable:
    return true;
  case LifecycleCacheKind::LazyMemberValue:
  case LifecycleCacheKind::NativeIdentity:
    return false;
  }
  return false;
}

} // namespace

std::string_view
LifecyclePublishStatusText(LifecyclePublishStatus Status) noexcept {
  switch (Status) {
  case LifecyclePublishStatus::Published:
    return "published";
  case LifecyclePublishStatus::NothingStaged:
    return "nothing_staged";
  case LifecyclePublishStatus::UnsupportedDynamicMode:
    return "unsupported_dynamic_mode";
  case LifecyclePublishStatus::UnusableTransaction:
    return "unusable_transaction";
  case LifecyclePublishStatus::ModuleFailure:
    return "module_failure";
  case LifecyclePublishStatus::ReflectionFailure:
    return "reflection_failure";
  case LifecyclePublishStatus::DispatchFailure:
    return "dispatch_failure";
  case LifecyclePublishStatus::GenerationFailure:
    return "generation_failure";
  }
  return "unknown";
}

LifecyclePublishStatus
PublishLifecycle(RegistrationTransaction &Transaction,
                 const LifecyclePlan &Plan, PreparedLifecycle &Prepared,
                 const LifecyclePublicationTargets &Targets,
                 LifecyclePublicationObservation &Observed) {
  const auto Refuse = [&](LifecyclePublishStatus Status,
                          ErrorDiagnostic Diagnostic) {
    Observed.Status = Status;
    Observed.Diagnostic = Diagnostic.Message();
    Observed.IsPublished = false;
    Transaction.Poison(std::move(Diagnostic));
    Prepared.Rollback();
    return Status;
  };

  if (!Targets.IsComplete())
    return Refuse(LifecyclePublishStatus::UnusableTransaction,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the lifecycle publication has no complete State."));

  // A State or module without dynamic lifecycle support stays load-only: the
  // request is refused deterministically and nothing is published.
  if (!Plan.DynamicLifecycleEnabled)
    return Refuse(
        LifecyclePublishStatus::UnsupportedDynamicMode,
        Refusal(ErrorCategory::StateNotReady, Plan,
                "dynamic module lifecycle is unsupported for this State, "
                "which remains load-only."));

  if (!Prepared.IsStaged() || Prepared.IsRolledBack() ||
      Prepared.IsCommitted() || !Prepared.Journal)
    return Refuse(LifecyclePublishStatus::NothingStaged,
                  Refusal(ErrorCategory::Internal, Plan,
                          "no complete staged generation is available."));

  if (!Transaction.IsOpen() || !Transaction.CanPublish())
    return Refuse(LifecyclePublishStatus::UnusableTransaction,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the outermost transaction cannot publish."));

  if (Targets.Faults->Consume(StateFaultPoint::LifecycleGenerationPublication))
    return Refuse(LifecyclePublishStatus::GenerationFailure,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the new generation set could not be published."));

  DispatchTable &Dispatch = Targets.Bindings->Dispatch();
  if (Prepared.Dispatch->Generation() <= Dispatch.Generation())
    return Refuse(
        LifecyclePublishStatus::DispatchFailure,
        Refusal(ErrorCategory::Internal, Plan,
                "the staged dispatch generation no longer succeeds the "
                "published one."));

  if (!Prepared.Candidate->Reflection() || !Prepared.Candidate->Types())
    return Refuse(LifecyclePublishStatus::GenerationFailure,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the candidate generation set is incomplete."));

  for (const ModuleManifest &Member : Prepared.ModuleGraph) {
    if (!Member.IsValid())
      return Refuse(LifecyclePublishStatus::ModuleFailure,
                    Refusal(ErrorCategory::Internal, Plan,
                            "the staged module graph is inconsistent."));
  }

  const std::uint64_t SupersededDispatch = Dispatch.Generation();

  // Affected caches are invalidated before the replacement becomes visible, so
  // no cached lookup can answer from a generation that is about to be retired.
  for (const LifecycleCacheEntry &Entry : Prepared.InvalidatedCaches) {
    Observed.InvalidatedCaches.push_back(
        std::string(LifecycleCacheKindText(Entry.Kind))
            .append("|")
            .append(Entry.Subject));
    if (IsFrozenCacheKind(Entry.Kind) && Targets.Caches != nullptr &&
        *Targets.Caches) {
      Targets.Caches->reset();
      Observed.DroppedFrozenCaches = true;
      continue;
    }
    if (Entry.Kind == LifecycleCacheKind::LazyMemberValue &&
        Targets.LazyValues != nullptr) {
      Observed.DroppedLazyEntries += Targets.LazyValues->Clear();
      continue;
    }
    if (Entry.Kind == LifecycleCacheKind::NativeIdentity &&
        Targets.Identities != nullptr) {
      std::vector<NativeIdentity> Removed;
      for (const UserdataCacheEntry &Cached : Targets.Identities->Entries()) {
        const bool Affected =
            std::find(Prepared.RemovedSymbols.begin(),
                      Prepared.RemovedSymbols.end(),
                      Cached.ClassSymbol) != Prepared.RemovedSymbols.end();
        if (Affected)
          Removed.push_back(Cached.Identity);
      }
      for (const NativeIdentity &Identity : Removed) {
        if (Targets.Identities->Forget(Identity))
          ++Observed.DroppedIdentityEntries;
      }
      Observed.RetainedIdentityEntries = Targets.Identities->Size();
    }
  }
  Observed.InvalidatedCachesBeforePublication =
      Dispatch.Generation() == SupersededDispatch;

  // The staged graph is already validated, so only an allocation failure can
  // refuse here, and it refuses before the module registry changes at all.
  if (!Targets.Modules->Publish(Prepared.ModuleGraph))
    return Refuse(LifecyclePublishStatus::ModuleFailure,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the new module graph could not be published."));

  // The database and the candidate generation set share one immutable
  // reflection generation, so no later query can observe two of them.
  if (!Targets.Reflection->Publish(Prepared.Candidate->Reflection()))
    return Refuse(LifecyclePublishStatus::ReflectionFailure,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the new reflection generation could not be "
                          "published."));

  // Publishing the dispatch generation retires the previous one without
  // reclaiming it: an invocation, userdata cleanup, or lifecycle journal that
  // still retains it keeps every target and cleanup record it needs alive.
  if (!Dispatch.Publish(Prepared.Dispatch))
    return Refuse(LifecyclePublishStatus::DispatchFailure,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the new dispatch generation could not be "
                          "published."));

  *Targets.Generations = Prepared.Candidate;
  Targets.Bindings->PublishTypes(Prepared.Candidate->Types());

  for (const DispatchEntry &Entry : Prepared.Dispatch->All()) {
    if (Entry.IsAvailable())
      continue;
    Observed.UnavailableSlots.push_back(Entry.QualifiedName);
  }
  for (const std::string &Path : Prepared.RetainedPaths) {
    if (Dispatch.FindSlot(Path))
      Observed.RetainedSlots.push_back(Path);
  }

  // Every removed table path leaves the virtual machine only after the new
  // generation is published, so a stale closure held by script code resolves
  // an immutable unavailable entry instead of a retired record.
  std::vector<std::string> Removed = Prepared.RemovedPaths;
  std::sort(Removed.begin(), Removed.end(), std::greater<std::string>());
  for (const std::string &Path : Removed) {
    if (Targets.Machine->ClearVmPath(Path))
      Observed.ClearedPaths.push_back(Path);
  }

  Prepared.Commit();
  Transaction.MarkCommitted();

  Observed.Status = LifecyclePublishStatus::Published;
  Observed.IsPublished = true;
  Observed.JournalCommitted = true;
  Observed.PublishedGeneration = (*Targets.Generations)->Generation();
  Observed.PublishedSymbolCount = (*Targets.Generations)->Symbols().Size();
  Observed.PublishedReflectionGeneration = Targets.Reflection->Generation();
  Observed.PublishedTypeGeneration =
      (*Targets.Generations)->Types()->Generation();
  Observed.PublishedDispatchGeneration = Dispatch.Generation();
  Observed.PublishedModuleCount = Targets.Modules->Count();
  Observed.SupersededDispatchGeneration = SupersededDispatch;
  Observed.PreviousDispatchRetained =
      Dispatch.IsGenerationRetained(SupersededDispatch);
  Observed.LifecycleJournalRetainers =
      Dispatch.RetainerCount(DispatchRetainer::LifecycleJournal);
  return LifecyclePublishStatus::Published;
}

} // namespace Luna::Detail