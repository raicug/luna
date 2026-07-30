// clang-format off
#include <luna/luna.hpp>

#include <rapidcheck.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace {

[[nodiscard]] std::uint32_t Normalize(int Value) noexcept {
  return static_cast<std::uint32_t>(Value);
}

[[nodiscard]] int GeneratedValue(const std::vector<int> &Values,
                                 std::size_t Index) noexcept {
  if (Values.empty())
    return static_cast<int>(Index * 97U + 13U);
  return Values[Index % Values.size()];
}

[[nodiscard]] int Behavior(const std::vector<int> &Values,
                           std::size_t Index) noexcept {
  return static_cast<int>(Normalize(GeneratedValue(Values, Index)) % 2001U) -
         1000;
}

[[nodiscard]] std::string ExistingName(const std::vector<int> &Values,
                                       std::size_t Index) {
  return "existing_" + std::to_string(Index) + "_" +
         std::to_string(Normalize(GeneratedValue(Values, Index + 1U)) % 1000U);
}

[[nodiscard]] std::string InvalidName(int GeneratedKind) {
  switch (Normalize(GeneratedKind) % 5U) {
  case 0:
    return {};
  case 1:
    return "9invalid";
  case 2:
    return "invalid-name";
  case 3:
    return std::string("invalid") + static_cast<char>(0x80);
  default:
    return std::string(256, 'a');
  }
}
[[nodiscard]]
std::string VerificationSource(const std::vector<std::string> &Names) {
  std::string Source;
  for (std::size_t Index = 0; Index < Names.size(); ++Index) {
    Source +=
        "ObserveValue(" + Names[Index] + "(), " + std::to_string(Index) + ")\n";
  }
  return Source;
}

[[nodiscard]] bool HasCategory(const Luna::RegistrationResult &Result,
                               Luna::ErrorCategory Category) noexcept {
  return !Result.IsSuccess() && Result.Diagnostic() &&
         Result.Diagnostic()->Category() == Category;
}

} // namespace

int RunFailedRegistrationTransactionsProperties() {

  const bool Passed = rc::check(

      "Failed registration is transactional",
      [](const std::vector<int> &GeneratedValues, int GeneratedCount,
         int GeneratedAttempt) {
        const std::size_t Count = 1U + (Normalize(GeneratedCount) % 6U);
        std::vector<std::string> Names;
        std::vector<int> Behaviors;
        std::vector<int> Invocations(Count, 0);
        std::vector<bool> BehaviorMatches(Count, true);
        Names.reserve(Count);
        Behaviors.reserve(Count);

        for (std::size_t Index = 0; Index < Count; ++Index) {
          Names.push_back(ExistingName(GeneratedValues, Index));
          Behaviors.push_back(Behavior(GeneratedValues, Index));
        }

        Luna::State State;
        RC_ASSERT(State.IsReady());
        for (std::size_t Index = 0; Index < Count; ++Index) {
          const auto Registration = State.Bindings().Register(
              Names[Index], [&Invocations, &Behaviors, Index]() {
                ++Invocations[Index];
                return Behaviors[Index];
              });
          RC_ASSERT(Registration.IsSuccess());
        }

        int ObserverInvocations = 0;
        const auto ObserverRegistration = State.Bindings().Register(
            "ObserveValue", [&Behaviors, &BehaviorMatches,
                             &ObserverInvocations](int Actual, int Index) {
              ++ObserverInvocations;
              const auto Position = static_cast<std::size_t>(Index);
              if (Position >= Behaviors.size())
                return;
              BehaviorMatches[Position] =
                  BehaviorMatches[Position] && Actual == Behaviors[Position];
            });
        RC_ASSERT(ObserverRegistration.IsSuccess());

        const std::string Source = VerificationSource(Names);
        int ExpectedInvocations = 0;
        const auto VerifyOriginalBehavior = [&]() {
          const auto Execution = State.Execute(Source);
          RC_ASSERT(Execution.IsSuccess());
          ++ExpectedInvocations;
          for (const int InvocationCount : Invocations)
            RC_ASSERT(InvocationCount == ExpectedInvocations);
          RC_ASSERT(ObserverInvocations ==
                    static_cast<int>(Count) * ExpectedInvocations);
          for (const bool Matches : BehaviorMatches)
            RC_ASSERT(Matches);
        };

        VerifyOriginalBehavior();

        const auto Invalid = State.Bindings().Register(
            InvalidName(GeneratedAttempt), []() { return 2001; });
        RC_ASSERT(HasCategory(Invalid, Luna::ErrorCategory::InvalidGlobalName));
        VerifyOriginalBehavior();

        const std::size_t DuplicateIndex = Normalize(GeneratedAttempt) % Count;
        const int ReplacementBehavior = Behaviors[DuplicateIndex] + 1;
        const auto Duplicate = State.Bindings().Register(
            Names[DuplicateIndex],
            [ReplacementBehavior]() { return ReplacementBehavior; });
        RC_ASSERT(
            HasCategory(Duplicate, Luna::ErrorCategory::DuplicateGlobalName));
        VerifyOriginalBehavior();

        int (*NullTarget)() = nullptr;
        const auto Null = State.Bindings().Register("null_attempt", NullTarget);
        RC_ASSERT(HasCategory(Null, Luna::ErrorCategory::NullCallable));
        VerifyOriginalBehavior();
      });

  return Passed ? 0 : 1;
}
