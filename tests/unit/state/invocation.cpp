// clang-format off
#include <luna/luna.hpp>

#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace {

using FaultPoint = Luna::Detail::StateFaultPoint;
using Hooks = Luna::Detail::StateTestHooks;
using Observation = Luna::Detail::NativeInvocationObservation;

[[nodiscard]] bool Contains(const Observation &Result, std::string_view Text) {
  return Result.ErrorMessage.find(Text) != std::string::npos;
}

[[nodiscard]] bool IsBalancedFailure(const Observation &Result) {
  return !Result.Succeeded && Result.ReturnCount == 0 &&
         !Result.ErrorMessage.empty() &&
         Result.CompletionStackDepth == Result.EntryStackDepth + 1 &&
         Result.FinalStackDepth == Result.EntryStackDepth;
}

[[nodiscard]] bool TestValidationOrderAndInvocation() {
  Luna::State State;
  if (!State.IsReady() || !Hooks::SetRootStackDepth(State, 4))
    return false;

  int Calls = 0;
  const auto Registered =
      State.Bindings().Register("Ordered", [&Calls](int Left, int Right) {
        ++Calls;
        return Left + Right;
      });
  if (!Registered.IsSuccess())
    return false;

  Hooks::InjectFault(State, FaultPoint::ArgumentInspection);
  const auto CountFailure = Hooks::InvokeBinding(State, "Ordered", {1});
  if (!IsBalancedFailure(CountFailure) || !Contains(CountFailure, "Ordered") ||
      !Contains(CountFailure, "expected 2") ||
      !Contains(CountFailure, "received 1") || Calls != 0 ||
      Hooks::PendingFaults(State, FaultPoint::ArgumentInspection) != 1)
    return false;

  if (!Hooks::ConsumeFault(State, FaultPoint::ArgumentInspection))
    return false;
  const auto FirstMismatch = Hooks::InvokeBinding(
      State, "Ordered", {std::string("first"), std::string("second")});
  if (!IsBalancedFailure(FirstMismatch) ||
      !Contains(FirstMismatch, "argument 1") ||
      Contains(FirstMismatch, "argument 2") || Calls != 0)
    return false;
  const auto Success = Hooks::InvokeBinding(State, "Ordered", {19, 23});
  return Success.Succeeded && Success.ReturnCount == 1 &&
         Success.ReturnedValue && std::get<int>(*Success.ReturnedValue) == 42 &&
         Calls == 1 &&
         Success.CompletionStackDepth == Success.EntryStackDepth + 1 &&
         Success.FinalStackDepth == Success.EntryStackDepth;
}

[[nodiscard]] bool TestZeroAndOneReturnModes() {
  Luna::State State;
  if (!State.IsReady())
    return false;

  int VoidCalls = 0;
  if (!State.Bindings()
           .Register("NoResult", [&VoidCalls]() { ++VoidCalls; })
           .IsSuccess() ||
      !State.Bindings()
           .Register("OneResult", []() { return true; })
           .IsSuccess())
    return false;

  const auto NoResult = Hooks::InvokeBinding(State, "NoResult", {});
  const auto OneResult = Hooks::InvokeBinding(State, "OneResult", {});
  return NoResult.Succeeded && NoResult.ReturnCount == 0 &&
         NoResult.CompletionStackDepth == NoResult.EntryStackDepth &&
         NoResult.FinalStackDepth == NoResult.EntryStackDepth &&
         VoidCalls == 1 && OneResult.Succeeded && OneResult.ReturnCount == 1 &&
         OneResult.ReturnedValue && std::get<bool>(*OneResult.ReturnedValue);
}

[[nodiscard]] bool TestReturnFailureTranslation() {
  Luna::State State;
  if (!State.IsReady() || !Hooks::SetRootStackDepth(State, 3))
    return false;

  int ValueCalls = 0;
  int VoidCalls = 0;
  if (!State.Bindings()
           .Register("WriterFailure",
                     [&ValueCalls]() {
                       ++ValueCalls;
                       return 7;
                     })
           .IsSuccess() ||
      !State.Bindings()
           .Register("VoidFailure", [&VoidCalls]() { ++VoidCalls; })
           .IsSuccess())
    return false;

  Hooks::InjectFault(State, FaultPoint::ReturnWrite);
  const auto Writer = Hooks::InvokeBinding(State, "WriterFailure", {});
  Hooks::InjectFault(State, FaultPoint::VoidFinalization);
  const auto Void = Hooks::InvokeBinding(State, "VoidFailure", {});

  return IsBalancedFailure(Writer) && Contains(Writer, "WriterFailure") &&
         Contains(Writer, "return-writer") && ValueCalls == 1 &&
         IsBalancedFailure(Void) && Contains(Void, "VoidFailure") &&
         Contains(Void, "void-finalization") && VoidCalls == 1;
}

[[nodiscard]] bool TestExceptionAndInternalFailureTranslation() {
  Luna::State State;
  if (!State.IsReady() || !Hooks::SetRootStackDepth(State, 2))
    return false;

  int Destroyed = 0;
  int OrderedCalls = 0;
  if (!State.Bindings()
           .Register("StandardThrow",
                     [&Destroyed]() -> int {
                       struct Guard final {
                         int *Count;
                         ~Guard() { ++*Count; }
                       } Lifetime{&Destroyed};
                       throw std::runtime_error("standard detail");
                     })
           .IsSuccess() ||
      !State.Bindings()
           .Register("UnknownThrow", []() -> int { throw 17; })
           .IsSuccess() ||
      !State.Bindings()
           .Register("InternalValidation",
                     [&OrderedCalls](int Value) {
                       ++OrderedCalls;
                       return Value;
                     })
           .IsSuccess())
    return false;

  const auto Standard = Hooks::InvokeBinding(State, "StandardThrow", {});
  const auto Unknown = Hooks::InvokeBinding(State, "UnknownThrow", {});
  Hooks::InjectFault(State, FaultPoint::MissingMetadata);
  const auto Missing = Hooks::InvokeBinding(State, "InternalValidation", {9});
  Hooks::InjectFault(State, FaultPoint::ArgumentInspection);
  const auto Inspection =
      Hooks::InvokeBinding(State, "InternalValidation", {9});

  return IsBalancedFailure(Standard) && Contains(Standard, "StandardThrow") &&
         Contains(Standard, "standard detail") && Destroyed == 1 &&
         IsBalancedFailure(Unknown) && Contains(Unknown, "UnknownThrow") &&
         Contains(Unknown, "unknown C++ exception") &&
         IsBalancedFailure(Missing) && Contains(Missing, "metadata") &&
         IsBalancedFailure(Inspection) && Contains(Inspection, "inspecting") &&
         OrderedCalls == 0;
}

} // namespace

int RunNativeTrampolineTests() {
  if (!TestValidationOrderAndInvocation())
    return 1;
  if (!TestZeroAndOneReturnModes())
    return 2;
  if (!TestReturnFailureTranslation())
    return 3;
  if (!TestExceptionAndInternalFailureTranslation())
    return 4;
  return 0;
}
