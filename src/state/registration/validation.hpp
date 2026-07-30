#pragma once

// clang-format off
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include "state/registration/plan.hpp"
#include "state/transaction/capture.hpp"
#include "state/transaction/generation_set.hpp"
#include "state/type/type_generation.hpp"

#include <optional>
#include <string>
#include <string_view>
// clang-format on

namespace Luna::Detail {

enum class RegistrationPrecedence { FoundationRootFunction, GeneralOperation };

struct RegistrationValidationRequest final {
  RegistrationPrecedence Precedence = RegistrationPrecedence::GeneralOperation;

  std::string_view Name;

  const DescriptorPlanEntry *Entry = nullptr;

  PlanEntryKind Category = PlanEntryKind::Function;

  bool HasTarget = false;

  bool VmPathIsOccupied = false;

  bool ScopeIsOwned = true;
  bool ScopeIsCurrent = true;
  std::string_view ParentQualifiedName;

  std::string_view SubjectKindText;

  bool VmPathHoldsUnownedValue = false;
  std::string_view VmPathValueKindText;

  bool JoinsOverloadSet = false;

  const TypeDescriptor *DeclaredValueType = nullptr;
};

[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateRegistration(const RegistrationValidationRequest &Request,
                     const TransactionCapture &Capture,
                     const SymbolView &Symbols);

[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateTransactionEntry(const TransactionCapture &Capture,
                         std::string_view Subject);

[[nodiscard]] std::string
RequestSubject(const RegistrationValidationRequest &Request);

[[nodiscard]] bool IsAvailableCanonicalType(const TypeGeneration &Types,
                                            const TypeDescriptor &Type,
                                            bool AllowVoid) noexcept;

[[nodiscard]] bool IsAvailableCanonicalType(const TypeDescriptor &Type,
                                            bool AllowVoid) noexcept;

} // namespace Luna::Detail
