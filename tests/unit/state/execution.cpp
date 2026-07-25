// clang-format off
#include <luna/luna.hpp>

#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
// clang-format on

namespace {

using FaultPoint = Luna::Detail::StateFaultPoint;
using Hooks = Luna::Detail::StateTestHooks;

[[nodiscard]] bool
HasFailure(const Luna::ExecutionResult &Result, Luna::ErrorCategory Category,
           std::string_view Prefix,
           std::initializer_list<std::string_view> Contexts = {}) {
  const auto *Diagnostic = Result.Diagnostic();
  if (Result.IsSuccess() || !Diagnostic || Diagnostic->Category() != Category ||
      !Diagnostic->Message().starts_with(Prefix))
    return false;

  for (const std::string_view Context : Contexts) {
    if (Diagnostic->Message().find(Context) == std::string::npos)
      return false;
  }
  return true;
}

[[nodiscard]] bool HasExactFailure(const Luna::ExecutionResult &Result,
                                   Luna::ErrorCategory Category,
                                   std::string_view Message) {
  const auto *Diagnostic = Result.Diagnostic();
  return !Result.IsSuccess() && Diagnostic &&
         Diagnostic->Category() == Category && Diagnostic->Message() == Message;
}

[[nodiscard]] bool IsDepth(const Luna::State &State, int Expected) {
  return Hooks::ObserveRootStackDepth(State) == Expected;
}

[[nodiscard]] bool TestNonReadyRejection() {
  Hooks::ResetLifecycle();
  Hooks::FailNextCreations();
  Luna::State State;
  Hooks::InjectFault(State, FaultPoint::ExecutionThreadCreation);
  const auto Result = State.Execute("this is not valid Luau source !");
  return !State.IsReady() &&
         HasFailure(Result, Luna::ErrorCategory::StateNotReady,
                    "State not ready:", {"ready State"}) &&
         Hooks::PendingFaults(State, FaultPoint::ExecutionThreadCreation) == 1;
}

[[nodiscard]] bool TestSuccessAndZeroChunkResults() {
  Luna::State State;
  int Calls = 0;
  if (!State.IsReady() || !Hooks::SetRootStackDepth(State, 3) ||
      !State.Bindings()
           .Register("Observe", [&Calls](int Value) { Calls += Value; })
           .IsSuccess())
    return false;

  constexpr std::string_view ValidSource = "Observe(7)\nreturn 1, 2, 3";
  std::string SourceBuffer(ValidSource);
  SourceBuffer += "\nlocal =";
  const auto Result =
      State.Execute(std::string_view(SourceBuffer.data(), ValidSource.size()));
  return Result.IsSuccess() && !Result.Diagnostic() && Calls == 7 &&
         IsDepth(State, 3);
}

[[nodiscard]] bool TestCompilationFailureAndRecovery() {
  Luna::State State;
  int Calls = 0;
  if (!State.IsReady() || !Hooks::SetRootStackDepth(State, 4) ||
      !State.Bindings()
           .Register("StillRegistered", [&Calls]() { ++Calls; })
           .IsSuccess())
    return false;

  const auto Failed = State.Execute("local =");
  if (!HasFailure(Failed, Luna::ErrorCategory::Compilation,
                  "Compilation error:") ||
      Calls != 0 || !IsDepth(State, 4))
    return false;

  const auto Recovered = State.Execute("StillRegistered()");
  return Recovered.IsSuccess() && !Recovered.Diagnostic() && Calls == 1 &&
         IsDepth(State, 4);
}

[[nodiscard]] bool TestRuntimeAndNativeFailureRecovery() {
  Luna::State State;
  int SignalCalls = 0;
  int IntegerTotal = 0;
  if (!State.IsReady() || !Hooks::SetRootStackDepth(State, 2) ||
      !State.Bindings()
           .Register("CommittedSignal", [&SignalCalls]() { ++SignalCalls; })
           .IsSuccess() ||
      !State.Bindings()
           .Register("NeedsInteger",
                     [&IntegerTotal](int Value) { IntegerTotal += Value; })
           .IsSuccess() ||
      !State.Bindings()
           .Register("StandardThrow",
                     []() -> int {
                       throw std::runtime_error("execution exception detail");
                     })
           .IsSuccess())
    return false;

  int ExpectedRecoveries = 0;
  const auto RecoverCommittedRegistrations = [&]() {
    const auto Recovered = State.Execute("CommittedSignal()\nNeedsInteger(9)");
    ++ExpectedRecoveries;
    return Recovered.IsSuccess() && !Recovered.Diagnostic() &&
           SignalCalls == ExpectedRecoveries &&
           IntegerTotal == ExpectedRecoveries * 9 && IsDepth(State, 2);
  };

  const auto ExplicitFailure = State.Execute("local Missing = nil\nMissing()");
  if (!HasFailure(ExplicitFailure, Luna::ErrorCategory::Runtime,
                  "Runtime error:", {"attempt to call"}) ||
      !RecoverCommittedRegistrations())
    return false;

  Hooks::InjectFault(State, FaultPoint::ExecutionErrorDiagnostic);
  const auto FallbackFailure = State.Execute("local Missing = nil\nMissing()");
  if (!HasExactFailure(
          FallbackFailure, Luna::ErrorCategory::Runtime,
          "Runtime error: execution failed without a Luau diagnostic.") ||
      Hooks::PendingFaults(State, FaultPoint::ExecutionErrorDiagnostic) != 0 ||
      !RecoverCommittedRegistrations())
    return false;

  const auto ValidationFailure = State.Execute("NeedsInteger('wrong')");
  if (!HasFailure(ValidationFailure, Luna::ErrorCategory::Runtime,
                  "Runtime error:",
                  {"NeedsInteger", "argument 1",
                   "expected signed 32-bit integer", "received string"}) ||
      SignalCalls != ExpectedRecoveries ||
      IntegerTotal != ExpectedRecoveries * 9 ||
      !RecoverCommittedRegistrations())
    return false;

  const auto ExceptionFailure = State.Execute("StandardThrow()");
  if (!HasFailure(
          ExceptionFailure, Luna::ErrorCategory::Runtime,
          "Runtime error:", {"StandardThrow", "execution exception detail"}) ||
      !RecoverCommittedRegistrations())
    return false;

  return Hooks::BindingCount(State) == 3 &&
         Hooks::BindingIsCommitted(State, "CommittedSignal") &&
         Hooks::BindingIsCommitted(State, "NeedsInteger") &&
         Hooks::BindingIsCommitted(State, "StandardThrow");
}

[[nodiscard]] bool TestInternalThreadFailureAndRecovery() {
  Luna::State State;
  int Calls = 0;
  if (!State.IsReady() || !Hooks::SetRootStackDepth(State, 5) ||
      !State.Bindings()
           .Register("AfterInternal", [&Calls]() { ++Calls; })
           .IsSuccess())
    return false;

  Hooks::InjectFault(State, FaultPoint::ExecutionThreadCreation);
  const auto Failed = State.Execute("AfterInternal()");
  if (!HasExactFailure(
          Failed, Luna::ErrorCategory::Internal,
          "Internal error: could not create disposable execution thread.") ||
      Calls != 0 || !IsDepth(State, 5) ||
      Hooks::PendingFaults(State, FaultPoint::ExecutionThreadCreation) != 0)
    return false;

  const auto Recovered = State.Execute("AfterInternal()");
  return Recovered.IsSuccess() && !Recovered.Diagnostic() && Calls == 1 &&
         Hooks::BindingCount(State) == 1 &&
         Hooks::BindingIsCommitted(State, "AfterInternal") && IsDepth(State, 5);
}

} // namespace

int RunSourceExecutionTests() {
  if (!TestNonReadyRejection())
    return 1;
  if (!TestSuccessAndZeroChunkResults())
    return 2;
  if (!TestCompilationFailureAndRecovery())
    return 3;
  if (!TestRuntimeAndNativeFailureRecovery())
    return 4;
  if (!TestInternalThreadFailureAndRecovery())
    return 5;
  return 0;
}
