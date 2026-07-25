// clang-format off
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/core/results/execution_result.hpp>
#include <luna/core/results/registration_result.hpp>

#include <array>
#include <string>
#include <type_traits>
#include <utility>
// clang-format on

namespace {
template <class Result>
bool HasFailure(const Result &Value, Luna::ErrorCategory Category,
                const std::string &Message) {
  return !Value.IsSuccess() && Value.Diagnostic() != nullptr &&
         Value.Diagnostic()->Category() == Category &&
         Value.Diagnostic()->Message() == Message;
}

template <class Result> bool HasSuccess(const Result &Value) {
  return Value.IsSuccess() && Value.Diagnostic() == nullptr;
}

template <class Result> bool HasInvariant(const Result &Value) {
  if (Value.IsSuccess())
    return Value.Diagnostic() == nullptr;
  return Value.Diagnostic() != nullptr &&
         !Value.Diagnostic()->Message().empty();
}

static_assert(std::is_copy_constructible_v<Luna::ErrorDiagnostic>);
static_assert(std::is_copy_assignable_v<Luna::ErrorDiagnostic>);
static_assert(std::is_move_constructible_v<Luna::ErrorDiagnostic>);
static_assert(std::is_move_assignable_v<Luna::ErrorDiagnostic>);

static_assert(std::is_copy_constructible_v<Luna::ExecutionResult>);
static_assert(std::is_copy_assignable_v<Luna::ExecutionResult>);
static_assert(std::is_move_constructible_v<Luna::ExecutionResult>);
static_assert(std::is_move_assignable_v<Luna::ExecutionResult>);
static_assert(!std::is_default_constructible_v<Luna::ExecutionResult>);
static_assert(
    !std::is_constructible_v<Luna::ExecutionResult, Luna::ErrorDiagnostic>);

static_assert(std::is_copy_constructible_v<Luna::RegistrationResult>);
static_assert(std::is_copy_assignable_v<Luna::RegistrationResult>);
static_assert(std::is_move_constructible_v<Luna::RegistrationResult>);
static_assert(std::is_move_assignable_v<Luna::RegistrationResult>);
static_assert(!std::is_default_constructible_v<Luna::RegistrationResult>);
static_assert(
    !std::is_constructible_v<Luna::RegistrationResult, Luna::ErrorDiagnostic>);
} // namespace

constexpr std::array ErrorCategories{
    Luna::ErrorCategory::StateNotReady,
    Luna::ErrorCategory::InvalidGlobalName,
    Luna::ErrorCategory::DuplicateGlobalName,
    Luna::ErrorCategory::NullCallable,
    Luna::ErrorCategory::Compilation,
    Luna::ErrorCategory::Runtime,
    Luna::ErrorCategory::Internal,
};

template <class Result> bool VerifyResultInvariant() {
  auto Success = Result::Success();
  auto CopiedSuccess = Success;
  auto MovedSuccess = std::move(CopiedSuccess);
  if (!HasSuccess(Success) || !HasInvariant(CopiedSuccess) ||
      !HasSuccess(MovedSuccess))
    return false;

  std::string SourceMessage = "original runtime failure";
  auto Failure = Result::Failure(Luna::ErrorCategory::Runtime, SourceMessage);
  SourceMessage = "changed";

  auto CopiedFailure = Failure;
  auto MovedFailure = std::move(Failure);
  auto AssignedFailure = Result::Success();
  AssignedFailure = CopiedFailure;
  auto MoveAssignedFailure = Result::Success();
  MoveAssignedFailure = std::move(CopiedFailure);

  constexpr auto Runtime = Luna::ErrorCategory::Runtime;
  constexpr auto RuntimeMessage = "original runtime failure";
  if (!HasInvariant(Failure) || !HasInvariant(CopiedFailure) ||
      !HasFailure(MovedFailure, Runtime, RuntimeMessage) ||
      !HasFailure(AssignedFailure, Runtime, RuntimeMessage) ||
      !HasFailure(MoveAssignedFailure, Runtime, RuntimeMessage))
    return false;

  AssignedFailure = Result::Success();
  MoveAssignedFailure = Result::Success();
  if (!HasSuccess(AssignedFailure) || !HasSuccess(MoveAssignedFailure))
    return false;

  auto OwnedDiagnostic = Luna::ErrorDiagnostic::Create(
      Luna::ErrorCategory::InvalidGlobalName, "invalid byte sequence");
  auto DiagnosticFailure = Result::Failure(OwnedDiagnostic);
  OwnedDiagnostic = Luna::ErrorDiagnostic::Create(Luna::ErrorCategory::Internal,
                                                  "replacement diagnostic");
  if (!HasFailure(DiagnosticFailure, Luna::ErrorCategory::InvalidGlobalName,
                  "invalid byte sequence"))
    return false;

  for (const auto Category : ErrorCategories) {
    const auto FallbackDiagnostic = Luna::ErrorDiagnostic::Create(Category, {});
    const auto FallbackFailure = Result::Failure(Category, {});
    if (FallbackDiagnostic.Category() != Category ||
        FallbackDiagnostic.Message().empty() || FallbackFailure.IsSuccess() ||
        FallbackFailure.Diagnostic() == nullptr ||
        FallbackFailure.Diagnostic()->Category() != Category ||
        FallbackFailure.Diagnostic()->Message().empty())
      return false;
  }

  return true;
}

int RunResultTypesTests() {
  if (!VerifyResultInvariant<Luna::ExecutionResult>())
    return 1;
  if (!VerifyResultInvariant<Luna::RegistrationResult>())
    return 2;

  auto OriginalDiagnostic = Luna::ErrorDiagnostic::Create(
      Luna::ErrorCategory::Compilation, "owned compiler diagnostic");
  auto CopiedDiagnostic = OriginalDiagnostic;
  auto MovedDiagnostic = std::move(OriginalDiagnostic);
  if (OriginalDiagnostic.Message().empty() ||
      CopiedDiagnostic.Message() != "owned compiler diagnostic" ||
      MovedDiagnostic.Message() != "owned compiler diagnostic")
    return 3;

  return 0;
}
