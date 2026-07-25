// clang-format off
#include <luna/luna.hpp>

#include <cstdlib>
#include <iostream>
// clang-format on

int main() {
  Luna::State State;

  if (!State.IsReady()) {
    std::cerr << "State creation failed: Luna state is not ready.\n";
    return EXIT_FAILURE;
  }

  constexpr int ExpectedResult = 42;
  int ObservedResult = 0;

  const auto Registration = State.Bindings().Register(
      "FoundationSmoke", [&ObservedResult](int Left, int Right) {
        ObservedResult = Left + Right;
        return ObservedResult;
      });
  if (!Registration.IsSuccess()) {
    std::cerr << "Callable registration failed: ";
    if (const auto *Diagnostic = Registration.Diagnostic())
      std::cerr << Diagnostic->Message();
    else
      std::cerr << "Luna returned no diagnostic.";
    std::cerr << '\n';
    return EXIT_FAILURE;
  }

  const auto Execution = State.Execute("assert(FoundationSmoke(19, 23) == 42)");
  if (!Execution.IsSuccess()) {
    std::cerr << "Luau execution failed: ";
    if (const auto *Diagnostic = Execution.Diagnostic())
      std::cerr << Diagnostic->Message();
    else
      std::cerr << "Luna returned no diagnostic.";
    std::cerr << '\n';
    return EXIT_FAILURE;
  }

  if (ObservedResult != ExpectedResult) {
    std::cerr << "Result validation failed: expected " << ExpectedResult
              << ", observed " << ObservedResult << ".\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
