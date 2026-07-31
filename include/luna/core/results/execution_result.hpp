#pragma once

// clang-format off
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include <optional>
#include <string>
#include <utility>
// clang-format on

namespace Luna {

class ExecutionResult {
public:
  [[nodiscard]] static ExecutionResult Success() { return ExecutionResult(); }

  [[nodiscard]] static ExecutionResult Failure(ErrorCategory Category,
                                               std::string Message) {
    return Failure(ErrorDiagnostic::Create(Category, std::move(Message)));
  }

  [[nodiscard]] static ExecutionResult
  Failure(ErrorDiagnostic DiagnosticValue) {
    return ExecutionResult(std::move(DiagnosticValue));
  }

  [[nodiscard]] static ExecutionResult Interrupted(std::string Message) {
    return ExecutionResult(ErrorDiagnostic::Create(ErrorCategory::Interrupted,
                                                   std::move(Message)));
  }

  ExecutionResult(const ExecutionResult &) = default;
  ExecutionResult &operator=(const ExecutionResult &) = default;
  ExecutionResult(ExecutionResult &&) = default;
  ExecutionResult &operator=(ExecutionResult &&) = default;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return !DiagnosticValue.has_value();
  }

  [[nodiscard]] bool IsInterrupted() const noexcept {
    return DiagnosticValue &&
           DiagnosticValue->Category() == ErrorCategory::Interrupted;
  }

  [[nodiscard]] const ErrorDiagnostic *Diagnostic() const noexcept {
    return DiagnosticValue ? &*DiagnosticValue : nullptr;
  }

private:
  ExecutionResult() = default;

  explicit ExecutionResult(ErrorDiagnostic DiagnosticValue)
      : DiagnosticValue(std::move(DiagnosticValue)) {}

  std::optional<ErrorDiagnostic> DiagnosticValue;
};

} // namespace Luna
