// clang-format off
#include <luna/luna.hpp>

#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using FaultPoint = Luna::Detail::StateFaultPoint;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "frozen state integration check failed: " << Description << '\n';
}

enum class Mode { Off = 0, On = 1 };

std::size_t LevelReads = 0;
std::size_t BoostCalls = 0;

struct Widget final {
  int Charge = 3;

  [[nodiscard]] int Level() const {
    ++LevelReads;
    return Charge * 2;
  }

  int Boost(int Amount) {
    ++BoostCalls;
    Charge += Amount;
    return Charge;
  }
};

[[nodiscard]] int Increment(int Value) { return Value + 1; }
[[nodiscard]] int Measure(int Value) { return Value + 1; }
[[nodiscard]] int Measure(int Value, int Scale) { return Value * Scale; }

[[nodiscard]] int Tally(Luna::ArgumentView Arguments) {
  return static_cast<int>(Arguments.Size());
}

[[nodiscard]] std::tuple<bool, int, std::string> Describe(std::string Text) {
  return {!Text.empty(), static_cast<int>(Text.size()), Text};
}

[[nodiscard]] int Failing(int Value) {
  throw std::runtime_error("the frozen model refused " + std::to_string(Value));
}

[[nodiscard]] Luna::StableTypeKey WidgetKey() {
  return Luna::StableTypeKey("tests.freeze.integration.Widget");
}

[[nodiscard]] Luna::StableTypeKey ModeKey() {
  return Luna::StableTypeKey("tests.freeze.integration.Mode");
}

[[nodiscard]] Luna::ModuleManifest UnitsManifest() {
  const std::optional<Luna::SemanticVersion> Version =
      Luna::SemanticVersion::TryParse("1.4.0");
  const std::optional<Luna::ModuleManifest> Manifest =
      Version ? Luna::ModuleManifest::TryCreate(
                    "tests.freeze.integration.units", *Version, {}, "", {})
              : std::nullopt;
  return Manifest ? *Manifest : Luna::ModuleManifest();
}

void ConfigureUnits(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Units = Builder.RegisterNamespace("Units");
  static_cast<void>(Units.RegisterConstant("Scale", 2));
}

[[nodiscard]] bool RegisterModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();

  if (!Registry.Register("Increment", &Increment).IsSuccess())
    return false;
  if (!Registry.Register("Failing", &Failing).IsSuccess())
    return false;
  if (!Registry.RegisterFunction("Measure", Luna::Overload<int(int)>(&Measure))
           .IsSuccess())
    return false;
  if (!Registry
           .RegisterFunction("Measure", Luna::Overload<int(int, int)>(&Measure))
           .IsSuccess())
    return false;
  if (!Registry.RegisterFunction("Tally", &Tally).IsSuccess())
    return false;
  if (!Registry.RegisterFunction("Describe", &Describe).IsSuccess())
    return false;

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  static_cast<void>(Studio.RegisterConstant("Version", 7));
  Luna::NamespaceBuilder Nested = Studio.RegisterNamespace("Nested");
  static_cast<void>(Nested.RegisterConstant("Depth", 2));

  Luna::EnumBuilder<Mode> Modes = Studio.RegisterEnum<Mode>("Mode", ModeKey());
  static_cast<void>(Modes.Value("Off", Mode::Off)
                        .Value("On", Mode::On)
                        .Alias("Enabled", "On")
                        .QualifiedName());

  Luna::ClassBuilder<Widget> Class =
      Studio.RegisterClass<Widget>("Widget", WidgetKey());
  static_cast<void>(
      Class.Constructor<>()
          .Method("Boost", &Widget::Boost)
          .Field("Charge", &Widget::Charge)
          .Property("Level", Luna::PropertyPolicy::Lazy(), &Widget::Level)
          .QualifiedName());
  if (!Studio.Commit().IsSuccess())
    return false;
  return Registry.RegisterModule(UnitsManifest(), &ConfigureUnits).IsSuccess();
}

