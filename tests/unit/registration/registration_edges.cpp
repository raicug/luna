// clang-format off
#include <luna/luna.hpp>

#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <initializer_list>
#include <string>
#include <string_view>
// clang-format on

namespace {

using FaultPoint = Luna::Detail::StateFaultPoint;
using Hooks = Luna::Detail::StateTestHooks;

[[nodiscard]] bool
HasFailure(const Luna::RegistrationResult &Result, Luna::ErrorCategory Category,
           std::initializer_list<std::string_view> Contexts) {
  if (Result.IsSuccess() || !Result.Diagnostic() ||
      Result.Diagnostic()->Category() != Category ||
      Result.Diagnostic()->Message().empty())
    return false;

  for (const std::string_view Context : Contexts) {
    if (Result.Diagnostic()->Message().find(Context) == std::string::npos)
      return false;
  }
  return true;
}

[[nodiscard]] bool TestReadyAndNonReadyStates() {
  Hooks::ResetLifecycle();
  Hooks::FailNextCreations();
  Luna::State NotReady;
  if (NotReady.IsReady())
    return false;

  const auto Rejected = NotReady.Bindings().Register("NonReadyGlobal", []() {});
  if (!HasFailure(Rejected, Luna::ErrorCategory::StateNotReady,
                  {"NonReadyGlobal", "not ready"}) ||
      Hooks::BindingCount(NotReady) != 0)
    return false;

  Luna::State Ready;
  int Calls = 0;
  const auto Registered =
      Ready.Bindings().Register("ReadyGlobal", [&Calls]() { ++Calls; });
  return Ready.IsReady() && Registered.IsSuccess() &&
         Registered.Diagnostic() == nullptr &&
         Ready.Execute("ReadyGlobal()").IsSuccess() && Calls == 1;
}

[[nodiscard]] bool TestInvalidNamePartitions() {
  Luna::State State;
  if (!State.IsReady())
    return false;

  const std::string Overlong(256, 'A');
  std::string NonAscii = "Name";
  NonAscii.push_back(static_cast<char>(0x80));
  const std::string EmbeddedZero("Good\0Name", 9);

  const auto Empty = State.Bindings().Register("", []() {});
  const auto TooLong = State.Bindings().Register(Overlong, []() {});
  const auto NonAsciiByte = State.Bindings().Register(NonAscii, []() {});
  const auto IllegalFirst = State.Bindings().Register("9Name", []() {});
  const auto IllegalLater = State.Bindings().Register("Bad-Name", []() {});
  const auto ZeroByte = State.Bindings().Register(EmbeddedZero, []() {});

  return HasFailure(Empty, Luna::ErrorCategory::InvalidGlobalName,
                    {"empty", "1 to 255 ASCII bytes"}) &&
         HasFailure(TooLong, Luna::ErrorCategory::InvalidGlobalName,
                    {"256 bytes", "maximum is 255"}) &&
         HasFailure(NonAsciiByte, Luna::ErrorCategory::InvalidGlobalName,
                    {"byte 5", "non-ASCII", "0x80"}) &&
         HasFailure(IllegalFirst, Luna::ErrorCategory::InvalidGlobalName,
                    {"first byte", "0x39"}) &&
         HasFailure(IllegalLater, Luna::ErrorCategory::InvalidGlobalName,
                    {"byte 4", "0x2D"}) &&
         HasFailure(ZeroByte, Luna::ErrorCategory::InvalidGlobalName,
                    {"byte 5", "0x00"}) &&
         Hooks::BindingCount(State) == 0;
}

[[nodiscard]] bool TestNullFunctionPointer() {
  Luna::State State;
  int (*NullTarget)(int) = nullptr;
  const auto Result = State.Bindings().Register("NullTarget", NullTarget);

  return State.IsReady() &&
         HasFailure(Result, Luna::ErrorCategory::NullCallable,
                    {"NullTarget", "callable target is null"}) &&
         Hooks::BindingCount(State) == 0;
}

[[nodiscard]] bool TestDuplicatePreservesOriginalCallable() {
  Luna::State State;
  int OriginalCalls = 0;
  int ReplacementCalls = 0;

  const auto Original =
      State.Bindings().Register("Preserved", [&OriginalCalls]() {
        ++OriginalCalls;
        return 17;
      });
  const auto OriginalAddress = Hooks::BindingRecordAddress(State, "Preserved");
  const auto Duplicate =
      State.Bindings().Register("Preserved", [&ReplacementCalls]() {
        ++ReplacementCalls;
        return 99;
      });

  const auto Execution = State.Execute("ObservedPreserved = Preserved()");
  return Original.IsSuccess() && OriginalAddress &&
         HasFailure(Duplicate, Luna::ErrorCategory::DuplicateGlobalName,
                    {"Preserved", "already registered"}) &&
         Hooks::BindingCount(State) == 1 &&
         Hooks::BindingRecordAddress(State, "Preserved") == OriginalAddress &&
         Execution.IsSuccess() &&
         Hooks::ObserveIntegerGlobal(State, "ObservedPreserved") == 17 &&
         OriginalCalls == 1 && ReplacementCalls == 0;
}
[[nodiscard]] bool TestRegistrationErrorPrecedence() {
  Hooks::ResetLifecycle();
  Hooks::FailNextCreations();
  Luna::State NotReady;
  int (*NullTarget)() = nullptr;

  const auto InvalidDominates =
      NotReady.Bindings().Register("Bad.Name", NullTarget);
  const auto ReadinessDominates =
      NotReady.Bindings().Register("ValidName", NullTarget);
  if (!HasFailure(InvalidDominates, Luna::ErrorCategory::InvalidGlobalName,
                  {"byte 4", "0x2E"}) ||
      !HasFailure(ReadinessDominates, Luna::ErrorCategory::StateNotReady,
                  {"ValidName", "State is not ready"}) ||
      Hooks::BindingCount(NotReady) != 0)
    return false;

  Luna::State Ready;
  int (*OriginalTarget)() = []() { return 23; };
  const auto Original =
      Ready.Bindings().Register("PrecedenceGlobal", OriginalTarget);
  if (!Ready.IsReady() || !Original.IsSuccess() || Original.Diagnostic())
    return false;

  const auto NullDominatesDuplicate =
      Ready.Bindings().Register("PrecedenceGlobal", NullTarget);
  const auto Duplicate =
      Ready.Bindings().Register("PrecedenceGlobal", []() { return 99; });
  const auto Execution =
      Ready.Execute("ObservedPrecedence = PrecedenceGlobal()");

  return HasFailure(NullDominatesDuplicate, Luna::ErrorCategory::NullCallable,
                    {"PrecedenceGlobal", "callable target is null"}) &&
         HasFailure(Duplicate, Luna::ErrorCategory::DuplicateGlobalName,
                    {"PrecedenceGlobal", "already registered"}) &&
         Hooks::BindingCount(Ready) == 1 && Execution.IsSuccess() &&
         Hooks::ObserveIntegerGlobal(Ready, "ObservedPrecedence") == 23;
}

[[nodiscard]] bool TestInstallationRollbackPreservesExistingValues() {
  Luna::State State;
  int SurvivorCalls = 0;

  const auto Survivor =
      State.Bindings().Register("Survivor", [&SurvivorCalls]() {
        ++SurvivorCalls;
        return 41;
      });
  const auto SurvivorAddress = Hooks::BindingRecordAddress(State, "Survivor");
  if (!Survivor.IsSuccess() || !SurvivorAddress ||
      !Hooks::SetIntegerGlobal(State, "InstallCandidate", 73))
    return false;

  Hooks::InjectFault(State, FaultPoint::BindingInstallation);
  const auto FailedInstallation =
      State.Bindings().Register("InstallCandidate", []() { return 5; });

  if (!HasFailure(FailedInstallation, Luna::ErrorCategory::Internal,
                  {"InstallCandidate", "installation failed", "rolled back"}) ||
      Hooks::PendingFaults(State, FaultPoint::BindingInstallation) != 0 ||
      Hooks::BindingCount(State) != 1 ||
      Hooks::PendingBindingCount(State) != 0 ||
      Hooks::BindingRecordAddress(State, "InstallCandidate") ||
      Hooks::BindingRecordAddress(State, "Survivor") != SurvivorAddress ||
      Hooks::ObserveIntegerGlobal(State, "InstallCandidate") != 73)
    return false;

  const auto Preserved =
      State.Execute("ObservedSurvivor = Survivor()\n"
                    "ObservedInstallCandidate = InstallCandidate");
  if (!Preserved.IsSuccess() || SurvivorCalls != 1 ||
      Hooks::ObserveIntegerGlobal(State, "ObservedSurvivor") != 41 ||
      Hooks::ObserveIntegerGlobal(State, "ObservedInstallCandidate") != 73)
    return false;

  const auto Retried =
      State.Bindings().Register("InstallCandidate", []() { return 5; });
  const auto RetriedExecution =
      State.Execute("ObservedRetriedCandidate = InstallCandidate()");
  return Retried.IsSuccess() && Hooks::BindingCount(State) == 2 &&
         Hooks::PendingBindingCount(State) == 0 &&
         RetriedExecution.IsSuccess() &&
         Hooks::ObserveIntegerGlobal(State, "ObservedRetriedCandidate") == 5;
}

} // namespace

int RunRegistrationExamplesAndEdgeCaseTests() {
  if (!TestReadyAndNonReadyStates())
    return 1;
  if (!TestInvalidNamePartitions())
    return 2;
  if (!TestNullFunctionPointer())
    return 3;
  if (!TestDuplicatePreservesOriginalCallable())
    return 4;
  if (!TestRegistrationErrorPrecedence())
    return 5;
  if (!TestInstallationRollbackPreservesExistingValues())
    return 6;
  return 0;
}
