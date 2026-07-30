// clang-format off
#include <luna/luna.hpp>

#include <rapidcheck.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
// clang-format on

namespace {

constexpr std::string_view GlobalName = "InvalidInteger";

struct InvalidIntegerCase final {
  double Value;
  std::string Expression;
  std::string ExpectedConstraint;
  std::array<std::string_view, 2> ExcludedConstraints;
};

[[nodiscard]] bool Contains(std::string_view Text, std::string_view Context) {
  return Text.find(Context) != std::string_view::npos;
}

[[nodiscard]] std::string FormatNumber(double Number) {
  if (std::isnan(Number))
    return "NaN";
  if (std::isinf(Number))
    return std::signbit(Number) ? "negative infinity" : "positive infinity";

  std::ostringstream Stream;
  Stream << std::setprecision(std::numeric_limits<double>::max_digits10)
         << Number;
  return Stream.str();
}
[[nodiscard]] InvalidIntegerCase SelectNonFinite(std::uint8_t Selector) {
  switch (Selector % 3U) {
  case 0:
    return {std::numeric_limits<double>::quiet_NaN(),
            "(0/0)",
            "expected a finite signed 32-bit integer",
            {"signed 32-bit range", "integral value"}};
  case 1:
    return {std::numeric_limits<double>::infinity(),
            "(1/0)",
            "expected a finite signed 32-bit integer",
            {"signed 32-bit range", "integral value"}};
  default:
    return {-std::numeric_limits<double>::infinity(),
            "(-1/0)",
            "expected a finite signed 32-bit integer",
            {"signed 32-bit range", "integral value"}};
  }
}

[[nodiscard]] InvalidIntegerCase
SelectOutOfRange(std::uint64_t GeneratedMagnitude, bool Negative) {
  const double Magnitude =
      2'147'483'648.5 + static_cast<double>(GeneratedMagnitude % 1'000'000U);
  const double Value = Negative ? -Magnitude : Magnitude;
  return {Value,
          FormatNumber(Value),
          "expected signed 32-bit range [-2147483648, 2147483647]",
          {"finite signed 32-bit integer", "integral value"}};
}

[[nodiscard]] InvalidIntegerCase SelectFractional(std::uint64_t GeneratedBase,
                                                  bool NegativeFraction) {
  constexpr std::uint64_t Domain = 4'000'000'000ULL;
  const auto Base =
      static_cast<std::int64_t>(GeneratedBase % Domain) - 2'000'000'000LL;
  const double Value =
      static_cast<double>(Base) + (NegativeFraction ? -0.25 : 0.25);
  return {Value,
          FormatNumber(Value),
          "expected an integral value",
          {"finite signed 32-bit integer", "signed 32-bit range"}};
}

[[nodiscard]] std::string InvocationSource(const InvalidIntegerCase &Case) {
  return std::string(GlobalName) + "(" + Case.Expression + ")";
}

} // namespace

int RunInvalidIntegerClassificationProperties() {

  const bool Passed = rc::check(

      "Invalid integer numbers are classified deterministically",
      [](std::uint8_t GeneratedNonFinite, std::uint64_t GeneratedMagnitude,
         bool NegativeOutOfRange, std::uint64_t GeneratedBase,
         bool NegativeFraction) {
        const std::array Cases{
            SelectNonFinite(GeneratedNonFinite),
            SelectOutOfRange(GeneratedMagnitude, NegativeOutOfRange),
            SelectFractional(GeneratedBase, NegativeFraction),
        };

        Luna::State State;
        RC_ASSERT(State.IsReady());

        int Calls = 0;
        const auto Registration =
            State.Bindings().Register(GlobalName, [&Calls](int Value) {
              ++Calls;
              return Value;
            });
        RC_ASSERT(Registration.IsSuccess());

        for (const auto &Case : Cases) {
          const auto Execution = State.Execute(InvocationSource(Case));
          const auto *Diagnostic = Execution.Diagnostic();
          RC_ASSERT(!Execution.IsSuccess());
          RC_ASSERT(Diagnostic != nullptr);
          RC_ASSERT(Diagnostic->Category() == Luna::ErrorCategory::Runtime);

          const std::string_view Message = Diagnostic->Message();
          const std::string Expected =
              "Callable '" + std::string(GlobalName) + "' argument 1 " +
              Case.ExpectedConstraint + " but received " +
              FormatNumber(Case.Value) + ".";
          RC_ASSERT(Contains(Message, Expected));
          RC_ASSERT(!Contains(Message, Case.ExcludedConstraints[0]));
          RC_ASSERT(!Contains(Message, Case.ExcludedConstraints[1]));
          RC_ASSERT(Calls == 0);
        }
      });

  return Passed ? 0 : 1;
}
