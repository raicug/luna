// clang-format off
#include <luna/luna.hpp>
// clang-format on

void VerifyConsumerBoundaryCompiles() {
  Luna::State State;
  [[maybe_unused]] const Luna::RegistrationResult Registration =
      State.Bindings().Register("Increment",
                                [](int Value) { return Value + 1; });
  [[maybe_unused]] const Luna::ExecutionResult Execution =
      State.Execute("return Increment(41)");
  static_cast<void>(Execution);
}
