// Overload probing and selection benchmark scenario.
//
// The corpus is registered once per mode, outside the measured region, so a
// sample times only what a call site pays: probing every candidate of one
// overload set, ranking them, and selecting one. Root overload sets and member
// overload sets are measured separately, because a member call additionally
// validates its receiver at rank position zero.

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

[[nodiscard]] int Probe(int Value) { return Value + 1; }
[[nodiscard]] int Probe(int Left, int Right) { return Left + Right; }
[[nodiscard]] int Probe(int First, int Second, int Third) {
  return First + Second + Third;
}

// The measured class and the base it declares, so the registered class graph is
// a connected one like a real corpus.
struct Panel {
  double Depth = 1.0;

  [[nodiscard]] double Deeper() const { return Depth + 1.0; }
};

struct Dial final : Panel {
  double Level = 1.0;

  Dial() = default;
  explicit Dial(double LevelValue) : Level(LevelValue) {}

  [[nodiscard]] double Combine(double First) const { return Level + First; }
  [[nodiscard]] double Combine(double First, double Second) const {
    return Level + First + Second;
  }
};

constexpr std::size_t LoopCount = 250;

[[nodiscard]] Luna::StableTypeKey DialKey() {
  return Luna::StableTypeKey("benchmarks.overload.Dial");
}

[[nodiscard]] Luna::StableTypeKey PanelKey() {
  return Luna::StableTypeKey("benchmarks.overload.Panel");
}

[[nodiscard]] Luna::RegistrationResult
RegisterCorpus(Luna::BindingRegistry &Registry) {
  if (auto Result =
          Registry.RegisterFunction("Probe", Luna::Overload<int(int)>(&Probe));
      !Result.IsSuccess())
    return Result;
  if (auto Result = Registry.RegisterFunction(
          "Probe", Luna::Overload<int(int, int)>(&Probe));
      !Result.IsSuccess())
    return Result;
  if (auto Result = Registry.RegisterFunction(
          "Probe", Luna::Overload<int(int, int, int)>(&Probe));
      !Result.IsSuccess())
    return Result;

  Luna::ClassBuilder<Panel> Base =
      Registry.RegisterClass<Panel>("Panel", PanelKey());
  static_cast<void>(Base.Property("Deeper", &Panel::Deeper).QualifiedName());
  if (auto Result = Base.Commit(); !Result.IsSuccess())
    return Result;

  Luna::ClassBuilder<Dial> Class =
      Registry.RegisterClass<Dial>("Dial", DialKey());
  static_cast<void>(
      Class.Constructor<double>()
          .Method("Combine",
                  Luna::Overload<double(double), Dial>(&Dial::Combine))
          .Method("Combine",
                  Luna::Overload<double(double, double), Dial>(&Dial::Combine))
          .Base<Panel>(PanelKey())
          .QualifiedName());
  return Class.Commit();
}

[[nodiscard]] std::string RootProbingScript() {
  return "local Total = 0\n"
         "for Index = 1, " +
         std::to_string(LoopCount) +
         " do\n"
         "  Total = Total + Probe(Index)\n"
         "  Total = Total + Probe(Index, 2)\n"
         "  Total = Total + Probe(Index, 2, 3)\n"
         "end\n"
         "assert(Total > 0)\n";
}

[[nodiscard]] std::string MemberProbingScript() {
  return "local Object = Dial.New(2)\n"
         "local Total = 0\n"
         "for Index = 1, " +
         std::to_string(LoopCount) +
         " do\n"
         "  Total = Total + Object:Combine(Index)\n"
         "  Total = Total + Object:Combine(Index, 2)\n"
         "end\n"
         "assert(Total > 0)\n";
}

void MeasureMode(LunaBenchmark::Suite &Suite, CacheMode Mode) {
  ScenarioModel Prepared(Mode, &RegisterCorpus);
  const std::string RootScript = RootProbingScript();
  const std::string MemberScript = MemberProbingScript();

  const auto MeasureCase =
      [&Suite, &Prepared, Mode](std::string_view Name, std::string_view Corpus,
                                std::size_t Operations,
                                const std::string &Script) {
        if (!Prepared.IsPrepared()) {
          Suite.Block(Name, Corpus, Mode, Prepared.Blocker());
          return;
        }
        Suite.Measure(Name, Corpus, Mode, Operations,
                      [&Prepared, &Script] { return Prepared.Run(Script); });
      };

  MeasureCase("RootOverloadSelection",
              "250 iterations of one three-candidate root overload set",
              LoopCount * 3, RootScript);
  MeasureCase("MemberOverloadSelection",
              "250 iterations of one two-candidate member overload set",
              LoopCount * 2, MemberScript);
}

} // namespace

int main(int ArgumentCount, char **ArgumentValues) {
  LunaBenchmark::Suite Suite("overload", ArgumentCount, ArgumentValues);
  MeasureMode(Suite, CacheMode::UnfrozenUncached);
  MeasureMode(Suite, CacheMode::FrozenCached);
  return Suite.Finish();
}
