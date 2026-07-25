// clang-format off
#include <luna/luna.hpp>

#include <rapidcheck.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
// clang-format on

namespace {

enum class FailureKind : std::uint32_t {
  Compilation,
  LuauRuntime,
  NativeValidation,
  NativeException,
  Count
};

constexpr int NativeExceptionSentinel = 2'000'000'000;

[[nodiscard]] std::uint32_t Normalize(int Value) noexcept {
  return static_cast<std::uint32_t>(Value);
}

[[nodiscard]] int RecoveryValue(int GeneratedValue) noexcept {
  return static_cast<int>(Normalize(GeneratedValue) % 2001U) - 1000;
}

[[nodiscard]] std::array<FailureKind, 4>
FailureOrder(int GeneratedOrder) noexcept {
  constexpr std::array AllFailures{
      FailureKind::Compilation,
      FailureKind::LuauRuntime,
      FailureKind::NativeValidation,
      FailureKind::NativeException,
  };

  std::array<FailureKind, 4> Ordered{};
  const std::size_t Offset = Normalize(GeneratedOrder) % AllFailures.size();
  for (std::size_t Index = 0; Index < Ordered.size(); ++Index)
    Ordered[Index] = AllFailures[(Index + Offset) % AllFailures.size()];
  return Ordered;
}

[[nodiscard]] std::string_view FailureSource(FailureKind Kind) noexcept {
  switch (Kind) {
  case FailureKind::Compilation:
    return "local =";
  case FailureKind::LuauRuntime:
    return "error('generated runtime failure')";
  case FailureKind::NativeValidation:
    return "CommittedRecovery('not an integer')";
  case FailureKind::NativeException:
    return "CommittedRecovery(2000000000)";
  case FailureKind::Count:
    break;
  }
  return "local =";
}

[[nodiscard]] Luna::ErrorCategory ExpectedCategory(FailureKind Kind) noexcept {
  if (Kind == FailureKind::Compilation)
    return Luna::ErrorCategory::Compilation;
  return Luna::ErrorCategory::Runtime;
}

} // namespace

int RunExecutionRecoveryProperties() {
  // **Validates: Requirements 6.4**
  // clang-format off
  // Feature: luau-binding-foundation, Property 13: Execution failures do not poison the State
  const bool Passed = rc::check(
      // clang-format on
      "Execution failures do not poison the State",
      [](int GeneratedOrder, int GeneratedValue) {
        Luna::State State;
        RC_ASSERT(State.IsReady());

        int SuccessfulInvocations = 0;
        int LastValue = 0;
        const auto Registration = State.Bindings().Register(
            "CommittedRecovery",
            [&SuccessfulInvocations, &LastValue](int Value) -> int {
              if (Value == NativeExceptionSentinel)
                throw std::runtime_error("generated native exception");
              ++SuccessfulInvocations;
              LastValue = Value;
              return Value;
            });
        RC_ASSERT(Registration.IsSuccess());

        const int ExpectedValue = RecoveryValue(GeneratedValue);
        const std::string ValidSource =
            "CommittedRecovery(" + std::to_string(ExpectedValue) + ")";
        int ExpectedInvocations = 0;

        for (const FailureKind Kind : FailureOrder(GeneratedOrder)) {
          const auto Failure = State.Execute(FailureSource(Kind));
          RC_ASSERT(!Failure.IsSuccess());
          RC_ASSERT(Failure.Diagnostic() != nullptr);
          RC_ASSERT(Failure.Diagnostic()->Category() == ExpectedCategory(Kind));
          RC_ASSERT(!Failure.Diagnostic()->Message().empty());
          RC_ASSERT(SuccessfulInvocations == ExpectedInvocations);

          const auto Recovery = State.Execute(ValidSource);
          RC_ASSERT(Recovery.IsSuccess());
          RC_ASSERT(Recovery.Diagnostic() == nullptr);
          ++ExpectedInvocations;
          RC_ASSERT(SuccessfulInvocations == ExpectedInvocations);
          RC_ASSERT(LastValue == ExpectedValue);
        }
      });

  return Passed ? 0 : 1;
}
