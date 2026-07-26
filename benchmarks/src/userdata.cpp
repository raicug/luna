// Userdata access benchmark scenario.
//
// Construction, field reads and writes, method calls, and property reads all
// pass through the validated access gate, so each of them is measured on the
// same registered class. The corpus is registered once per mode, outside the
// timed region.

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

// The measured class and the base it declares, so the registered class graph is
// a connected one like a real corpus.
struct Container {
  double Capacity = 1.0;

  [[nodiscard]] double Doubled() const { return Capacity * 2.0; }
};

struct Crate final : Container {
  double Width = 1.0;

  Crate() = default;
  explicit Crate(double WidthValue) : Width(WidthValue) {}

  [[nodiscard]] double Scaled(double Factor) const { return Width * Factor; }
  [[nodiscard]] double Area() const { return Width * Width; }

  void Grow(double Factor) { Width *= Factor; }
};

constexpr std::size_t LoopCount = 250;

[[nodiscard]] Luna::StableTypeKey ContainerKey() {
  return Luna::StableTypeKey("benchmarks.userdata.Container");
}

[[nodiscard]] Luna::StableTypeKey CrateKey() {
  return Luna::StableTypeKey("benchmarks.userdata.Crate");
}

[[nodiscard]] Luna::RegistrationResult
RegisterCorpus(Luna::BindingRegistry &Registry) {
  Luna::ClassBuilder<Container> Base =
      Registry.RegisterClass<Container>("Container", ContainerKey());
  static_cast<void>(
      Base.Property("Doubled", &Container::Doubled).QualifiedName());
  if (auto Result = Base.Commit(); !Result.IsSuccess())
    return Result;

  Luna::ClassBuilder<Crate> Class =
      Registry.RegisterClass<Crate>("Crate", CrateKey());
  static_cast<void>(Class.Constructor<double>()
                        .Field("Width", &Crate::Width)
                        .Method("Scaled", &Crate::Scaled)
                        .Method("Grow", &Crate::Grow)
                        .Property("Area", &Crate::Area)
                        .Base<Container>(ContainerKey())
                        .QualifiedName());
  return Class.Commit();
}

[[nodiscard]] std::string ConstructionScript() {
  return "for Index = 1, " + std::to_string(LoopCount) +
         " do\n"
         "  local Object = Crate.New(Index)\n"
         "  assert(Object.Width == Index)\n"
         "end\n";
}

[[nodiscard]] std::string MemberAccessScript() {
  return "local Object = Crate.New(2)\n"
         "local Total = 0\n"
         "for Index = 1, " +
         std::to_string(LoopCount) +
         " do\n"
         "  Object.Width = Index\n"
         "  Total = Total + Object.Width + Object.Area + Object:Scaled(2)\n"
         "end\n"
         "assert(Total > 0)\n";
}

[[nodiscard]] std::string MutatingMethodScript() {
  return "local Object = Crate.New(1)\n"
         "for Index = 1, " +
         std::to_string(LoopCount) +
         " do\n"
         "  Object:Grow(1)\n"
         "end\n"
         "assert(Object.Width == 1)\n";
}

void MeasureMode(LunaBenchmark::Suite &Suite, CacheMode Mode) {
  ScenarioModel Prepared(Mode, &RegisterCorpus);
  const std::string Construction = ConstructionScript();
  const std::string MemberAccess = MemberAccessScript();
  const std::string MutatingMethod = MutatingMethodScript();

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

  MeasureCase("Construction",
              "250 constructed Lua-owned objects with one field read each",
              LoopCount, Construction);
  MeasureCase("MemberAccess",
              "250 iterations of one field write, one field read, one property "
              "read, and one method call",
              LoopCount * 4, MemberAccess);
  MeasureCase("MutatingMethod",
              "250 mutating method calls on one retained object", LoopCount,
              MutatingMethod);
}

} // namespace

int main(int ArgumentCount, char **ArgumentValues) {
  LunaBenchmark::Suite Suite("userdata", ArgumentCount, ArgumentValues);
  MeasureMode(Suite, CacheMode::UnfrozenUncached);
  MeasureMode(Suite, CacheMode::FrozenCached);
  return Suite.Finish();
}