[[nodiscard]] std::string Value(Luna::State &Owner, const std::string &Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (!Result.IsSuccess()) {
    const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
    return "unexpected refusal: " + (Diagnostic
                                         ? std::string(Diagnostic->Message())
                                         : std::string("<no diagnostic>"));
  }
  const std::optional<int> Observed =
      Hooks::ObserveIntegerGlobal(Owner, "Result");
  return Observed ? std::to_string(*Observed) : std::string("<no result>");
}

[[nodiscard]] std::string Refusal(Luna::State &Owner,
                                  const std::string &Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return "unexpected success";
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  return Diagnostic ? std::string(Diagnostic->Message())
                    : std::string("<no diagnostic>");
}

[[nodiscard]] std::vector<std::string> RunBattery(Luna::State &Owner) {
  std::vector<std::string> Observed;

  Observed.push_back(Owner.Execute("Value = Studio.Widget.New()").IsSuccess()
                         ? "constructed"
                         : "construction refused");

  Observed.push_back(Value(Owner, "Result = Increment(41)"));
  Observed.push_back(Value(Owner, "Result = Measure(4)"));
  Observed.push_back(Value(Owner, "Result = Measure(4, 3)"));
  Observed.push_back(Value(Owner, "Result = Tally()"));
  Observed.push_back(Value(Owner, "Result = Tally(1, 'text', true)"));
  Observed.push_back(
      Value(Owner, "local Flag, Size, Text = Describe('abcd')\n"
                   "Result = 0\n"
                   "if Flag and Size == 4 and Text == 'abcd' then Result = 1 "
                   "end"));
  Observed.push_back(Value(Owner, "Result = Studio.Version"));
  Observed.push_back(Value(Owner, "Result = Studio.Nested.Depth"));
  Observed.push_back(Value(Owner, "Result = Units.Scale"));
  Observed.push_back(Value(Owner, "Result = 0\n"
                                  "if Studio.Mode.On ~= nil then Result = 1 "
                                  "end\n"
                                  "if Studio.Mode.Enabled == Studio.Mode.On "
                                  "then Result = Result + 1 end"));

  Observed.push_back(Value(Owner, "Result = Value.Charge"));
  Observed.push_back(Value(Owner, "Result = Value.Level"));
  Observed.push_back(Value(Owner, "Result = Value.Level"));
  Observed.push_back(Value(Owner, "Result = Value:Boost(2)"));
  Observed.push_back(Value(Owner, "Value.Charge = 6\nResult = Value.Level"));

  Observed.push_back(Refusal(Owner, "return Increment()"));
  Observed.push_back(Refusal(Owner, "return Increment('text')"));
  Observed.push_back(Refusal(Owner, "return Measure('text')"));
  Observed.push_back(Refusal(Owner, "return Failing(3)"));
  Observed.push_back(Refusal(Owner, "return Value.Missing"));
  Observed.push_back(Refusal(Owner, "Value.Charge = 'text'"));

  Observed.push_back(Value(Owner, "Result = Increment(1)"));
  return Observed;
}

[[nodiscard]] std::vector<std::string>
OrderedNames(const Luna::ReflectionSnapshot &Snapshot) {
  const Luna::ReflectionRecordRange Symbols = Snapshot.Symbols();
  std::vector<std::string> Names;
  Names.reserve(Symbols.Size());
  for (std::size_t Index = 0; Index < Symbols.Size(); ++Index) {
    Names.push_back(
        std::string(Symbols.At(Index).QualifiedName()) + "|" +
        std::string(Luna::SymbolKindText(Symbols.At(Index).Kind())) + "|" +
        Symbols.At(Index).Id().ToString());
  }
  return Names;
}

