// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "declared parameter integration check failed: " << Description
            << '\n';
}

int ScaleCalls = 0;
int OffsetCalls = 0;
int SumCalls = 0;
int JoinCalls = 0;
int OptionalDefaultCalls = 0;

void ResetCalls() {
  ScaleCalls = 0;
  OffsetCalls = 0;
  SumCalls = 0;
  JoinCalls = 0;
  OptionalDefaultCalls = 0;
}

[[nodiscard]] int Scale(int Value, std::optional<int> Factor) {
  ++ScaleCalls;
  return Value * (Factor ? *Factor : 1);
}

[[nodiscard]] int Offset(int Value, int Amount) {
  ++OffsetCalls;
  return Value + Amount;
}

[[nodiscard]] int OptionalDefault(std::optional<int> Value) {
  ++OptionalDefaultCalls;
  return Value ? *Value : -1;
}

[[nodiscard]] int Sum(Luna::ArgumentView Arguments) {
  ++SumCalls;
  int Total = 0;
  for (std::size_t Index = 0; Index < Arguments.Size(); ++Index) {
    const std::optional<double> Number = Arguments.ToNumber(Index);
    if (Number)
      Total += static_cast<int>(*Number);
  }
  return Total;
}

Luna::ArgumentPack LastJoined;

[[nodiscard]] std::string Join(std::string Separator,
                               Luna::ArgumentPack Arguments) {
  ++JoinCalls;
  LastJoined = Arguments;
  std::string Joined;
  for (std::size_t Index = 0; Index < Arguments.Size(); ++Index) {
    const Luna::OwnedValue Element = Arguments.At(Index);
    const std::optional<std::string> Text = Element.ToText();
    if (Index != 0)
      Joined += Separator;
    Joined += Text ? *Text : std::string();
  }
  return Joined;
}

[[nodiscard]] bool RegisterDeclaredShapes(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  const bool Optional = Registry.RegisterFunction("Scale", &Scale).IsSuccess();
  const bool Defaulted =
      Registry.RegisterFunction("Offset", Luna::WithDefaults(&Offset, 5))
          .IsSuccess();
  const bool DefaultedOptional =
      Registry
          .RegisterFunction("OptionalDefault",
                            Luna::WithDefaults(&OptionalDefault, 7))
          .IsSuccess();
  const bool View = Registry.RegisterFunction("Sum", &Sum).IsSuccess();
  const bool Pack = Registry.RegisterFunction("Join", &Join).IsSuccess();
  return Optional && Defaulted && DefaultedOptional && View && Pack;
}

[[nodiscard]] std::string ExecutionFailure(Luna::State &Owner,
                                           std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return std::string();
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  return Diagnostic ? Diagnostic->Message() : std::string("<no diagnostic>");
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "declared parameter source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

[[nodiscard]] bool RestoredCallbackCheckpoint(const Luna::State &Owner) {
  const auto Observation = Hooks::ObserveLastCallbackStackRestoration(Owner);
  return Observation.has_value() &&
         Observation->EntryDepth == Observation->RestoredDepth &&
         Observation->ErrorDepth == Observation->RestoredDepth + 1;
}

void CheckOptionalAndDefaultedCalls() {
  ResetCalls();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterDeclaredShapes(Owner),
        "every declared shape registers through RegisterFunction");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(
      Succeeds(Owner, "assert(Scale(3) == 3, 'omitted optional')\n"
                      "assert(Scale(3, nil) == 3, 'explicit nil optional')\n"
                      "assert(Scale(3, 2) == 6, 'present optional')\n"),
      "a trailing optional maps omission and explicit nil to the empty value");
  Check(ScaleCalls == 3, "each optional call invokes the target exactly once");

  Check(Succeeds(Owner, "assert(Offset(1) == 6, 'omitted default')\n"
                        "assert(Offset(1, 2) == 3, 'supplied argument')\n"),
        "a default applies only when its parameter is omitted");
  Check(OffsetCalls == 2,
        "each defaulted call invokes the target exactly once");

  Check(Succeeds(Owner,
                 "assert(OptionalDefault() == 7, 'default materialized')\n"
                 "assert(OptionalDefault(nil) == -1, 'explicit nil supplied')\n"
                 "assert(OptionalDefault(4) == 4, 'present value')\n"),
        "an explicit nil is a supplied value rather than an omitted one");
  Check(OptionalDefaultCalls == 3,
        "a defaulted optional invokes the target once per call");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "successful declared-shape calls restore the exact root stack depth");
}

