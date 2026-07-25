#pragma once

// clang-format off
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include <optional>
#include <string>
// clang-format on

namespace Luna::Detail {

enum class InvocationValidationState { Success, CallerError, InternalError };

class InvocationValidationResult final {
public:
  InvocationValidationResult() = default;

  [[nodiscard]] InvocationValidationState State() const noexcept {
    return StateValue;
  }

  [[nodiscard]] bool IsSuccess() const noexcept {
    return StateValue == InvocationValidationState::Success;
  }

  [[nodiscard]] const ErrorDiagnostic *Diagnostic() const noexcept {
    return DiagnosticValue ? &*DiagnosticValue : nullptr;
  }

  bool RecordCallerFailure(std::string Message);
  bool RecordInternalFailure(std::string Message);

private:
  bool Record(InvocationValidationState FailureState, ErrorCategory Category,
              std::string Message);

  InvocationValidationState StateValue = InvocationValidationState::Success;
  std::optional<ErrorDiagnostic> DiagnosticValue;
};

} // namespace Luna::Detail
