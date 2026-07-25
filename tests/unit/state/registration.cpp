// clang-format off
#include <luna/luna.hpp>

#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <cstddef>
#include <string>
#include <string_view>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using FaultPoint = Luna::Detail::StateFaultPoint;

[[nodiscard]] bool HasFailure(const Luna::RegistrationResult &Result,
                              Luna::ErrorCategory Category,
                              std::string_view Context) {
  return !Result.IsSuccess() && Result.Diagnostic() &&
         Result.Diagnostic()->Category() == Category &&
         Result.Diagnostic()->Message().find(Context) != std::string::npos;
}

[[nodiscard]] bool HasStableClosure(const Luna::State &State,
                                    std::string_view Name) {
  const auto Record = Hooks::BindingRecordAddress(State, Name);
  return Record && Hooks::BindingIsCommitted(State, Name) &&
         Hooks::InstalledBindingRecordAddress(State, Name) == Record;
}

} // namespace

int RunRegistrationTransactionTests() {
  Luna::State Primary;
  Luna::State Isolated;
  if (!Primary.IsReady() || !Isolated.IsReady())
    return 1;

  if (!Hooks::SetRootStackDepth(Primary, 3))
    return 2;
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Primary);

  int InvocationCount = 0;
  const auto First = Primary.Bindings().Register(
      "First", [&InvocationCount]() { ++InvocationCount; });
  if (!First.IsSuccess() || Hooks::BindingCount(Primary) != 1 ||
      Hooks::PendingBindingCount(Primary) != 0 ||
      !HasStableClosure(Primary, "First") ||
      Hooks::ObserveRootStackDepth(Primary) != EntryDepth ||
      InvocationCount != 0)
    return 3;

  const auto FirstAddress = Hooks::BindingRecordAddress(Primary, "First");

  for (std::size_t Index = 0; Index < 96; ++Index) {
    const std::string Name = "Rehash" + std::to_string(Index);
    const auto Result =
        Primary.Bindings().Register(Name, [Index]() { return int(Index); });
    if (!Result.IsSuccess())
      return 4;
  }
  if (Hooks::BindingRecordAddress(Primary, "First") != FirstAddress ||
      Hooks::InstalledBindingRecordAddress(Primary, "First") != FirstAddress ||
      Hooks::ObserveRootStackDepth(Primary) != EntryDepth)
    return 5;

  const std::size_t CommittedCount = Hooks::BindingCount(Primary);
  const auto Duplicate =
      Primary.Bindings().Register("First", []() { return 999; });
  if (!HasFailure(Duplicate, Luna::ErrorCategory::DuplicateGlobalName,
                  "First") ||
      Hooks::BindingCount(Primary) != CommittedCount ||
      Hooks::BindingRecordAddress(Primary, "First") != FirstAddress ||
      Hooks::InstalledBindingRecordAddress(Primary, "First") != FirstAddress ||
      Hooks::ObserveRootStackDepth(Primary) != EntryDepth)
    return 6;

  Hooks::InjectFault(Primary, FaultPoint::BindingRecordAllocation);
  const auto AllocationFailure =
      Primary.Bindings().Register("AllocationFailure", []() {});
  if (!HasFailure(AllocationFailure, Luna::ErrorCategory::Internal,
                  "AllocationFailure") ||
      Hooks::PendingFaults(Primary, FaultPoint::BindingRecordAllocation) != 0 ||
      Hooks::BindingCount(Primary) != CommittedCount ||
      Hooks::PendingBindingCount(Primary) != 0 ||
      Hooks::BindingRecordAddress(Primary, "AllocationFailure") ||
      Hooks::ObserveRootStackDepth(Primary) != EntryDepth)
    return 7;

  const std::string ExistingName = "ExistingGlobal";
  if (!Hooks::SetIntegerGlobal(Primary, ExistingName, 73) ||
      Hooks::ObserveIntegerGlobal(Primary, ExistingName) != 73)
    return 8;

  Hooks::InjectFault(Primary, FaultPoint::BindingInstallation);
  const auto InstallationFailure =
      Primary.Bindings().Register(ExistingName, []() { return 5; });

  if (!HasFailure(InstallationFailure, Luna::ErrorCategory::Internal,
                  "rolled back") ||
      Hooks::PendingFaults(Primary, FaultPoint::BindingInstallation) != 0 ||
      Hooks::BindingCount(Primary) != CommittedCount ||
      Hooks::PendingBindingCount(Primary) != 0 ||
      Hooks::BindingRecordAddress(Primary, ExistingName) ||
      Hooks::ObserveIntegerGlobal(Primary, ExistingName) != 73 ||
      Hooks::BindingRecordAddress(Primary, "First") != FirstAddress ||
      Hooks::InstalledBindingRecordAddress(Primary, "First") != FirstAddress ||
      Hooks::ObserveRootStackDepth(Primary) != EntryDepth)
    return 9;

  const auto Replacement =
      Primary.Bindings().Register(ExistingName, []() { return 5; });
  if (!Replacement.IsSuccess() ||
      Hooks::BindingCount(Primary) != CommittedCount + 1 ||
      !HasStableClosure(Primary, ExistingName) ||
      Hooks::ObserveIntegerGlobal(Primary, ExistingName) ||
      Hooks::ObserveRootStackDepth(Primary) != EntryDepth)
    return 10;

  const auto IsolatedRegistration =
      Isolated.Bindings().Register("First", []() { return 42; });
  const auto IsolatedAddress = Hooks::BindingRecordAddress(Isolated, "First");
  if (!IsolatedRegistration.IsSuccess() || !IsolatedAddress ||
      !HasStableClosure(Isolated, "First") || IsolatedAddress == FirstAddress ||
      Hooks::BindingCount(Isolated) != 1 ||
      Hooks::BindingRecordAddress(Primary, "First") != FirstAddress)
    return 11;

  return 0;
}
