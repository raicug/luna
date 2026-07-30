// clang-format off
#include "state/transaction/lifecycle_staging.hpp"

#include <luna/core/diagnostics/error_category.hpp>

#include "state/freeze/cache.hpp"
#include "state/module/registry.hpp"
#include "state/reflection/storage.hpp"
#include "state/registration/store.hpp"
#include "state/testing/fault_injector.hpp"
#include "state/testing/fault_point.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"
#include "state/vm/owner.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] bool PathContains(std::string_view Scope,
                                std::string_view Candidate) noexcept {
  if (Scope.empty() || Candidate.size() < Scope.size())
    return false;
  if (Candidate.compare(0, Scope.size(), Scope) != 0)
    return false;
  if (Candidate.size() == Scope.size())
    return true;
  return Candidate[Scope.size()] == '.';
}

[[nodiscard]] bool IsRemoved(const LifecyclePlan &Plan,
                             std::string_view Name) noexcept {
  if (Name.empty())
    return false;
  for (const std::string &Subject : Plan.RemovedSubjects) {
    if (PathContains(Subject, Name))
      return true;
  }
  return false;
}

[[nodiscard]] bool IsRetained(const LifecyclePlan &Plan,
                              std::string_view Name) noexcept {
  if (Name.empty())
    return false;
  for (const std::string &Retained : Plan.RetainedPaths) {
    if (PathContains(Name, Retained))
      return true;
  }
  return false;
}

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

[[nodiscard]] std::string SlotDetail(std::uint64_t Slot) {
  return std::string("slot ").append(std::to_string(Slot));
}

} // namespace

std::string_view LifecycleStagedKindText(LifecycleStagedKind Kind) noexcept {
  switch (Kind) {
  case LifecycleStagedKind::ModuleGraph:
    return "module_graph";
  case LifecycleStagedKind::TablePath:
    return "table_path";
  case LifecycleStagedKind::Type:
    return "type";
  case LifecycleStagedKind::UserdataAction:
    return "userdata_action";
  case LifecycleStagedKind::Reflection:
    return "reflection";
  case LifecycleStagedKind::Cache:
    return "cache";
  case LifecycleStagedKind::UnavailableSlot:
    return "unavailable_slot";
  case LifecycleStagedKind::DispatchTarget:
    return "dispatch_target";
  }
  return "invalid";
}

std::string_view
LifecycleUserdataActionText(LifecycleUserdataAction Action) noexcept {
  switch (Action) {
  case LifecycleUserdataAction::Migrate:
    return "migrate";
  case LifecycleUserdataAction::RemainValid:
    return "remain_valid";
  }
  return "invalid";
}

std::string_view
LifecycleStageStatusText(LifecycleStageStatus Status) noexcept {
  switch (Status) {
  case LifecycleStageStatus::Prepared:
    return "prepared";
  case LifecycleStageStatus::ValidationFailure:
    return "validation_failure";
  case LifecycleStageStatus::CallbackFailure:
    return "callback_failure";
  case LifecycleStageStatus::AllocationFailure:
    return "allocation_failure";
  case LifecycleStageStatus::InstallationFailure:
    return "installation_failure";
  case LifecycleStageStatus::MigrationFailure:
    return "migration_failure";
  case LifecycleStageStatus::CacheFailure:
    return "cache_failure";
  case LifecycleStageStatus::PublicationFailure:
    return "publication_failure";
  }
  return "unknown";
}

std::string LifecycleStagedItem::Text() const {
  std::string Result(LifecycleStagedKindText(Kind));
  Result.push_back('|');
  Result.append(Subject);
  Result.push_back('|');
  Result.append(Detail);
  return Result;
}

bool operator==(const LifecycleStagedItem &Left,
                const LifecycleStagedItem &Right) {
  return Left.Kind == Right.Kind && Left.Subject == Right.Subject &&
         Left.Detail == Right.Detail;
}