void CheckFreezePreservesEveryObservableOutcome() {
  LevelReads = 0;
  BoostCalls = 0;

  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Owner.IsReady() && RegisterModel(Owner),
        "one plan publishes the whole representative model");

  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  const std::vector<std::string> BeforeFreeze = RunBattery(Owner);
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every script run and refusal restores the root stack depth");
  Check(BeforeFreeze[0] == "constructed" && BeforeFreeze[1] == "42" &&
            BeforeFreeze[2] == "5" && BeforeFreeze[3] == "12" &&
            BeforeFreeze[7] == "7" && BeforeFreeze[8] == "2" &&
            BeforeFreeze[9] == "2" && BeforeFreeze[10] == "2",
        "the unfrozen State answers every registered value as declared");
  Check(BeforeFreeze[4] == "0" && BeforeFreeze[5] == "3" &&
            BeforeFreeze[6] == "1",
        "the unfrozen State answers the variadic tail and the fixed multiple "
        "return as declared");
  Check(BeforeFreeze[11] == "3" && BeforeFreeze[12] == "6" &&
            BeforeFreeze[13] == "6" && BeforeFreeze[14] == "5" &&
            BeforeFreeze[15] == "12",
        "the unfrozen State answers every class member as declared");

  const std::size_t ReadsBefore = LevelReads;
  const std::size_t BoostsBefore = BoostCalls;
  const Luna::ReflectionSnapshot Reflected = Registry.Reflection();
  const std::vector<std::string> Declared = OrderedNames(Reflected);

  Hooks::InjectFault(Owner, FaultPoint::FreezePreparation);
  const Luna::RegistrationResult Refused = Registry.Freeze();
  Check(!Refused.IsSuccess() && Refused.Diagnostic() != nullptr &&
            Refused.Diagnostic()->Category() == Luna::ErrorCategory::Internal,
        "the injected preparation failure refuses the freeze");
  Check(Owner.IsReady() && !Hooks::IsFrozen(Owner) &&
            !Hooks::ObserveFreezeCache(Owner).Published,
        "the refused freeze leaves a Ready State and publishes nothing");

  const std::vector<std::string> AfterRefusal = RunBattery(Owner);
  Check(AfterRefusal == BeforeFreeze,
        "every value and every diagnostic is identical after a refused freeze");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "the refused freeze and the following run restore the stack depth");
  Check(LevelReads == ReadsBefore * 2 && BoostCalls == BoostsBefore * 2,
        "each run ran the declared native members exactly as often as the "
        "first");

  Check(Registry.Freeze().IsSuccess(),
        "the populated State freezes through the real virtual-machine path");
  Check(Hooks::IsFrozen(Owner) && Hooks::ObserveFreezeCache(Owner).Published,
        "freeze publishes its caches and its lifecycle phase together");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "freeze itself restores the exact root stack depth");
  Check(OrderedNames(Registry.Reflection()) == Declared &&
            Registry.Reflection().Generation() == Reflected.Generation(),
        "freeze changes no reflected record, kind, identity, or generation");

  const std::vector<std::string> AfterFreeze = RunBattery(Owner);
  Check(AfterFreeze == BeforeFreeze,
        "every value and every diagnostic is identical after freeze");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every frozen script run and refusal restores the root stack depth");

  Check(!Registry.Register("Later", &Increment).IsSuccess() &&
            !Registry.RegisterModule(UnitsManifest(), &ConfigureUnits)
                 .IsSuccess() &&
            !Hooks::BindingIsCommitted(Owner, "Later"),
        "a frozen State rejects registration and module mutation");
  Check(OrderedNames(Registry.Reflection()) == Declared,
        "every rejected mutation leaves the reflected metadata unchanged");
  Check(Value(Owner, "Result = Increment(41)") == "42" &&
            Refusal(Owner, "return Failing(3)")
                    .find("the frozen model refused 3") != std::string::npos,
        "the established callable path keeps invoking and translating after "
        "every rejection");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "the frozen State ends exactly where it started");
}

} // namespace

int RunFrozenStateIntegrationTests() {
  FailureCount = 0;
  CheckFreezePreservesEveryObservableOutcome();
  return FailureCount == 0 ? 0 : 1;
}
