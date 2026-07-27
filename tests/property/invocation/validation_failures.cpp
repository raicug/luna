// clang-format off
#include <luna/luna.hpp>

#include "state/invocation/testing/test_hooks.hpp"
#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <rapidcheck.h>

#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace {

constexpr std::string_view GlobalName = "InternalValidation";

using FaultPoint = Luna::Detail::StateFaultPoint;
using PrimitiveHooks = Luna::Detail::InvocationPrimitiveTestHooks;
using StateHooks = Luna::Detail::StateTestHooks;
using ValidationState = Luna::Detail::InvocationValidationState;

[[nodiscard]] bool Contains(std::string_view Text, std::string_view Context) {
  return Text.find(Context) != std::string_view::npos;
}

[[nodiscard]] bool RecordsInternalError(FaultPoint Fault, int Argument) {
  const Luna::CallableMetadata Metadata(
      {Luna::ValueKind::Integer},
      Luna::ReturnMetadata::ForValue(Luna::ValueKind::Integer));
  const auto Observation = PrimitiveHooks::Validate(
      {Luna::Detail::InvocationTestValue{Argument}}, std::string(GlobalName),
      &Metadata, Fault == FaultPoint::MissingMetadata ? 1U : 0U,
      Fault == FaultPoint::ArgumentInspection ? 1U : 0U);
  const auto *Diagnostic = Observation.Invocation.Validation.Diagnostic();

  return Observation.Invocation.Validation.State() ==
             ValidationState::InternalError &&
         !Observation.Invocation.Validation.IsSuccess() &&
         Observation.Invocation.Arguments.empty() && Diagnostic != nullptr &&
         Diagnostic->Category() == Luna::ErrorCategory::Internal &&
         Contains(Diagnostic->Message(), GlobalName) &&
         Observation.PendingMissingMetadataFaults == 0 &&
         Observation.PendingArgumentInspectionFaults == 0;
}
[[nodiscard]] bool
IsProtectedInternalFailure(const Luna::ExecutionResult &Execution) {
  const auto *Diagnostic = Execution.Diagnostic();
  return !Execution.IsSuccess() && Diagnostic != nullptr &&
         Diagnostic->Category() == Luna::ErrorCategory::Runtime &&
         Contains(Diagnostic->Message(), GlobalName) &&
         Contains(Diagnostic->Message(), "Internal error");
}

} // namespace

int RunInternalValidationFailureProperties() {
  // clang-format off
  const bool Passed = rc::check(
      // clang-format on
      "Internal validation failure never invokes user code", [](int Argument) {
        RC_ASSERT(RecordsInternalError(FaultPoint::MissingMetadata, Argument));
        RC_ASSERT(
            RecordsInternalError(FaultPoint::ArgumentInspection, Argument));

        Luna::State State;
        RC_ASSERT(State.IsReady());

        int Calls = 0;
        const auto Registration =
            State.Bindings().Register(GlobalName, [&Calls](int Value) {
              ++Calls;
              return Value;
            });
        RC_ASSERT(Registration.IsSuccess());

        const std::string Source =
            std::string(GlobalName) + "(" + std::to_string(Argument) + ")";

        StateHooks::InjectFault(State, FaultPoint::MissingMetadata);
        const auto MissingMetadata = State.Execute(Source);
        RC_ASSERT(IsProtectedInternalFailure(MissingMetadata));
        RC_ASSERT(Calls == 0);

        StateHooks::InjectFault(State, FaultPoint::ArgumentInspection);
        const auto ArgumentInspection = State.Execute(Source);
        RC_ASSERT(IsProtectedInternalFailure(ArgumentInspection));
        RC_ASSERT(Calls == 0);
      });

  return Passed ? 0 : 1;
}
