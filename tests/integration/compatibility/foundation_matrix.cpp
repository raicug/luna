// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

enum class RegistrationApi { LegacyRegister, ExplicitRegisterFunction };

RegistrationApi ActiveApi = RegistrationApi::LegacyRegister;

class DualRegistry final {
public:
  explicit DualRegistry(Luna::BindingRegistry Registry) noexcept
      : Inner(Registry) {}

  template <class Callable>
  [[nodiscard]] Luna::RegistrationResult Register(std::string_view Name,
                                                  Callable &&Target) {
    if (ActiveApi == RegistrationApi::LegacyRegister)
      return Inner.Register(Name, std::forward<Callable>(Target));
    return Inner.RegisterFunction(Name, std::forward<Callable>(Target));
  }

private:
  Luna::BindingRegistry Inner;
};

[[nodiscard]] DualRegistry Dual(Luna::State &Owner) {
  return DualRegistry(Owner.Bindings());
}

int FreeFunctionCalls = 0;
int NoexceptFunctionCalls = 0;

int DoubleInteger(int Value) {
  ++FreeFunctionCalls;
  return Value * 2;
}

bool AlwaysTrue() noexcept {
  ++NoexceptFunctionCalls;
  return true;
}

struct ScalingFunctor final {
  int Multiplier = 1;

  [[nodiscard]] int operator()(int Value) const { return Value * Multiplier; }
};

struct CountingFunctor final {
  int Calls = 0;

  [[nodiscard]] int operator()() { return ++Calls; }
};

struct FailureExpectation final {
  std::string_view Script;
  std::array<std::string_view, 4> Fragments;
};

[[nodiscard]] bool
DiagnosticMatches(const Luna::ErrorDiagnostic *Diagnostic,
                  Luna::ErrorCategory Category,
                  const std::array<std::string_view, 4> &Fragments) {
  if (!Diagnostic || Diagnostic->Category() != Category ||
      Diagnostic->Message().empty())
    return false;

  for (const auto Fragment : Fragments) {
    if (Fragment.empty())
      continue;
    if (Diagnostic->Message().find(Fragment) == std::string::npos)
      return false;
  }
  return true;
}

[[nodiscard]] bool
ExecutionFailed(const Luna::ExecutionResult &Result,
                const std::array<std::string_view, 4> &Fragments) {
  return !Result.IsSuccess() &&
         DiagnosticMatches(Result.Diagnostic(), Luna::ErrorCategory::Runtime,
                           Fragments);
}

[[nodiscard]] bool
RegistrationFailed(const Luna::RegistrationResult &Result,
                   Luna::ErrorCategory Category,
                   const std::array<std::string_view, 4> &Fragments = {}) {
  return !Result.IsSuccess() &&
         DiagnosticMatches(Result.Diagnostic(), Category, Fragments);
}

[[nodiscard]] bool TestAcceptedSourceForms() {
  FreeFunctionCalls = 0;
  NoexceptFunctionCalls = 0;

  Luna::State State;
  int (*FunctionPointer)(int) = &DoubleInteger;
  const ScalingFunctor ConstFunctor{4};
  ScalingFunctor MutableLvalueFunctor{3};
  int CapturedTotal = 0;
  if (!State.IsReady() ||
      !Dual(State).Register("FreeFunction", DoubleInteger).IsSuccess() ||
      !Dual(State).Register("FunctionPointer", FunctionPointer).IsSuccess() ||
      !Dual(State).Register("AddressOfFunction", &DoubleInteger).IsSuccess() ||
      !Dual(State).Register("NoexceptFunction", AlwaysTrue).IsSuccess() ||
      !Dual(State)
           .Register("StatelessLambda",
                     [](double Value) { return Value / 4.0; })
           .IsSuccess() ||
      !Dual(State)
           .Register("CapturingLambda",
                     [&CapturedTotal](int Value) {
                       CapturedTotal += Value;
                       return CapturedTotal;
                     })
           .IsSuccess() ||
      !Dual(State)
           .Register("MutableLambda", [Count = 0]() mutable { return ++Count; })
           .IsSuccess() ||
      !Dual(State).Register("ConstFunctor", ConstFunctor).IsSuccess() ||
      !Dual(State)
           .Register("LvalueFunctor", MutableLvalueFunctor)
           .IsSuccess() ||
      !Dual(State)
           .Register("TemporaryFunctor", CountingFunctor{})
           .IsSuccess() ||
      !Dual(State).Register("VoidLambda", [] {}).IsSuccess())
    return false;

  const auto Execution = State.Execute("assert(FreeFunction(21) == 42)\n"
                                       "assert(FunctionPointer(5) == 10)\n"
                                       "assert(AddressOfFunction(-3) == -6)\n"
                                       "assert(NoexceptFunction() == true)\n"
                                       "assert(StatelessLambda(9.0) == 2.25)\n"
                                       "assert(CapturingLambda(4) == 4)\n"
                                       "assert(CapturingLambda(6) == 10)\n"
                                       "assert(MutableLambda() == 1)\n"
                                       "assert(MutableLambda() == 2)\n"
                                       "assert(ConstFunctor(5) == 20)\n"
                                       "assert(LvalueFunctor(5) == 15)\n"
                                       "assert(TemporaryFunctor() == 1)\n"
                                       "assert(TemporaryFunctor() == 2)");

  return Execution.IsSuccess() && !Execution.Diagnostic() &&
         FreeFunctionCalls == 3 && NoexceptFunctionCalls == 1 &&
         CapturedTotal == 10;
}

