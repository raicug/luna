// clang-format off
#include <luna/luna.hpp>

#include "state/binding/registration_checks.hpp"
#include "state/testing/test_hooks.hpp"

#include <optional>
#include <string>
#include <string_view>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

[[nodiscard]] bool
HasFailure(const std::optional<Luna::ErrorDiagnostic> &Diagnostic,
           Luna::ErrorCategory Category, std::string_view Context) {
  return Diagnostic && Diagnostic->Category() == Category &&
         !Diagnostic->Message().empty() &&
         Diagnostic->Message().find(Context) != std::string::npos;
}

[[nodiscard]] bool HasFailure(const Luna::RegistrationResult &Result,
                              Luna::ErrorCategory Category,
                              std::string_view Context) {
  return !Result.IsSuccess() && Result.Diagnostic() &&
         Result.Diagnostic()->Category() == Category &&
         Result.Diagnostic()->Message().find(Context) != std::string::npos;
}

} // namespace

int RunRegistrationPrecheckTests() {
  using Luna::Detail::CheckRegistrationPreconditions;

  if (!HasFailure(CheckRegistrationPreconditions("", false, false, true),
                  Luna::ErrorCategory::InvalidGlobalName, "empty"))
    return 1;

  const std::string Overlong(256, 'A');
  if (!HasFailure(CheckRegistrationPreconditions(Overlong, true, true, false),
                  Luna::ErrorCategory::InvalidGlobalName, "256 bytes"))
    return 2;

  std::string NonAscii = "Name";
  NonAscii.push_back(static_cast<char>(0x80));
  if (!HasFailure(CheckRegistrationPreconditions(NonAscii, true, true, false),
                  Luna::ErrorCategory::InvalidGlobalName, "0x80"))
    return 3;

  if (!HasFailure(CheckRegistrationPreconditions("1Name", true, true, false),
                  Luna::ErrorCategory::InvalidGlobalName, "first byte"))
    return 4;

  if (!HasFailure(CheckRegistrationPreconditions("Bad-Name", true, true, false),
                  Luna::ErrorCategory::InvalidGlobalName, "byte 4"))
    return 5;

  const std::string EmbeddedZero("Good\0Name", 9);
  if (!HasFailure(
          CheckRegistrationPreconditions(EmbeddedZero, true, true, false),
          Luna::ErrorCategory::InvalidGlobalName, "0x00"))
    return 6;

  const std::string MaximumLength(255, 'z');
  if (CheckRegistrationPreconditions("_", true, true, false) ||
      CheckRegistrationPreconditions("A0_", true, true, false) ||
      CheckRegistrationPreconditions(MaximumLength, true, true, false))
    return 7;

  if (!HasFailure(
          CheckRegistrationPreconditions("ReadyName", false, false, true),
          Luna::ErrorCategory::StateNotReady, "ReadyName"))
    return 8;

  if (!HasFailure(CheckRegistrationPreconditions("NullName", true, false, true),
                  Luna::ErrorCategory::NullCallable, "NullName"))
    return 9;

  if (!HasFailure(
          CheckRegistrationPreconditions("DuplicateName", true, true, true),
          Luna::ErrorCategory::DuplicateGlobalName, "DuplicateName"))
    return 10;

  Hooks::ResetLifecycle();
  Hooks::FailNextCreations();
  Luna::State NotReady;
  int (*NullTarget)(int) = nullptr;

  const auto InvalidDominates =
      NotReady.Bindings().Register("9Invalid", NullTarget);
  if (!HasFailure(InvalidDominates, Luna::ErrorCategory::InvalidGlobalName,
                  "first byte"))
    return 11;

  const auto ReadinessDominates =
      NotReady.Bindings().Register("ValidName", NullTarget);
  if (!HasFailure(ReadinessDominates, Luna::ErrorCategory::StateNotReady,
                  "ValidName"))
    return 12;

  Luna::State Ready;
  if (!Ready.IsReady())
    return 13;

  const auto EntryDepth = Hooks::ObserveRootStackDepth(Ready);
  const auto NullFailure = Ready.Bindings().Register("NullTarget", NullTarget);
  if (!HasFailure(NullFailure, Luna::ErrorCategory::NullCallable,
                  "NullTarget") ||
      Hooks::BindingCount(Ready) != 0 ||
      Hooks::ObserveRootStackDepth(Ready) != EntryDepth)
    return 14;

  const auto InvalidFailure =
      Ready.Bindings().Register("Bad.Name", [](int Value) { return Value; });
  if (!HasFailure(InvalidFailure, Luna::ErrorCategory::InvalidGlobalName,
                  "byte 4") ||
      Hooks::BindingCount(Ready) != 0 ||
      Hooks::ObserveRootStackDepth(Ready) != EntryDepth)
    return 15;

  const auto Installed = Ready.Bindings().Register(
      "Deferred", [](int Value) { return Value + 1; });
  if (!Installed.IsSuccess() || Hooks::BindingCount(Ready) != 1 ||
      Hooks::PendingBindingCount(Ready) != 0 ||
      Hooks::ObserveRootStackDepth(Ready) != EntryDepth)
    return 16;

  return 0;
}