std::strong_ordering CompareStaged(const LifecycleStagedItem &Left,
                                   const LifecycleStagedItem &Right) {
  if (Left.Kind != Right.Kind)
    return static_cast<std::uint8_t>(Left.Kind) <=>
           static_cast<std::uint8_t>(Right.Kind);
  if (const std::strong_ordering Subjects =
          Left.Subject.compare(Right.Subject) <=> 0;
      Subjects != std::strong_ordering::equal)
    return Subjects;
  return Left.Detail.compare(Right.Detail) <=> 0;
}

PreparedLifecycle::~PreparedLifecycle() noexcept {
  if (!RolledBack && !Committed)
    Rollback();
}

void PreparedLifecycle::Rollback() noexcept {
  if (RolledBack)
    return;

  if (Journal)
    Journal->Undo();

  ObserveLifecycleStaging(*this, Observation);

  Candidate.reset();
  Dispatch.reset();
  Reflection.reset();
  Types.reset();
  Symbols.reset();
  ModuleGraph.clear();
  InvalidatedCaches.clear();

  PreviousDispatch.Release();
  PreviousCaches.reset();
  Journal.reset();

  RolledBack = true;
  Observation.IsPrepared = false;
  Observation.IsRolledBack = true;
}

void PreparedLifecycle::Commit() noexcept {
  if (RolledBack || Committed)
    return;

  if (Journal)
    Journal->Commit();

  ObserveLifecycleStaging(*this, Observation);

  PreviousDispatch.Release();
  PreviousCaches.reset();

  Committed = true;
  Observation.IsPrepared = true;
  Observation.IsRolledBack = false;
}

void ObserveLifecycleStaging(const PreparedLifecycle &Prepared,
                             LifecycleStagingObservation &Observed) {
  Observed.Staged.clear();
  Observed.Staged.reserve(Prepared.Staged.size());
  for (const LifecycleStagedItem &Item : Prepared.Staged)
    Observed.Staged.push_back(Item.Text());

  Observed.IsPrepared = Prepared.IsStaged();
  Observed.StagedModuleCount = Prepared.ModuleGraph.size();
  Observed.StagedCacheEntries = Prepared.InvalidatedCaches.size();
  Observed.StagedSymbolCount = Prepared.Symbols ? Prepared.Symbols->Size() : 0;
  Observed.StagedGeneration =
      Prepared.Candidate ? Prepared.Candidate->Generation() : 0;
  Observed.StagedReflectionGeneration =
      Prepared.Reflection ? Prepared.Reflection->Generation() : 0;
  Observed.StagedTypeGeneration =
      Prepared.Types ? Prepared.Types->Generation() : 0;
  Observed.StagedDispatchGeneration =
      Prepared.Dispatch ? Prepared.Dispatch->Generation() : 0;
  Observed.PreviousDispatchRetained = Prepared.PreviousDispatch.IsHeld();

  if (!Prepared.Journal)
    return;

  const InstallationJournal &Journal = *Prepared.Journal;
  Observed.JournalledEntries = Journal.Size();
  Observed.JournalledPaths =
      Journal.CountOf(InstallationScope::VirtualMachinePath);
  Observed.JournalledOverlays =
      Observed.JournalledEntries - Observed.JournalledPaths;
  Observed.PriorValueKinds.clear();
  Observed.JournalledPathNames.clear();
  for (const JournalEntry &Entry : Journal.Journalled()) {
    if (Entry.Scope != InstallationScope::VirtualMachinePath)
      continue;
    Observed.JournalledPathNames.push_back(Entry.Path);
    Observed.PriorValueKinds.emplace_back(VmValueKindText(Entry.PriorKind()));
  }
  Observed.RestorationOrder = Journal.RestorationOrder();
  Observed.RestoredEveryEntry = Journal.RestoredEveryEntry();
  Observed.RestoredEntryStackDepth = Journal.RestoredEntryStackDepth();
  Observed.EntryStackDepth = Journal.EntryStackDepth();
}