[[nodiscard]] bool TestSupportedValues() {
  Luna::State State;
  bool ObservedFlag = false;
  int ObservedInteger = 0;
  double ObservedNumber = 0.0;
  std::string ObservedText;
  if (!State.IsReady() ||
      !Dual(State)
           .Register("EchoBoolean", [](bool Value) { return Value; })
           .IsSuccess() ||
      !Dual(State)
           .Register("EchoInteger", [](int Value) { return Value; })
           .IsSuccess() ||
      !Dual(State)
           .Register("EchoNumber", [](double Value) { return Value; })
           .IsSuccess() ||
      !Dual(State)
           .Register("EchoString",
                     [](std::string Value) { return std::move(Value); })
           .IsSuccess() ||
      !Dual(State)
           .Register(
               "MeasureString",
               [](std::string Value) { return static_cast<int>(Value.size()); })
           .IsSuccess() ||
      !Dual(State)
           .Register(
               "Observe",
               [&](bool Flag, int Integer, double Number, std::string Text) {
                 ObservedFlag = Flag;
                 ObservedInteger = Integer;
                 ObservedNumber = Number;
                 ObservedText = std::move(Text);
               })
           .IsSuccess())
    return false;

  const auto Execution =
      State.Execute("assert(EchoBoolean(true) == true)\n"
                    "assert(EchoBoolean(false) == false)\n"
                    "assert(EchoInteger(0) == 0)\n"
                    "assert(EchoInteger(-2147483648) == -2147483648)\n"
                    "assert(EchoInteger(2147483647) == 2147483647)\n"
                    "assert(EchoNumber(-0.5) == -0.5)\n"
                    "assert(EchoString('') == '')\n"
                    "assert(EchoString('luna') == 'luna')\n"
                    "assert(MeasureString('\\x41\\x00\\x42') == 3)\n"
                    "assert(#EchoString(string.rep('a', 1048576)) == 1048576)\n"
                    "Observe(true, -17, 3.25, '\\x41\\x00\\x42')");

  return Execution.IsSuccess() && ObservedFlag && ObservedInteger == -17 &&
         ObservedNumber == 3.25 && ObservedText == std::string("A\0B", 3);
}

[[nodiscard]] bool TestRegistrationDiagnostics() {
  Luna::State State;
  if (!State.IsReady())
    return false;

  const std::string Overlong(256, 'A');
  int OriginalCalls = 0;
  int ReplacementCalls = 0;
  const auto Empty = Dual(State).Register("", [] {});
  const auto TooLong = Dual(State).Register(Overlong, [] {});
  const auto IllegalFirst = Dual(State).Register("7Name", [] {});
  const auto IllegalLater = Dual(State).Register("Bad-Name", [] {});
  const auto NullTarget =
      Dual(State).Register("NullTarget", static_cast<int (*)(int)>(nullptr));
  const auto Original = Dual(State).Register("Preserved", [&] {
    ++OriginalCalls;
    return 17;
  });
  const auto Duplicate = Dual(State).Register("Preserved", [&] {
    ++ReplacementCalls;
    return 99;
  });

  return RegistrationFailed(Empty, Luna::ErrorCategory::InvalidGlobalName) &&
         RegistrationFailed(TooLong, Luna::ErrorCategory::InvalidGlobalName,
                            {"256 bytes", "maximum is 255"}) &&
         RegistrationFailed(IllegalFirst,
                            Luna::ErrorCategory::InvalidGlobalName) &&
         RegistrationFailed(IllegalLater,
                            Luna::ErrorCategory::InvalidGlobalName) &&
         RegistrationFailed(NullTarget, Luna::ErrorCategory::NullCallable) &&
         Original.IsSuccess() &&
         RegistrationFailed(Duplicate,
                            Luna::ErrorCategory::DuplicateGlobalName) &&
         Hooks::BindingCount(State) == 1 &&
         Hooks::PendingBindingCount(State) == 0 &&
         State.Execute("assert(Preserved() == 17)").IsSuccess() &&
         OriginalCalls == 1 && ReplacementCalls == 0;
}

