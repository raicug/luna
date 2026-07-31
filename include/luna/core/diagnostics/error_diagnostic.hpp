#pragma once

// clang-format off
#include <luna/core/diagnostics/error_category.hpp>

#include <string>
#include <utility>
// clang-format on

namespace Luna {

class ErrorDiagnostic {
public:
  [[nodiscard]] static ErrorDiagnostic Create(ErrorCategory Category,
                                              std::string Message) {
    return ErrorDiagnostic(Category, std::move(Message));
  }

  ErrorDiagnostic(const ErrorDiagnostic &) = default;
  ErrorDiagnostic &operator=(const ErrorDiagnostic &) = default;

  ErrorDiagnostic(ErrorDiagnostic &&Other)
      : CategoryValue(Other.CategoryValue), MessageValue(Other.MessageValue) {}

  ErrorDiagnostic &operator=(ErrorDiagnostic &&Other) {
    return operator=(static_cast<const ErrorDiagnostic &>(Other));
  }

  [[nodiscard]] ErrorCategory Category() const noexcept {
    return CategoryValue;
  }

  [[nodiscard]] const std::string &Message() const noexcept {
    return MessageValue;
  }

private:
  ErrorDiagnostic(ErrorCategory Category, std::string Message)
      : CategoryValue(Category),
        MessageValue(Message.empty() ? FallbackMessage(Category)
                                     : std::move(Message)) {}

  [[nodiscard]] static std::string FallbackMessage(ErrorCategory Category) {
    switch (Category) {
    case ErrorCategory::StateNotReady:
      return "State is not ready.";
    case ErrorCategory::InvalidGlobalName:
      return "Global name is invalid.";
    case ErrorCategory::DuplicateGlobalName:
      return "Global name is already registered.";
    case ErrorCategory::NullCallable:
      return "Callable target is null.";
    case ErrorCategory::Compilation:
      return "Compilation failed.";
    case ErrorCategory::Runtime:
      return "Runtime execution failed.";
    case ErrorCategory::Interrupted:
      return "Execution was interrupted.";
    case ErrorCategory::Internal:
      return "Internal Luna error.";
    }
    return "Luna error.";
  }

  ErrorCategory CategoryValue;
  std::string MessageValue;
};

} // namespace Luna
