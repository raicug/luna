#pragma once

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

enum class PreparationStatus {
  Prepared,
  IncompletePlan,
  AllocationFailure,
  InconsistentReflection,
  InconsistentTypes
};

[[nodiscard]] std::string_view
PreparationStatusText(PreparationStatus Status) noexcept;

struct PreparedGenerations final {
  std::shared_ptr<const CommittedSymbolTable> Symbols;
  std::shared_ptr<const ReflectionStorage> Reflection;
  std::shared_ptr<const TypeGeneration> Types;
  std::shared_ptr<const GenerationSet> Candidate;

  ReflectionGenerationStatus ReflectionStatus =
      ReflectionGenerationStatus::Valid;

  TypeDeclarationStatus TypeStatus = TypeDeclarationStatus::Acceptable;

  bool TypesAdvance = false;

  bool ReflectionAdvances = false;

  [[nodiscard]] bool IsPrepared() const noexcept {
    return Symbols != nullptr && Reflection != nullptr && Types != nullptr &&
           Candidate != nullptr;
  }
};

[[nodiscard]] PreparationStatus
PrepareGenerations(const RegistrationTransaction &Transaction,
                   const ReflectionDatabase &Reflection,
                   PreparedGenerations &Prepared);

struct PreparedSubmission final {
  std::optional<ErrorDiagnostic> Failure;
  std::size_t PlanIndex = 0;

  BindingRecord *Binding = nullptr;

  [[nodiscard]] bool IsPrepared() const noexcept {
    return !Failure.has_value();
  }
};

[[nodiscard]] PreparedSubmission
PrepareFunctionResources(RegistrationTransaction &Transaction,
                         std::size_t PlanIndex, BindingStore &Bindings,
                         FaultInjector &Faults);

} // namespace Luna::Detail