[[nodiscard]] bool TestFirstFailureDiagnostics() {
  static constexpr std::array<FailureExpectation, 9> Expectations{{
      {"Strict(true, 1)", {"Callable 'Strict'", "expected 3", "received 2"}},
      {"Strict(true, 1, 'text', false)",
       {"Callable 'Strict'", "expected 3", "received 4"}},
      {"Strict('wrong', 'wrong', 'wrong')",
       {"Callable 'Strict'", "argument 1", "expected boolean",
        "received string"}},
      {"Strict(true, 'wrong', 5)",
       {"Callable 'Strict'", "argument 2", "expected signed 32-bit integer",
        "received string"}},
      {"Strict(true, 1, 5)",
       {"Callable 'Strict'", "argument 3", "expected string",
        "received number"}},
      {"StrictInteger(math.huge)",
       {"argument 1", "finite signed 32-bit integer", "positive infinity"}},
      {"StrictInteger(2147483648)",
       {"argument 1", "signed 32-bit range", "2147483648"}},
      {"StrictInteger(1.5)", {"argument 1", "integral value", "1.5"}},
      {"MeasureString(string.rep('a', 1048577))",
       {"argument 1", "1048577 string bytes", "maximum is 1048576"}},
  }};

  Luna::State State;
  int StrictCalls = 0;
  int IntegerCalls = 0;
  int MeasureCalls = 0;
  if (!State.IsReady() ||
      !Dual(State)
           .Register("Strict",
                     [&](bool Flag, int Integer, std::string Text) {
                       ++StrictCalls;
                       return Text + (Flag ? ":" : "-") +
                              std::to_string(Integer);
                     })
           .IsSuccess() ||
      !Dual(State)
           .Register("StrictInteger",
                     [&](int Value) {
                       ++IntegerCalls;
                       return Value;
                     })
           .IsSuccess() ||
      !Dual(State)
           .Register("MeasureString",
                     [&](std::string Value) {
                       ++MeasureCalls;
                       return static_cast<int>(Value.size());
                     })
           .IsSuccess())
    return false;

  for (const auto &Expectation : Expectations) {
    const auto Result = State.Execute(Expectation.Script);
    if (!ExecutionFailed(Result, Expectation.Fragments))
      return false;

    const auto Restoration = Hooks::ObserveLastCallbackStackRestoration(State);
    if (!Restoration || Restoration->RestoredDepth != Restoration->EntryDepth ||
        Restoration->ErrorDepth != Restoration->EntryDepth + 1)
      return false;
  }

  return StrictCalls == 0 && IntegerCalls == 0 && MeasureCalls == 0 &&
         State.Execute("assert(Strict(true, 7, 'ok') == 'ok:7')").IsSuccess() &&
         StrictCalls == 1;
}

[[nodiscard]] bool TestExceptionTranslationAndRecovery() {
  Luna::State State;
  int StandardCalls = 0;
  int UnknownCalls = 0;
  int SurvivorCalls = 0;
  if (!State.IsReady() ||
      !Dual(State)
           .Register("ThrowStandard",
                     [&]() -> int {
                       ++StandardCalls;
                       throw std::runtime_error("matrix exception detail");
                     })
           .IsSuccess() ||
      !Dual(State)
           .Register("ThrowUnknown",
                     [&]() -> int {
                       ++UnknownCalls;
                       throw 17;
                     })
           .IsSuccess() ||
      !Dual(State)
           .Register("Survivor",
                     [&] {
                       ++SurvivorCalls;
                       return 5;
                     })
           .IsSuccess())
    return false;

  const auto Standard = State.Execute("ThrowStandard()");
  const auto Unknown = State.Execute("ThrowUnknown()");
  const auto Compilation = State.Execute("local =");
  const auto ScriptError = State.Execute("error('script failure')");

  return ExecutionFailed(Standard,
                         {"Runtime error:", "callable 'ThrowStandard'",
                          "threw: matrix exception detail"}) &&
         ExecutionFailed(Unknown, {"callable 'ThrowUnknown'",
                                   "threw an unknown C++ exception"}) &&
         !Compilation.IsSuccess() &&
         DiagnosticMatches(Compilation.Diagnostic(),
                           Luna::ErrorCategory::Compilation, {}) &&
         ExecutionFailed(ScriptError, {"script failure"}) &&
         StandardCalls == 1 && UnknownCalls == 1 &&
         State.Execute("assert(Survivor() == 5)").IsSuccess() &&
         SurvivorCalls == 1;
}

