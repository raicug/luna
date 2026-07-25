#pragma once

// clang-format off
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include <optional>
#include <string>
#include <utility>
// clang-format on

namespace Luna {

class RegistrationResult {
public:
  [[nodiscard]] static RegistrationResult Success() {
    return RegistrationResult();
  }

  [[nodiscard]] static RegistrationResult Failure(ErrorCategory Category,
                                                  std::string Message) {
    return Failure(ErrorDiagnostic::Create(Category, std::move(Message)));
  }

  [[nodiscard]] static RegistrationResult
  Failure(ErrorDiagnostic DiagnosticValue) {
    return RegistrationResult(std::move(DiagnosticValue));
  }

  RegistrationResult(const RegistrationResult &) = default;
  RegistrationResult &operator=(const RegistrationResult &) = default;
  RegistrationResult(RegistrationResult &&) = default;
  RegistrationResult &operator=(RegistrationResult &&) = default;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return !DiagnosticValue.has_value();
  }

  [[nodiscard]] const ErrorDiagnostic *Diagnostic() const noexcept {
    return DiagnosticValue ? &*DiagnosticValue : nullptr;
  }

private:
  RegistrationResult() = default;

  explicit RegistrationResult(ErrorDiagnostic DiagnosticValue)
      : DiagnosticValue(std::move(DiagnosticValue)) {}

  std::optional<ErrorDiagnostic> DiagnosticValue;
};

} // namespace Luna
