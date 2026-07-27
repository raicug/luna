// clang-format off
#include <luna/luna.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
// clang-format on

namespace {

static_assert(
    std::is_same_v<decltype(std::declval<Luna::BindingRegistry &>().Freeze()),
                   Luna::RegistrationResult>,
    "BindingRegistry::Freeze must keep returning RegistrationResult.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::BindingRegistry &>().Reflection()),
        Luna::ReflectionSnapshot>,
    "A frozen State must keep answering reflection with an owning snapshot.");
static_assert(std::is_copy_constructible_v<Luna::ReflectionSnapshot>,
              "A snapshot retained across freeze must stay a copyable value.");

struct ConsumerGauge final {
  int Charge = 3;
  [[nodiscard]] int Level() const { return Charge * 2; }
};

enum class ConsumerMode { Off = 0, On = 1 };

[[nodiscard]] int ConsumerIncrement(int Value) { return Value + 1; }
[[nodiscard]] int ConsumerMeasure(int Value) { return Value + 1; }
[[nodiscard]] int ConsumerMeasure(int Value, int Scale) {
  return Value * Scale;
}

void ConfigureConsumerUnits(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Units = Builder.RegisterNamespace("Units");
  static_cast<void>(Units.RegisterConstant("Scale", 2));
}

} // namespace

void VerifyFreezeConsumerBoundaryCompiles() {
  Luna::ReflectionSnapshot Retained;
  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();

    [[maybe_unused]] const Luna::RegistrationResult Established =
        Registry.Register("Increment", &ConsumerIncrement);
    [[maybe_unused]] const Luna::RegistrationResult First =
        Registry.RegisterFunction("Measure",
                                  Luna::Overload<int(int)>(&ConsumerMeasure));
    [[maybe_unused]] const Luna::RegistrationResult Second =
        Registry.RegisterFunction(
            "Measure", Luna::Overload<int(int, int)>(&ConsumerMeasure));

    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    static_cast<void>(Studio.RegisterConstant("Version", 7));
    Luna::EnumBuilder<ConsumerMode> Modes = Studio.RegisterEnum<ConsumerMode>(
        "Mode", Luna::StableTypeKey("consumer.freeze.Mode"));
    static_cast<void>(Modes.Value("Off", ConsumerMode::Off)
                          .Value("On", ConsumerMode::On)
                          .QualifiedName());
    Luna::ClassBuilder<ConsumerGauge> Gauge =
        Studio.RegisterClass<ConsumerGauge>(
            "Gauge", Luna::StableTypeKey("consumer.freeze.Gauge"));
    static_cast<void>(Gauge.Constructor<>()
                          .Field("Charge", &ConsumerGauge::Charge)
                          .Property("Level", Luna::PropertyPolicy::Lazy(),
                                    &ConsumerGauge::Level)
                          .QualifiedName());
    [[maybe_unused]] const Luna::RegistrationResult Committed = Studio.Commit();

    const std::optional<Luna::ModuleManifest> Units =
        Luna::ModuleManifest::TryCreate(
            "consumer.freeze.units",
            Luna::SemanticVersion::TryParse("1.0.0").value_or(
                Luna::SemanticVersion()),
            {}, std::string(), {});
    if (Units) {
      [[maybe_unused]] const Luna::RegistrationResult Loaded =
          Registry.RegisterModule(*Units, &ConfigureConsumerUnits);
    }

    const Luna::RegistrationResult Frozen = Registry.Freeze();
    const Luna::RegistrationResult Repeated = Registry.Freeze();
    [[maybe_unused]] const bool Reported =
        Frozen.IsSuccess() ||
        (Frozen.Diagnostic() != nullptr &&
         Frozen.Diagnostic()->Category() == Luna::ErrorCategory::StateNotReady);
    [[maybe_unused]] const bool Deterministic =
        Repeated.IsSuccess() || Repeated.Diagnostic() != nullptr;

    [[maybe_unused]] const Luna::ExecutionResult Executed =
        Owner.Execute("return Increment(41)");
    [[maybe_unused]] const Luna::RegistrationResult Rejected =
        Registry.Register("Later", &ConsumerIncrement);

    Retained = Registry.Reflection();
    const Luna::ReflectionRecord Class = Retained.Find("Studio.Gauge");
    [[maybe_unused]] const bool Queried =
        Class.IsValid() && Class.Kind() == Luna::SymbolKind::Class &&
        Retained.Find(Class.Id()).Id() == Class.Id() &&
        !Retained.Find("Studio.Missing").IsValid();
  }

  const Luna::ReflectionRecordRange Symbols = Retained.Symbols();
  std::vector<std::string> Names;
  Names.reserve(Symbols.Size());
  for (std::size_t Index = 0; Index < Symbols.Size(); ++Index)
    Names.push_back(std::string(Symbols.At(Index).QualifiedName()));
  [[maybe_unused]] const std::size_t Counted =
      Names.size() + Retained.Types().Size() + Retained.Modules().Size();
}
