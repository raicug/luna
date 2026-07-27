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
    const std::vector<std::size_t> Order = Plan.CanonicalOrder();

    std::vector<CommittedSymbol> Added;
    Added.reserve(Order.size());

    std::vector<TypeRecord> DeclaredTypes;

    ReflectionGenerationBuilder Candidate(*Captured.Reflection());

    std::vector<std::pair<ReflectionRecordFields, std::string_view>>
        PendingRecords;

    for (const std::size_t Index : Order) {
      const DescriptorPlanEntry *Entry = Plan.At(Index);
      if (!Entry)
        return PreparationStatus::IncompletePlan;

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

      if (Entry->OverloadSetRecord)
        PendingRecords.emplace_back(*Entry->OverloadSetRecord,
                                    Entry->ModuleIdentity);
      if (Entry->Record)
        PendingRecords.emplace_back(*Entry->Record, Entry->ModuleIdentity);
    }

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

  Result.Binding = Pending;
  return Result;
}

} // namespace Luna::Detail
