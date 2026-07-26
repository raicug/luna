// clang-format off
#include "state/transaction/preparation.hpp"

#include <luna/core/diagnostics/error_category.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include "state/reflection/database.hpp"
#include "state/reflection/storage.hpp"
#include "state/registration/plan.hpp"
#include "state/registration/record.hpp"
#include "state/registration/store.hpp"
#include "state/testing/fault_injector.hpp"
#include "state/testing/fault_point.hpp"
#include "state/transaction/generation_set.hpp"
#include "state/transaction/transaction.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"

#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] ErrorDiagnostic Internal(std::string Message) {
  return ErrorDiagnostic::Create(ErrorCategory::Internal, std::move(Message));
}

} // namespace

std::string_view PreparationStatusText(PreparationStatus Status) noexcept {
  switch (Status) {
  case PreparationStatus::Prepared:
    return "prepared";
  case PreparationStatus::IncompletePlan:
    return "incomplete_plan";
  case PreparationStatus::AllocationFailure:
    return "allocation_failure";
  case PreparationStatus::InconsistentReflection:
    return "inconsistent_reflection";
  case PreparationStatus::InconsistentTypes:
    return "inconsistent_types";
  }
  return "unknown";
}

PreparationStatus PrepareGenerations(const RegistrationTransaction &Transaction,
                                     const ReflectionDatabase &Reflection,
                                     PreparedGenerations &Prepared) {
  Prepared = PreparedGenerations();

  const GenerationSet &Captured = Transaction.Captured();
  const DescriptorPlan &Plan = Transaction.Plan();

  try {
    // Canonical order, never insertion order: an equivalent plan prepares one
    // identical candidate generation.
    const std::vector<std::size_t> Order = Plan.CanonicalOrder();

    std::vector<CommittedSymbol> Added;
    Added.reserve(Order.size());

    // Canonical type declarations of the plan, in the same canonical order.
    std::vector<TypeRecord> DeclaredTypes;

    // The candidate reflection generation starts from the captured committed
    // one, so publication replaces one complete generation rather than merging.
    ReflectionGenerationBuilder Candidate(*Captured.Reflection());

    // The records of the plan, paired with the module identity whose load
    // contributed each one. Records are added in a second pass because the
    // canonical index of a module is only known once every module of the
    // candidate generation has been added.
    std::vector<std::pair<ReflectionRecordFields, std::string_view>>
        PendingRecords;

    for (const std::size_t Index : Order) {
      const DescriptorPlanEntry *Entry = Plan.At(Index);
      if (!Entry)
        return PreparationStatus::IncompletePlan;

      // Only the canonical identity half is required here. Descriptor
      // completeness, including every payload a category requires, is a
      // validation concern that already ran before any payload was staged.
      CommittedSymbol Symbol = MakeCommittedSymbol(*Entry);
      if (!Symbol.IsValid())
        return PreparationStatus::IncompletePlan;
      Added.push_back(std::move(Symbol));

      if (Entry->ModuleFields)
        static_cast<void>(Candidate.AddModule(*Entry->ModuleFields));
      if (Entry->TypeFields)
        static_cast<void>(Candidate.AddType(*Entry->TypeFields));
      if (Entry->TypeConversion)
        DeclaredTypes.push_back(*Entry->TypeConversion);

      // The overload set of a callable is published before its candidates, so
      // every candidate record already names an existing set.
      if (Entry->OverloadSetRecord)
        PendingRecords.emplace_back(*Entry->OverloadSetRecord,
                                    Entry->ModuleIdentity);
      if (Entry->Record)
        PendingRecords.emplace_back(*Entry->Record, Entry->ModuleIdentity);
    }

    // Module provenance: a declaration a module load contributed names the
    // module record of this candidate generation, so every symbol a module
    // publishes reports the identity and version it came from.
    for (auto &[Fields, ModuleIdentity] : PendingRecords) {
      if (!ModuleIdentity.empty()) {
        const std::optional<std::size_t> Owner =
            Candidate.FindModule(ModuleIdentity);
        if (!Owner) {
          Prepared.ReflectionStatus =
              ReflectionGenerationStatus::InconsistentModule;
          return PreparationStatus::InconsistentReflection;
        }
        Fields.Module = *Owner;
      }
      Candidate.AddRecord(std::move(Fields));
    }

    // The candidate type generation. A conflicting converter, an incompatible
    // duplicate declaration, an unavailable nested type, and a
    // canonical-descriptor collision are all rejected here, before anything is
    // installed or published.
    const std::shared_ptr<const TypeGeneration> CapturedTypes =
        Captured.Types() ? Captured.Types() : TypeGeneration::Foundation();
    Prepared.TypesAdvance = !DeclaredTypes.empty();
    std::shared_ptr<const TypeGeneration> PreparedTypes = CapturedTypes;
    if (Prepared.TypesAdvance) {
      PreparedTypes = TypeGeneration::Derive(
          *CapturedTypes, std::move(DeclaredTypes), Prepared.TypeStatus);
      if (!PreparedTypes)
        return PreparationStatus::InconsistentTypes;
    }

    std::shared_ptr<const ReflectionStorage> PreparedReflection;
    const ReflectionGenerationStatus Status =
        Reflection.Prepare(Candidate, PreparedReflection);
    if (Status != ReflectionGenerationStatus::Valid || !PreparedReflection) {
      Prepared.ReflectionStatus = Status;
      return PreparationStatus::InconsistentReflection;
    }

    auto Symbols =
        CommittedSymbolTable::Extend(Captured.Symbols(), std::move(Added));

    // The candidate reflection generation is validated either way, but it only
    // becomes the generation set's reflection half when the plan actually
    // contributes reflection content.
    Prepared.ReflectionAdvances = PlanContributesReflection(Plan);
    auto CandidateSet = GenerationSet::Derive(Captured, Symbols,
                                              Prepared.ReflectionAdvances
                                                  ? PreparedReflection
                                                  : Captured.Reflection(),
                                              PreparedTypes);
    if (!Symbols || !CandidateSet)
      return PreparationStatus::AllocationFailure;

    Prepared.Symbols = std::move(Symbols);
    Prepared.Reflection = std::move(PreparedReflection);
    Prepared.Types = std::move(PreparedTypes);
    Prepared.Candidate = std::move(CandidateSet);
    return PreparationStatus::Prepared;
  } catch (const std::exception &) {
    Prepared = PreparedGenerations();
    return PreparationStatus::AllocationFailure;
  } catch (...) {
    Prepared = PreparedGenerations();
    return PreparationStatus::AllocationFailure;
  }
}