LifecycleStageStatus PrepareLifecycle(RegistrationTransaction &Transaction,
                                      const LifecyclePlan &Plan,
                                      const LifecycleAnalysis &Analysis,
                                      const LifecycleStagingSources &Sources,
                                      const LifecycleStagingCallback &Callback,
                                      PreparedLifecycle &Prepared) {
  LifecycleStagingObservation &Observed = Prepared.Observed();
  const GenerationSet &Previous = Transaction.Captured();

  Observed.EntryStackDepth = Transaction.EntryStackDepth();
  Observed.PreviousGeneration = Previous.Generation();
  Observed.PreviousSymbolCount = Previous.Symbols().Size();
  Observed.PreviousReflectionGeneration =
      Previous.Reflection() ? Previous.Reflection()->Generation() : 0;
  Observed.PreviousTypeGeneration =
      Previous.Types() ? Previous.Types()->Generation() : 0;
  Observed.PreviousModuleCount = Sources.Modules ? Sources.Modules->Count() : 0;
  Observed.PreviousDispatchGeneration =
      Sources.Bindings ? Sources.Bindings->Dispatch().Generation() : 0;

  const auto Refuse = [&](LifecycleStageStatus Status,
                          ErrorDiagnostic Diagnostic) {
    Observed.Status = Status;
    Observed.Diagnostic = Diagnostic.Message();
    Transaction.Poison(std::move(Diagnostic));
    Prepared.Rollback();
    Observed.Status = Status;
    return Status;
  };

  const auto Stage = [&Prepared](LifecycleStagedKind Kind, std::string Subject,
                                 std::string Detail) {
    LifecycleStagedItem Item;
    Item.Kind = Kind;
    Item.Subject = std::move(Subject);
    Item.Detail = std::move(Detail);
    const auto Position = std::lower_bound(
        Prepared.Staged.begin(), Prepared.Staged.end(), Item,
        [](const LifecycleStagedItem &Left, const LifecycleStagedItem &Right) {
          return CompareStaged(Left, Right) == std::strong_ordering::less;
        });
    if (Position != Prepared.Staged.end() && *Position == Item)
      return;
    Prepared.Staged.insert(Position, std::move(Item));
  };

  if (Sources.Machine == nullptr || Sources.Bindings == nullptr ||
      Sources.Faults == nullptr)
    return Refuse(LifecycleStageStatus::ValidationFailure,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the lifecycle attempt has no complete State."));
  if (!Transaction.IsOpen())
    return Refuse(LifecycleStageStatus::ValidationFailure,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the outermost transaction is no longer open."));
  if (Plan.Identity.empty())
    return Refuse(LifecycleStageStatus::ValidationFailure,
                  Refusal(ErrorCategory::StateNotReady, Plan,
                          "the request names no module identity."));

  if (!Plan.DynamicLifecycleEnabled)
    return Refuse(
        LifecycleStageStatus::ValidationFailure,
        Refusal(ErrorCategory::StateNotReady, Plan,
                "dynamic module lifecycle is unsupported for this State, "
                "which remains load-only."));

  if (Analysis.Operation != Plan.Operation ||
      Analysis.Identity != Plan.Identity)
    return Refuse(LifecycleStageStatus::ValidationFailure,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the analysis describes a different request."));

  if (!Analysis.IsPermitted())
    return Refuse(
        LifecycleStageStatus::ValidationFailure,
        Refusal(ErrorCategory::StateNotReady, Plan, Analysis.Message()));

  if (Sources.Modules == nullptr || !Sources.Modules->IsLoaded(Plan.Identity))
    return Refuse(LifecycleStageStatus::ValidationFailure,
                  Refusal(ErrorCategory::StateNotReady, Plan,
                          "the module is not loaded."));

  if (Plan.Operation == LifecycleOperation::Replacement &&
      (!Plan.Replacement.IsValid() ||
       Plan.Replacement.Identity() != Plan.Identity))
    return Refuse(
        LifecycleStageStatus::ValidationFailure,
        Refusal(ErrorCategory::StateNotReady, Plan,
                "the replacement manifest does not describe this module."));

  if (Callback) {
    std::optional<ErrorDiagnostic> Failure;
    if (Sources.Faults->Consume(StateFaultPoint::LifecycleCallback)) {
      Failure = Refusal(ErrorCategory::Internal, Plan,
                        "the replacement registration callback failed.");
    } else {
      try {
        Failure = Callback();
      } catch (...) {
        Failure = Refusal(ErrorCategory::Internal, Plan,
                          "the replacement registration callback threw.");
      }
    }
    if (Failure)
      return Refuse(LifecycleStageStatus::CallbackFailure, std::move(*Failure));
  }

  Prepared.Journal = std::make_unique<InstallationJournal>(
      *Sources.Machine, *Sources.Bindings, *Sources.Faults,
      Transaction.EntryStackDepth());
  Prepared.PreviousDispatch =
      Sources.Bindings->Dispatch().Retain(DispatchRetainer::LifecycleJournal);
  Prepared.PreviousCaches = Sources.Caches;
  Observed.LifecycleJournalRetainers =
      Sources.Bindings->Dispatch().RetainerCount(
          DispatchRetainer::LifecycleJournal);

  if (Sources.Faults->Consume(StateFaultPoint::LifecycleModuleStaging))
    return Refuse(LifecycleStageStatus::AllocationFailure,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the staged module graph could not be allocated."));

  std::string RemovedKey;
  for (const ModuleManifest *Loaded : Sources.Modules->LoadedModules()) {
    if (Loaded == nullptr)
      continue;
    if (Loaded->Identity() == Plan.Identity) {
      RemovedKey = Loaded->Key();
      continue;
    }
    Prepared.ModuleGraph.push_back(*Loaded);
  }
  if (Plan.Operation == LifecycleOperation::Replacement)
    Prepared.ModuleGraph.push_back(Plan.Replacement);
  std::sort(Prepared.ModuleGraph.begin(), Prepared.ModuleGraph.end(),
            [](const ModuleManifest &Left, const ModuleManifest &Right) {
              return CompareManifest(Left, Right) == std::strong_ordering::less;
            });
  Prepared.Journal->JournalOverlay(InstallationScope::Module, Plan.Identity);
  Stage(LifecycleStagedKind::ModuleGraph, RemovedKey, "removed");
  for (const ModuleManifest &Member : Prepared.ModuleGraph) {
    Stage(LifecycleStagedKind::ModuleGraph, Member.Key(),
          Member.Identity() == Plan.Identity ? "installed" : "retained");
  }

  std::vector<std::string> Paths;
  for (std::size_t Index = 0; Index < Previous.Symbols().Size(); ++Index) {
    const CommittedSymbol *Symbol = Previous.Symbols().At(Index);
    if (Symbol == nullptr || Symbol->VmPath.empty())
      continue;
    if (IsRemoved(Plan, Symbol->Symbol.QualifiedName))
      Paths.push_back(Symbol->VmPath);
  }
  for (const std::string &Retained : Plan.RetainedPaths)
    Paths.push_back(Retained);
  std::sort(Paths.begin(), Paths.end());
  Paths.erase(std::unique(Paths.begin(), Paths.end()), Paths.end());

  for (const std::string &Path : Paths) {
    if (Sources.Faults->Consume(StateFaultPoint::BindingPathJournal) ||
        !Prepared.Journal->JournalVirtualMachinePath(Path))
      return Refuse(
          LifecycleStageStatus::InstallationFailure,
          Refusal(ErrorCategory::Internal, Plan,
                  "the prior value of '" + Path + "' could not be recorded."));
    const bool Retained =
        std::find(Plan.RetainedPaths.begin(), Plan.RetainedPaths.end(), Path) !=
        Plan.RetainedPaths.end();
    if (Retained)
      Prepared.RetainedPaths.push_back(Path);
    else
      Prepared.RemovedPaths.push_back(Path);
    Stage(LifecycleStagedKind::TablePath, Path,
          Retained ? "retained" : "removed");
  }

  if (Sources.Faults->Consume(StateFaultPoint::LifecycleTypeStaging))
    return Refuse(LifecycleStageStatus::AllocationFailure,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the staged type generation could not be prepared."));

  const std::shared_ptr<const TypeGeneration> PreviousTypes =
      Previous.Types() ? Previous.Types() : TypeGeneration::Foundation();
  if (Plan.RemovedTypes.empty()) {
    Prepared.Types = PreviousTypes;
  } else {
    std::vector<TypeRecord> RetainedTypes;
    for (const TypeRecord &Record : PreviousTypes->All()) {
      const bool Removed =
          std::find(Plan.RemovedTypes.begin(), Plan.RemovedTypes.end(),
                    Record.PublicName) != Plan.RemovedTypes.end();
      if (Removed) {
        Stage(LifecycleStagedKind::Type, Record.PublicName, "removed");
        continue;
      }
      RetainedTypes.push_back(Record);
    }
    TypeDeclarationStatus TypeStatus = TypeDeclarationStatus::Acceptable;
    Prepared.Types = TypeGeneration::Retain(
        *PreviousTypes, std::move(RetainedTypes), TypeStatus);
    if (!Prepared.Types)
      return Refuse(
          LifecycleStageStatus::AllocationFailure,
          Refusal(ErrorCategory::Internal, Plan,
                  "the staged type generation is inconsistent (" +
                      std::string(TypeDeclarationStatusText(TypeStatus)) +
                      ")."));
    Prepared.Journal->JournalOverlay(InstallationScope::Type, Plan.Identity);
  }

  if (Sources.Faults->Consume(StateFaultPoint::LifecycleMigration))
    return Refuse(LifecycleStageStatus::MigrationFailure,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the staged userdata migration failed."));

  for (const LifecycleUserdataPolicy &Policy : Plan.LiveUserdata) {
    if (!Policy.MigrationAvailable && !Policy.RemainsValid)
      return Refuse(LifecycleStageStatus::MigrationFailure,
                    Refusal(ErrorCategory::StateNotReady, Plan,
                            "the live value '" + Policy.Subject +
                                "' declares neither a migration nor continued "
                                "validity."));
    const LifecycleUserdataAction Action =
        Policy.MigrationAvailable ? LifecycleUserdataAction::Migrate
                                  : LifecycleUserdataAction::RemainValid;
    Prepared.Journal->JournalOverlay(InstallationScope::Userdata,
                                     Policy.Subject);
    Stage(LifecycleStagedKind::UserdataAction, Policy.Subject,
          std::string(LifecycleUserdataActionText(Action)));
    ++Observed.StagedUserdataActions;
  }

  if (Sources.Faults->Consume(StateFaultPoint::LifecycleReflectionStaging))
    return Refuse(
        LifecycleStageStatus::AllocationFailure,
        Refusal(ErrorCategory::Internal, Plan,
                "the staged reflection generation could not be prepared."));

  const std::shared_ptr<const ReflectionStorage> PreviousReflection =
      Previous.Reflection() ? Previous.Reflection()
                            : ReflectionStorage::Empty();

  std::optional<std::size_t> RemovedModule;
  std::vector<std::optional<std::size_t>> ModuleRemap(
      PreviousReflection->ModuleCount());
  std::vector<ReflectionModuleFields> RetainedModules;
  for (std::size_t Index = 0; Index < PreviousReflection->ModuleCount();
       ++Index) {
    const ReflectionModuleFields *Module = PreviousReflection->ModuleAt(Index);
    if (Module == nullptr)
      continue;
    if (Module->Identity == Plan.Identity) {
      RemovedModule = Index;
      continue;
    }
    ModuleRemap[Index] = RetainedModules.size();
    RetainedModules.push_back(*Module);
  }

  std::set<SymbolId> DroppedSymbols;
  std::vector<ReflectionRecordFields> RetainedRecords;
  for (std::size_t Index = 0; Index < PreviousReflection->RecordCount();
       ++Index) {
    const ReflectionRecordFields *Record = PreviousReflection->RecordAt(Index);
    if (Record == nullptr)
      continue;
    const bool BelongsToModule =
        Record->Module && RemovedModule && *Record->Module == *RemovedModule;

    const bool RetainedPath = IsRetained(Plan, Record->QualifiedName);
    if (!RetainedPath &&
        (BelongsToModule || IsRemoved(Plan, Record->QualifiedName))) {
      DroppedSymbols.insert(Record->Id);
      Stage(LifecycleStagedKind::Reflection, Record->QualifiedName, "removed");
      continue;
    }
    ReflectionRecordFields Retained = *Record;
    if (Retained.Module)
      Retained.Module = ModuleRemap[*Retained.Module];
    RetainedRecords.push_back(std::move(Retained));
  }

  std::vector<ReflectionTypeFields> RetainedTypeFields;
  for (std::size_t Index = 0; Index < PreviousReflection->TypeCount();
       ++Index) {
    const ReflectionTypeFields *Type = PreviousReflection->TypeAt(Index);
    if (Type == nullptr)
      continue;
    const bool Removed =
        std::find(Plan.RemovedTypes.begin(), Plan.RemovedTypes.end(),
                  Type->Name) != Plan.RemovedTypes.end() ||
        DroppedSymbols.find(Type->Declaration) != DroppedSymbols.end();
    if (Removed) {
      Stage(LifecycleStagedKind::Type, Type->Name, "removed");
      continue;
    }
    RetainedTypeFields.push_back(*Type);
  }

  ReflectionGenerationStatus ReflectionStatus =
      ReflectionGenerationStatus::Valid;
  Prepared.Reflection = ReflectionStorage::Build(
      PreviousReflection->Generation() + 1, std::move(RetainedRecords),
      std::move(RetainedTypeFields), std::move(RetainedModules),
      ReflectionStatus);
  if (!Prepared.Reflection)
    return Refuse(LifecycleStageStatus::AllocationFailure,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the staged reflection generation is inconsistent (" +
                              std::string(ReflectionGenerationStatusText(
                                  ReflectionStatus)) +
                              ")."));
  Prepared.Journal->JournalOverlay(InstallationScope::Reflection,
                                   Plan.Identity);

  std::vector<CommittedSymbol> RetainedSymbols;
  for (std::size_t Index = 0; Index < Previous.Symbols().Size(); ++Index) {
    const CommittedSymbol *Symbol = Previous.Symbols().At(Index);
    if (Symbol == nullptr)
      continue;
    if (IsRemoved(Plan, Symbol->Symbol.QualifiedName) ||
        DroppedSymbols.find(Symbol->Identity) != DroppedSymbols.end())
      continue;
    RetainedSymbols.push_back(*Symbol);
  }
  Prepared.Symbols = CommittedSymbolTable::Build(std::move(RetainedSymbols));
  Prepared.RemovedSymbols.assign(DroppedSymbols.begin(), DroppedSymbols.end());

  if (Sources.Faults->Consume(StateFaultPoint::LifecycleCachePreparation))
    return Refuse(LifecycleStageStatus::CacheFailure,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the staged cache invalidation failed."));

  Prepared.InvalidatedCaches = Plan.InvalidatedCaches;
  std::sort(
      Prepared.InvalidatedCaches.begin(), Prepared.InvalidatedCaches.end(),
      [](const LifecycleCacheEntry &Left, const LifecycleCacheEntry &Right) {
        if (Left.Kind != Right.Kind)
          return static_cast<std::uint8_t>(Left.Kind) <
                 static_cast<std::uint8_t>(Right.Kind);
        return Left.Subject < Right.Subject;
      });
  for (const LifecycleCacheEntry &Entry : Prepared.InvalidatedCaches) {
    const InstallationScope Scope =
        Entry.Kind == LifecycleCacheKind::NativeIdentity
            ? InstallationScope::IdentityCache
            : InstallationScope::LookupCache;
    Prepared.Journal->JournalOverlay(Scope, Entry.Subject);
    Stage(LifecycleStagedKind::Cache, Entry.Subject,
          std::string(LifecycleCacheKindText(Entry.Kind)));
  }

  if (Sources.Faults->Consume(StateFaultPoint::LifecycleDispatchStaging))
    return Refuse(
        LifecycleStageStatus::PublicationFailure,
        Refusal(ErrorCategory::Internal, Plan,
                "the staged dispatch generation could not be prepared."));

  DispatchTable &Dispatch = Sources.Bindings->Dispatch();
  const std::shared_ptr<const DispatchGeneration> CurrentDispatch =
      Dispatch.Capture();
  if (!CurrentDispatch)
    return Refuse(LifecycleStageStatus::PublicationFailure,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the current dispatch generation is unavailable."));

  std::vector<DispatchEntry> StagedEntries(CurrentDispatch->All().begin(),
                                           CurrentDispatch->All().end());
  for (const std::string &Path : Paths) {
    const std::optional<DispatchSlotId> Slot = Dispatch.FindSlot(Path);
    if (!Slot)
      continue;
    const bool Retained =
        std::find(Plan.RetainedPaths.begin(), Plan.RetainedPaths.end(), Path) !=
        Plan.RetainedPaths.end();
    if (Retained) {
      Stage(LifecycleStagedKind::DispatchTarget, Path, SlotDetail(Slot->Value));
      Prepared.Journal->JournalOverlay(InstallationScope::Dispatch, Path);
      ++Observed.StagedDispatchTargets;
      continue;
    }

    const auto Position = std::find_if(
        StagedEntries.begin(), StagedEntries.end(),
        [&Slot](const DispatchEntry &Entry) { return Entry.Slot == *Slot; });
    if (Position != StagedEntries.end()) {
      Position->Target = nullptr;
      Position->Faults = nullptr;
      Position->Types = nullptr;
    } else {
      DispatchEntry Unavailable;
      Unavailable.Slot = *Slot;
      Unavailable.QualifiedName = Path;
      StagedEntries.push_back(std::move(Unavailable));
    }
    Stage(LifecycleStagedKind::UnavailableSlot, Path, SlotDetail(Slot->Value));
    Prepared.Journal->JournalOverlay(InstallationScope::Dispatch, Path);
    ++Observed.StagedUnavailableSlots;
  }

  Prepared.Dispatch =
      DispatchGeneration::Derive(*CurrentDispatch, std::move(StagedEntries));
  if (!Prepared.Dispatch)
    return Refuse(LifecycleStageStatus::PublicationFailure,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the staged dispatch generation was rejected."));

  if (Sources.Faults->Consume(StateFaultPoint::LifecyclePublication))
    return Refuse(
        LifecycleStageStatus::PublicationFailure,
        Refusal(ErrorCategory::Internal, Plan,
                "the candidate generation set could not be prepared."));

  Prepared.Candidate = GenerationSet::Derive(
      Previous, Prepared.Symbols, Prepared.Reflection, Prepared.Types);
  if (!Prepared.Candidate)
    return Refuse(LifecycleStageStatus::PublicationFailure,
                  Refusal(ErrorCategory::Internal, Plan,
                          "the candidate generation set was rejected."));

  Observed.Status = LifecycleStageStatus::Prepared;
  ObserveLifecycleStaging(Prepared, Observed);
  Observed.CompletedStaging = true;
  return LifecycleStageStatus::Prepared;
}

} // namespace Luna::Detail
