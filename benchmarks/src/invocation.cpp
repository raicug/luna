// clang-format off
#include <luna/luna.hpp>

#include "support/harness.hpp"
#include "support/invocation_cases.hpp"
#include "support/model.hpp"

#include <cstddef>
#include <string>
#include <string_view>
// clang-format on

namespace {

using LunaBenchmark::CacheMode;
using LunaBenchmark::ScenarioModel;

std::size_t SideEffectCount = 0;

void Touch() {
  ++SideEffectCount;
}

[[nodiscard]] int Add(int Left, int Right) {
  return Left + Right;
}

[[nodiscard]] Luna::ReturnPack Dynamic(int Seed) {
  Luna::ReturnPack Published;
  Published.AppendInteger(Seed).AppendInteger(Seed + 1).AppendInteger(Seed + 2);
  return Published;
}

using LunaBenchmark::InvocationCases::DynamicScript;
using LunaBenchmark::InvocationCases::LoopCount;
using LunaBenchmark::InvocationCases::ScalarScript;
using LunaBenchmark::InvocationCases::VoidScript;

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

  MeasureCase(Fixed, LunaBenchmark::InvocationCases::VoidName,
              LunaBenchmark::InvocationCases::VoidCorpus, LoopCount, Void);
  MeasureCase(Fixed, LunaBenchmark::InvocationCases::ScalarName,
              LunaBenchmark::InvocationCases::ScalarCorpus, LoopCount, Scalar);
  MeasureCase(DynamicPacks, LunaBenchmark::InvocationCases::DynamicName,
              LunaBenchmark::InvocationCases::DynamicCorpus, LoopCount,
              DynamicPack);
}

} // namespace

int main(int ArgumentCount, char **ArgumentValues) {
  LunaBenchmark::Suite Suite("invocation", ArgumentCount, ArgumentValues);
  MeasureMode(Suite, CacheMode::UnfrozenUncached);
  MeasureMode(Suite, CacheMode::FrozenCached);
  LunaBenchmark::Suite::Consume(SideEffectCount);
  return Suite.Finish();
}