PreparedSubmission
PrepareFunctionResources(RegistrationTransaction &Transaction,
                         std::size_t PlanIndex, BindingStore &Bindings,
                         FaultInjector &Faults) {
  PreparedSubmission Result;
  Result.PlanIndex = PlanIndex;

  DescriptorPlanEntry *Entry = Transaction.Plan().At(PlanIndex);
  if (!Entry || !Entry->Callable) {
    Result.Failure = Internal("Could not stage the planned callable of a "
                              "registration transaction.");
    return Result;
  }

  const std::string GlobalName = Entry->VmPath;

  if (Faults.Consume(StateFaultPoint::BindingRecordAllocation)) {
    Result.Failure = Internal("Could not allocate binding record for global '" +
                              GlobalName + "'.");
    return Result;
  }

  // The canonical signature and candidate identity of the declaration travel
  // with its target, because the staged candidate is one member of the overload
  // set that owns this path.
  const CallableSignatureDescriptor Signature =
      Entry->Symbol.Signature ? *Entry->Symbol.Signature
                              : CallableSignatureDescriptor();

  BindingRecord *Pending = nullptr;
  try {
    Pending = Bindings.Prepare(GlobalName, std::move(*Entry->Callable),
                               Signature, Entry->Identity, Faults);
  } catch (const std::exception &) {
    Result.Failure = Internal("Could not prepare binding record for global '" +
                              GlobalName + "'.");
    return Result;
  } catch (...) {
    Result.Failure =
        Internal("Unknown failure while preparing binding record for global '" +
                 GlobalName + "'.");
    return Result;
  }

  if (!Pending) {
    Result.Failure =
        Internal("Binding store rejected pending global '" + GlobalName + "'.");
    return Result;
  }

  // The staged record is uncommitted, so no ordinary virtual-machine,
  // reflection, or dispatch query can observe it before publication.
  Result.Binding = Pending;
  return Result;
}

} // namespace Luna::Detail
