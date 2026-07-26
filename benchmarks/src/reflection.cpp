// Reflection lookup benchmark scenario.
//
// A snapshot capture, a qualified-name lookup, and a full canonical enumeration
// are measured separately, because they answer from different parts of the
// model. The corpus is registered once per mode so a sample measures reading
// one committed generation rather than building it.

// clang-format off
#include <luna/luna.hpp>

#include "support/harness.hpp"
#include "support/model.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace {

using LunaBenchmark::CacheMode;
using LunaBenchmark::ScenarioModel;

[[nodiscard]] int Measure(int Value) { return Value + 1; }

// The measured class and the base it declares, so the registered class graph is
// a connected one like a real corpus.
struct Instrument {
  double Reading = 1.0;

  [[nodiscard]] double Halved() const { return Reading * 0.5; }
};

struct Gauge final : Instrument {
  double Level = 1.0;

  Gauge() = default;
  explicit Gauge(double LevelValue) : Level(LevelValue) {}

  [[nodiscard]] double Scaled(double Factor) const { return Level * Factor; }
  [[nodiscard]] double Doubled() const { return Level * 2.0; }
};

constexpr std::size_t SymbolCorpusSize = 32;
constexpr std::size_t LookupCount = 250;
constexpr std::size_t CaptureCount = 100;

[[nodiscard]] Luna::StableTypeKey InstrumentKey() {
  return Luna::StableTypeKey("benchmarks.reflection.Instrument");
}

[[nodiscard]] Luna::RegistrationResult
RegisterCorpus(Luna::BindingRegistry &Registry) {
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  for (std::size_t Index = 0; Index < SymbolCorpusSize; ++Index) {
    static_cast<void>(Studio
                          .RegisterFunction("Measure" + std::to_string(Index),
                                            Luna::Overload<int(int)>(&Measure))
                          .QualifiedName());
  }
  Luna::ClassBuilder<Instrument> Base =
      Studio.RegisterClass<Instrument>("Instrument", InstrumentKey());
  static_cast<void>(
      Base.Property("Halved", &Instrument::Halved).QualifiedName());
  Luna::ClassBuilder<Gauge> Class = Studio.RegisterClass<Gauge>(
      "Gauge", Luna::StableTypeKey("benchmarks.reflection.Gauge"));
  static_cast<void>(Class.Constructor<double>()
                        .Field("Level", &Gauge::Level)
                        .Method("Scaled", &Gauge::Scaled)
                        .Property("Doubled", &Gauge::Doubled)
                        .Base<Instrument>(InstrumentKey())
                        .QualifiedName());
  return Studio.Commit();
}

[[nodiscard]] std::vector<std::string> LookupNames() {
  std::vector<std::string> Names;
  Names.reserve(SymbolCorpusSize + 3);
  for (std::size_t Index = 0; Index < SymbolCorpusSize; ++Index)
    Names.push_back("Studio.Measure" + std::to_string(Index));
  Names.emplace_back("Studio.Gauge");
  Names.emplace_back("Studio.Gauge.Scaled");
  Names.emplace_back("Studio.Instrument");
  return Names;
}

void MeasureMode(LunaBenchmark::Suite &Suite, CacheMode Mode) {
  ScenarioModel Prepared(Mode, &RegisterCorpus);
  const std::vector<std::string> Names = LookupNames();

  if (!Prepared.IsPrepared()) {
    Suite.Block("SnapshotCapture", "100 captures of one committed generation",
                Mode, Prepared.Blocker());
    Suite.Block("QualifiedNameLookup",
                "250 qualified-name lookups over 35 committed symbols", Mode,
                Prepared.Blocker());
    Suite.Block("CanonicalEnumeration",
                "one full canonical symbol enumeration of the corpus", Mode,
                Prepared.Blocker());
    return;
  }

  Luna::BindingRegistry Registry = Prepared.Bindings();

  Suite.Measure("SnapshotCapture", "100 captures of one committed generation",
                Mode, CaptureCount, [&Registry] {
                  for (std::size_t Index = 0; Index < CaptureCount; ++Index) {
                    const Luna::ReflectionSnapshot Captured =
                        Registry.Reflection();
                    if (Captured.IsEmpty())
                      return false;
                    LunaBenchmark::Suite::Consume(Captured.Size());
                  }
                  return true;
                });

  Suite.Measure("QualifiedNameLookup",
                "250 qualified-name lookups over 35 committed symbols", Mode,
                LookupCount, [&Registry, &Names] {
                  const Luna::ReflectionSnapshot Captured =
                      Registry.Reflection();
                  for (std::size_t Index = 0; Index < LookupCount; ++Index) {
                    const Luna::ReflectionRecord Found =
                        Captured.Find(Names[Index % Names.size()]);
                    if (!Found.IsValid())
                      return false;
                    LunaBenchmark::Suite::Consume(Found.QualifiedName().size());
                  }
                  return true;
                });

  Suite.Measure(
      "CanonicalEnumeration",
      "one full canonical symbol enumeration of the corpus", Mode, 1,
      [&Registry] {
        const Luna::ReflectionSnapshot Captured = Registry.Reflection();
        const Luna::ReflectionRecordRange Symbols = Captured.Symbols();
        if (Symbols.IsEmpty())
          return false;
        for (std::size_t Index = 0; Index < Symbols.Size(); ++Index) {
          const Luna::ReflectionRecord Record = Symbols.At(Index);
          if (!Record.IsValid())
            return false;
          LunaBenchmark::Suite::Consume(Record.QualifiedName().size());
        }
        return true;
      });
}

} // namespace

int main(int ArgumentCount, char **ArgumentValues) {
  LunaBenchmark::Suite Suite("reflection", ArgumentCount, ArgumentValues);
  MeasureMode(Suite, CacheMode::UnfrozenUncached);
  MeasureMode(Suite, CacheMode::FrozenCached);
  return Suite.Finish();
}
