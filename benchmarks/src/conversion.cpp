// clang-format off
#include <luna/luna.hpp>

#include "support/harness.hpp"
#include "support/model.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
// clang-format on

namespace {

using LunaBenchmark::CacheMode;
using LunaBenchmark::ScenarioModel;

[[nodiscard]] double Scalars(int Count, double Factor, bool Enabled) {
  const double Scaled = static_cast<double>(Count) * Factor;
  return Enabled ? Scaled : -Scaled;
}

[[nodiscard]] std::string Text(std::string Source) {
  Source.push_back('.');
  return Source;
}

[[nodiscard]] int Optional(int Base, std::optional<int> Extra) {
  return Base + (Extra ? *Extra : 100);
}

[[nodiscard]] int Variadic(Luna::ArgumentView Arguments) {
  double Total = 0.0;
  for (std::size_t Index = 0; Index < Arguments.Size(); ++Index) {
    if (const std::optional<double> Element = Arguments.ToNumber(Index))
      Total += *Element;
  }
  return static_cast<int>(Total);
}

[[nodiscard]] std::tuple<int, double, std::string> Packed(int Seed) {
  return {Seed, static_cast<double>(Seed) * 0.5, std::to_string(Seed)};
}

constexpr std::size_t LoopCount = 250;

[[nodiscard]] Luna::RegistrationResult
RegisterFixedCorpus(Luna::BindingRegistry &Registry) {
  if (auto Result = Registry.RegisterFunction("Scalars", &Scalars);
      !Result.IsSuccess())
    return Result;
  if (auto Result = Registry.RegisterFunction("Text", &Text);
      !Result.IsSuccess())
    return Result;
  return Registry.RegisterFunction("Optional", &Optional);
}

[[nodiscard]] Luna::RegistrationResult
RegisterRelaxedCorpus(Luna::BindingRegistry &Registry) {
  if (auto Result = Registry.RegisterFunction("Variadic", &Variadic);
      !Result.IsSuccess())
    return Result;
  return Registry.RegisterFunction("Packed", &Packed);
}

[[nodiscard]] std::string Repeated(std::string_view Body) {
  return "local Total = 0\nfor Index = 1, " + std::to_string(LoopCount) +
         " do\n" + std::string(Body) + "end\nassert(Total > 0)\n";
}

void MeasureMode(LunaBenchmark::Suite &Suite, CacheMode Mode) {
  ScenarioModel Fixed(Mode, &RegisterFixedCorpus);
  ScenarioModel Relaxed(Mode, &RegisterRelaxedCorpus);

  const std::string ScalarScript =
      Repeated("  Total = Total + Scalars(Index, 1.5, true)\n"
               "  Total = Total + #Text('value')\n");
  const std::string OptionalScript =
      Repeated("  Total = Total + Optional(Index)\n"
               "  Total = Total + Optional(Index, 2)\n");
  const std::string VariadicScript =
      Repeated("  Total = Total + Variadic(Index, 2, 3, 4)\n");
  const std::string PackScript =
      Repeated("  local Count, Scaled, Label = Packed(Index)\n"
               "  Total = Total + Count + Scaled + #Label\n");

  const auto MeasureCase =
      [&Suite, Mode](ScenarioModel &Prepared, std::string_view Name,
                     std::string_view Corpus, std::size_t Operations,
                     const std::string &Script) {
        if (!Prepared.IsPrepared()) {
          Suite.Block(Name, Corpus, Mode, Prepared.Blocker());
          return;
        }
        Suite.Measure(Name, Corpus, Mode, Operations,
                      [&Prepared, &Script] { return Prepared.Run(Script); });
      };

  MeasureCase(Fixed, "ScalarAndString",
              "250 iterations of number, boolean, and string conversion",
              LoopCount * 2, ScalarScript);
  MeasureCase(Fixed, "OptionalTail",
              "250 iterations of one omitted and one supplied optional",
              LoopCount * 2, OptionalScript);
  MeasureCase(Relaxed, "Variadic",
              "250 iterations of one four-element variadic pack", LoopCount,
              VariadicScript);
  MeasureCase(Relaxed, "FixedReturnPack",
              "250 iterations of one three-element declared return pack",
              LoopCount, PackScript);
}

} // namespace

int main(int ArgumentCount, char **ArgumentValues) {
  LunaBenchmark::Suite Suite("conversion", ArgumentCount, ArgumentValues);
  MeasureMode(Suite, CacheMode::UnfrozenUncached);
  MeasureMode(Suite, CacheMode::FrozenCached);
  return Suite.Finish();
}