[[nodiscard]] bool TestStackRestorationAndReuse() {
  Luna::State State;
  if (!State.IsReady() || !Hooks::SetRootStackDepth(State, 3))
    return false;

  const auto EntryDepth = Hooks::ObserveRootStackDepth(State);
  if (!EntryDepth)
    return false;

  int Calls = 0;
  if (!Dual(State)
           .Register("Balanced",
                     [&](int Value) {
                       ++Calls;
                       return Value;
                     })
           .IsSuccess() ||
      Hooks::ObserveRootStackDepth(State) != EntryDepth)
    return false;

  static constexpr std::array<std::string_view, 5> Scripts{
      "Balanced()", "Balanced('wrong')", "Balanced(1.5)",
      "local =", "error('failure')"};
  for (const auto Script : Scripts) {
    if (State.Execute(Script).IsSuccess() ||
        Hooks::ObserveRootStackDepth(State) != EntryDepth)
      return false;
  }

  const auto Invalid = Dual(State).Register("Bad Name", [] {});

  const auto Duplicate =
      Dual(State).Register("Balanced", [](int Value) { return Value; });
  if (Invalid.IsSuccess() || Duplicate.IsSuccess() ||
      Hooks::ObserveRootStackDepth(State) != EntryDepth ||
      Hooks::PendingBindingCount(State) != 0)
    return false;

  return State.Execute("assert(Balanced(9) == 9)").IsSuccess() && Calls == 1 &&
         Hooks::ObserveRootStackDepth(State) == EntryDepth;
}

[[nodiscard]] bool TestStateIsolation() {
  Luna::State Owner;
  Luna::State Other;
  int OwnerCalls = 0;
  int OtherCalls = 0;
  if (!Owner.IsReady() || !Other.IsReady() ||
      !Dual(Owner)
           .Register("Shared",
                     [&] {
                       ++OwnerCalls;
                       return 11;
                     })
           .IsSuccess())
    return false;

  if (Other.Execute("Shared()").IsSuccess() || OwnerCalls != 0 ||
      Hooks::BindingCount(Other) != 0)
    return false;

  if (!Dual(Other)
           .Register("Shared",
                     [&] {
                       ++OtherCalls;
                       return 22;
                     })
           .IsSuccess())
    return false;

  return Owner.Execute("assert(Shared() == 11)").IsSuccess() &&
         Other.Execute("assert(Shared() == 22)").IsSuccess() &&
         OwnerCalls == 1 && OtherCalls == 1;
}

[[nodiscard]] bool TestReturnShapes() {
  Luna::State State;
  int VoidCalls = 0;
  if (!State.IsReady() ||
      !Dual(State).Register("ReturnVoid", [&] { ++VoidCalls; }).IsSuccess() ||
      !Dual(State).Register("ReturnBoolean", [] { return true; }).IsSuccess() ||
      !Dual(State).Register("ReturnInteger", [] { return 7; }).IsSuccess() ||
      !Dual(State).Register("ReturnNumber", [] { return 0.25; }).IsSuccess() ||
      !Dual(State)
           .Register("ReturnString", [] { return std::string("text"); })
           .IsSuccess())
    return false;

  const auto Execution =
      State.Execute("assert(select('#', ReturnVoid()) == 0)\n"
                    "assert(select('#', ReturnBoolean()) == 1)\n"
                    "assert(select('#', ReturnInteger()) == 1)\n"
                    "assert(select('#', ReturnNumber()) == 1)\n"
                    "assert(select('#', ReturnString()) == 1)\n"
                    "assert(ReturnVoid() == nil)\n"
                    "assert(ReturnInteger() == 7)\n"
                    "assert(ReturnString() == 'text')");

  return Execution.IsSuccess() && VoidCalls == 2;
}

} // namespace

int RunFoundationCompatibilityMatrixTests() {
  for (const RegistrationApi Api :
       {RegistrationApi::LegacyRegister,
        RegistrationApi::ExplicitRegisterFunction}) {
    ActiveApi = Api;
    const int Pass = Api == RegistrationApi::LegacyRegister ? 0 : 100;

    if (!TestAcceptedSourceForms())
      return Pass + 1;
    if (!TestSupportedValues())
      return Pass + 2;
    if (!TestRegistrationDiagnostics())
      return Pass + 3;
    if (!TestFirstFailureDiagnostics())
      return Pass + 4;
    if (!TestExceptionTranslationAndRecovery())
      return Pass + 5;
    if (!TestStackRestorationAndReuse())
      return Pass + 6;
    if (!TestStateIsolation())
      return Pass + 7;
    if (!TestReturnShapes())
      return Pass + 8;
  }
  ActiveApi = RegistrationApi::LegacyRegister;
  return 0;
}
