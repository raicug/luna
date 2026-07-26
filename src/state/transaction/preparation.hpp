#pragma once

// Preparation is the third phase of one registration transaction. It allocates
// and materializes everything publication will need - the replacement immutable
// committed symbol table, the replacement immutable reflection generation, the
// candidate generation set that bundles them, and the protected virtual-machine
// resources of every planned declaration - and then stops.
//
// Nothing prepared here is visible to an ordinary virtual-machine, reflection,
// or dispatch query: the candidate generation set is a private value that no
// State field points at yet, and a prepared binding record stays uncommitted
// until installation and publication accept it. Publication, the undo journal,
// and reverse restoration are separate phases.

// clang-format off
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include "state/reflection/database.hpp"
#include "state/reflection/storage.hpp"
#include "state/transaction/generation_set.hpp"
#include "state/transaction/transaction.hpp"
#include "state/type/type_generation.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

class BindingRecord;
class BindingStore;
class FaultInjector;

// Deterministic reason a candidate generation is prepared or rejected.
enum class PreparationStatus {
  Prepared,
  IncompletePlan,
  AllocationFailure,
  InconsistentReflection,
  InconsistentTypes
};

[[nodiscard]] std::string_view
PreparationStatusText(PreparationStatus Status) noexcept;

// The unpublished replacement stores of one attempt.
struct PreparedGenerations final {
  std::shared_ptr<const CommittedSymbolTable> Symbols;
  std::shared_ptr<const ReflectionStorage> Reflection;
  std::shared_ptr<const TypeGeneration> Types;
  std::shared_ptr<const GenerationSet> Candidate;

  // The reason a candidate reflection generation was rejected, when one was.
  ReflectionGenerationStatus ReflectionStatus =
      ReflectionGenerationStatus::Valid;

  // The reason a candidate type generation was rejected, when one was: a
  // conflicting converter, an incompatible duplicate declaration, an
  // unavailable nested type, or a canonical-descriptor collision.
  TypeDeclarationStatus TypeStatus = TypeDeclarationStatus::Acceptable;

  // The plan declares at least one canonical type, so publication replaces the
  // committed type generation as well.
  bool TypesAdvance = false;

  // The plan contributes reflection content, so publication replaces the
  // committed reflection generation as well as the committed symbol table. A
  // plan that contributes none keeps the reflection generation it captured, so
  // publication cannot advance a generation nothing changed.
  bool ReflectionAdvances = false;

  [[nodiscard]] bool IsPrepared() const noexcept {
    return Symbols != nullptr && Reflection != nullptr && Types != nullptr &&
           Candidate != nullptr;
  }
};

// Builds the replacement immutable stores of `Transaction` from its captured
// generation set plus its planned declarations. Nothing is published: on
// success the candidate generation set is returned to the caller, and on
// failure `Prepared` is left unprepared and the committed model is untouched.
[[nodiscard]] PreparationStatus
PrepareGenerations(const RegistrationTransaction &Transaction,
                   const ReflectionDatabase &Reflection,
                   PreparedGenerations &Prepared);

// One prepared declaration: either the first deterministic failure of the
// submission or the protected resources it staged.
struct PreparedSubmission final {
  std::optional<ErrorDiagnostic> Failure;
  std::size_t PlanIndex = 0;

  // The staged, still uncommitted binding record of a function declaration.
  BindingRecord *Binding = nullptr;

  [[nodiscard]] bool IsPrepared() const noexcept {
    return !Failure.has_value();
  }
};

// Stages the protected virtual-machine resource of one planned function
// declaration without installing or publishing it. Allocation and preparation
// faults are reported as the submission's failure, and no partial record
// remains staged when preparation fails.
[[nodiscard]] PreparedSubmission
PrepareFunctionResources(RegistrationTransaction &Transaction,
                         std::size_t PlanIndex, BindingStore &Bindings,
                         FaultInjector &Faults);

} // namespace Luna::Detail
