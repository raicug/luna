// Native invocation benchmark scenario.
//
// The declared shapes here are deliberately minimal, so a sample measures the
// invocation path itself: entering the callback, restoring the stack, and
// returning zero values, one value, or a dynamic pack. The corpus is registered
// once per mode, outside the timed region.

// clang-format off
#include <luna/luna.hpp>

#include "support/harness.hpp"
#include "support/model.hpp"

#include <cstddef>
#include <string>
#include <string_view>
// clang-format on

namespace {

using LunaBenchmark::CacheMode;
using LunaBenchmark::ScenarioModel;

std::size_t SideEffectCount = 0;

void Touch() { ++SideEffectCount; }

[[nodiscard]] int Add(int Left, int Right) { return Left + Right; }

[[nodiscard]] Luna::ReturnPack Dynamic(int Seed) {
  Luna::ReturnPack Published;
  Published.AppendInteger(Seed).AppendInteger(Seed + 1).AppendInteger(Seed + 2);
  return Published;
}

constexpr std::size_t LoopCount = 250;

// The fixed-return shapes and the dynamic pack are separate corpora, so a mode
// that cannot prepare one still reports measured numbers for the other.
[[nodiscard]] Luna::RegistrationResult
RegisterFixedCorpus(Luna::BindingRegistry &Registry) {
  if (auto Result = Registry.RegisterFunction("Touch", &Touch);
      !Result.IsSuccess())
    return Result;
  return Registry.RegisterFunction("Add", &Add);
}

[[nodiscard]] Luna::RegistrationResult
RegisterDynamicCorpus(Luna::BindingRegistry &Registry) {
  return Registry.RegisterFunction("Dynamic", &Dynamic);
}

[[nodiscard]] std::string VoidScript() {
  return "for Index = 1, " + std::to_string(LoopCount) +
         " do\n"
         "  Touch()\n"
         "end\n";
}

[[nodiscard]] std::string ScalarScript() {
  return "local Total = 0\n"
         "for Index = 1, " +
         std::to_string(LoopCount) +
         " do\n"
         "  Total = Total + Add(Index, 1)\n"
         "end\n"
         "assert(Total > 0)\n";
}

[[nodiscard]] std::string DynamicScript() {
  return "local Total = 0\n"
         "for Index = 1, " +
         std::to_string(LoopCount) +
         " do\n"
         "  local First, Second, Third = Dynamic(Index)\n"
         "  Total = Total + First + Second + Third\n"
         "end\n"
         "assert(Total > 0)\n";
}

void MeasureMode(LunaBenchmark::Suite &Suite, CacheMode Mode) {
  ScenarioModel Fixed(Mode, &RegisterFixedCorpus);
  ScenarioModel DynamicPacks(Mode, &RegisterDynamicCorpus);
  const std::string Void = VoidScript();
  const std::string Scalar = ScalarScript();
  const std::string DynamicPack = DynamicScript();

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

  MeasureCase(Fixed, "VoidCall", "250 nullary native calls returning no value",
              LoopCount, Void);
  MeasureCase(Fixed, "ScalarCall",
              "250 two-argument native calls returning one value", LoopCount,
              Scalar);
  MeasureCase(DynamicPacks, "DynamicPackCall",
              "250 native calls publishing one three-value dynamic pack",
              LoopCount, DynamicPack);
}

} // namespace

int main(int ArgumentCount, char **ArgumentValues) {
  LunaBenchmark::Suite Suite("invocation", ArgumentCount, ArgumentValues);
  MeasureMode(Suite, CacheMode::UnfrozenUncached);
  MeasureMode(Suite, CacheMode::FrozenCached);
  LunaBenchmark::Suite::Consume(SideEffectCount);
  return Suite.Finish();
}
