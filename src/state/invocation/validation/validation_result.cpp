// clang-format off
#include "state/invocation/validation/validation_result.hpp"

#include <utility>
// clang-format on

namespace Luna::Detail {

bool InvocationValidationResult::RecordCallerFailure(std::string Message) {
  return Record(InvocationValidationState::CallerError, ErrorCategory::Runtime,
                std::move(Message));
}

bool InvocationValidationResult::RecordInternalFailure(std::string Message) {
  return Record(InvocationValidationState::InternalError,
                ErrorCategory::Internal, std::move(Message));
}

bool InvocationValidationResult::Record(InvocationValidationState FailureState,
                                        ErrorCategory Category,
                                        std::string Message) {
  if (!IsSuccess())
    return false;

  DiagnosticValue = ErrorDiagnostic::Create(Category, std::move(Message));
  StateValue = FailureState;
  return true;
}

} // namespace Luna::Detail