void CheckVariadicCalls() {
  ResetCalls();
  LastJoined = Luna::ArgumentPack();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterDeclaredShapes(Owner),
        "the variadic shapes register before their calls");

  Check(Succeeds(Owner, "assert(Sum() == 0, 'empty variadic')\n"
                        "assert(Sum(1, 2, 3) == 6, 'three variadic values')\n"
                        "assert(Sum(4) == 4, 'one variadic value')\n"),
        "a callback-lifetime view receives every remaining call argument");
  Check(SumCalls == 3, "each variadic call invokes the target exactly once");

  Check(Succeeds(Owner, "assert(Join('-') == '', 'no variadic values')\n"
                        "assert(Join('-', 'a', 'b') == 'a-b', 'two values')\n"),
        "an owning pack receives the variadic tail after the fixed parameters");
  Check(JoinCalls == 2, "each pack call invokes the target exactly once");
  Check(LastJoined.Size() == 2 && LastJoined.FirstPosition() == 2 &&
            LastJoined.Position(0) == 2,
        "a retained pack keeps its values and one-based call positions");

  const Luna::OwnedValue First = LastJoined.At(0);
  Check(First.ToText() == std::optional<std::string>("a"),
        "a retained pack still reads its values after the call returned");
}

void CheckRefusedCallsAndRecovery() {
  ResetCalls();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterDeclaredShapes(Owner),
        "every declared shape registers before the failure matrix");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  const std::string TooFew = ExecutionFailure(Owner, "return Scale()");
  Check(TooFew.find("between 1 and 2 arguments") != std::string::npos &&
            TooFew.find("received 0") != std::string::npos,
        "a shape with an omittable parameter reports its accepted arity range");
  Check(ScaleCalls == 0, "an arity refusal never invokes the target");
  Check(RestoredCallbackCheckpoint(Owner),
        "an arity refusal restores the exact callback checkpoint");

  const std::string TooMany = ExecutionFailure(Owner, "return Scale(1, 2, 3)");
  Check(TooMany.find("between 1 and 2 arguments") != std::string::npos &&
            TooMany.find("received 3") != std::string::npos,
        "a non-variadic shape refuses more arguments than it accepts");

  const std::string WrongType = ExecutionFailure(Owner, "return Scale('x')");
  Check(WrongType.find("argument 1") != std::string::npos &&
            WrongType.find("received string") != std::string::npos,
        "a supplied argument keeps the foundation's type diagnostic");
  Check(ScaleCalls == 0, "a refused conversion never invokes the target");

  const std::string RefusedDefault =
      ExecutionFailure(Owner, "return Offset('x')");
  Check(RefusedDefault.find("argument 1") != std::string::npos,
        "a refused call names the supplied argument that failed");
  Check(OffsetCalls == 0,
        "no default is materialized and no target runs for a refused call");

  const std::string FirstVariadic =
      ExecutionFailure(Owner, "return Sum(1, {}, {})");
  Check(FirstVariadic.find("argument 2") != std::string::npos &&
            FirstVariadic.find("received table") != std::string::npos,
        "a variadic refusal names the first failing one-based call position");
  Check(SumCalls == 0, "a refused variadic call never invokes the target");

  const std::string LaterVariadic =
      ExecutionFailure(Owner, "return Join('-', 'a', {})");
  Check(LaterVariadic.find("argument 3") != std::string::npos,
        "the variadic tail is numbered by its call position, not by its index");
  Check(JoinCalls == 0, "a refused pack call never invokes the target");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every refused declared-shape call restores the root stack depth");

  Check(Succeeds(Owner, "assert(Scale(2, 3) == 6)\n"
                        "assert(Offset(1) == 6)\n"
                        "assert(Sum(1, 2) == 3)\n"
                        "assert(Join('+', 'x') == 'x')\n"),
        "the State stays reusable after every refused declared-shape call");
  Check(ScaleCalls == 1 && OffsetCalls == 1 && SumCalls == 1 && JoinCalls == 1,
        "each recovered call invokes its target exactly once");
}

} // namespace

int RunDeclaredParameterIntegrationTests() {
  FailureCount = 0;
  CheckOptionalAndDefaultedCalls();
  CheckVariadicCalls();
  CheckRefusedCallsAndRecovery();
  return FailureCount == 0 ? 0 : 1;
}
