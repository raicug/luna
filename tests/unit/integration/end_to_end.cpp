// clang-format off
#include <luna/luna.hpp>

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
// clang-format on

namespace {

int FreeBooleanCalls = 0;
int PointerIntegerCalls = 0;

bool NegateBoolean(bool Value) {
  ++FreeBooleanCalls;
  return !Value;
}

int AddIntegers(int Left, int Right) {
  ++PointerIntegerCalls;
  return Left + Right;
}

[[nodiscard]] bool
HasRegistrationFailure(const Luna::RegistrationResult &Result,
                       Luna::ErrorCategory Category) {
  return !Result.IsSuccess() && Result.Diagnostic() &&
         Result.Diagnostic()->Category() == Category &&
         !Result.Diagnostic()->Message().empty();
}

[[nodiscard]] bool
HasExecutionFailure(const Luna::ExecutionResult &Result,
                    std::initializer_list<std::string_view> Contexts) {
  if (Result.IsSuccess() || !Result.Diagnostic() ||
      Result.Diagnostic()->Category() != Luna::ErrorCategory::Runtime)
    return false;

  for (const auto Context : Contexts) {
    if (Result.Diagnostic()->Message().find(Context) == std::string::npos)
      return false;
  }
  return true;
}
[[nodiscard]] bool TestCallableAndReturnMatrix() {
  FreeBooleanCalls = 0;
  PointerIntegerCalls = 0;
  int ZeroCalls = 0;
  int NumberCalls = 0;
  int TripleCalls = 0;
  int QuadCalls = 0;
  int VoidCalls = 0;
  bool ObservedFlag = false;
  int ObservedInteger = 0;
  double ObservedNumber = 0.0;
  std::string ObservedText;

  Luna::State State;
  auto *AddPointer = &AddIntegers;
  if (!State.IsReady() ||
      !State.Bindings()
           .Register("ZeroArity",
                     [&ZeroCalls]() {
                       ++ZeroCalls;
                       return 73;
                     })
           .IsSuccess() ||
      !State.Bindings().Register("NegateBoolean", NegateBoolean).IsSuccess() ||
      !State.Bindings().Register("AddIntegers", AddPointer).IsSuccess() ||
      !State.Bindings()
           .Register("HalfNumber",
                     [&NumberCalls](double Value) {
                       ++NumberCalls;
                       return Value / 2.0;
                     })
           .IsSuccess() ||
      !State.Bindings()
           .Register("ComposeTriple",
                     [&TripleCalls](double, std::string Text, bool Enabled) {
                       ++TripleCalls;
                       return Text + (Enabled ? ":on" : ":off");
                     })
           .IsSuccess() ||
      !State.Bindings()
           .Register(
               "ObserveQuad",
               [&](bool Flag, int Integer, double Number, std::string Text) {
                 ++QuadCalls;
                 ObservedFlag = Flag;
                 ObservedInteger = Integer;
                 ObservedNumber = Number;
                 ObservedText = std::move(Text);
               })
           .IsSuccess() ||
      !State.Bindings()
           .Register("ReturnVoid", [&VoidCalls]() { ++VoidCalls; })
           .IsSuccess())
    return false;
  const auto Execution = State.Execute(
      "assert(ZeroArity() == 73)\n"
      "assert(NegateBoolean(false) == true)\n"
      "assert(AddIntegers(19, 23) == 42)\n"
      "assert(HalfNumber(9.0) == 4.5)\n"
      "assert(ComposeTriple(2.5, 'matrix', true) == 'matrix:on')\n"
      "ObserveQuad(true, -17, 3.25, '\\x41\\x00\\x42')\n"
      "assert(select('#', ReturnVoid()) == 0)\n"
      "assert(select('#', ZeroArity()) == 1)");

  return Execution.IsSuccess() && !Execution.Diagnostic() &&
         FreeBooleanCalls == 1 && PointerIntegerCalls == 1 && ZeroCalls == 2 &&
         NumberCalls == 1 && TripleCalls == 1 && QuadCalls == 1 &&
         VoidCalls == 1 && ObservedFlag && ObservedInteger == -17 &&
         ObservedNumber == 3.25 && ObservedText == std::string("A\0B", 3);
}

[[nodiscard]] bool TestRegistrationMatrix() {
  Luna::State State;
  if (!State.IsReady())
    return false;

  const std::string Overlong(256, 'A');
  std::string NonAscii = "Name";
  NonAscii.append("\xC3\xA9", 2);
  const std::string EmbeddedZero("Good\0Name", 9);
  const auto Empty = State.Bindings().Register("", []() {});
  const auto TooLong = State.Bindings().Register(Overlong, []() {});
  const auto NonAsciiName = State.Bindings().Register(NonAscii, []() {});
  const auto IllegalFirst = State.Bindings().Register("7Name", []() {});
  const auto IllegalLater = State.Bindings().Register("Bad-Name", []() {});
  const auto ZeroByte = State.Bindings().Register(EmbeddedZero, []() {});
  if (!HasRegistrationFailure(Empty, Luna::ErrorCategory::InvalidGlobalName) ||
      !HasRegistrationFailure(TooLong,
                              Luna::ErrorCategory::InvalidGlobalName) ||
      !HasRegistrationFailure(NonAsciiName,
                              Luna::ErrorCategory::InvalidGlobalName) ||
      !HasRegistrationFailure(IllegalFirst,
                              Luna::ErrorCategory::InvalidGlobalName) ||
      !HasRegistrationFailure(IllegalLater,
                              Luna::ErrorCategory::InvalidGlobalName) ||
      !HasRegistrationFailure(ZeroByte, Luna::ErrorCategory::InvalidGlobalName))
    return false;
  int OriginalCalls = 0;
  int ReplacementCalls = 0;
  const auto Original = State.Bindings().Register("PreservedDuplicate", [&]() {
    ++OriginalCalls;
    return 17;
  });
  const auto Duplicate = State.Bindings().Register("PreservedDuplicate", [&]() {
    ++ReplacementCalls;
    return 99;
  });
  const auto Execution = State.Execute("assert(PreservedDuplicate() == 17)");
  return Original.IsSuccess() &&
         HasRegistrationFailure(Duplicate,
                                Luna::ErrorCategory::DuplicateGlobalName) &&
         Execution.IsSuccess() && OriginalCalls == 1 && ReplacementCalls == 0;
}

[[nodiscard]] bool TestValidationAndExceptionMatrix() {
  Luna::State State;
  int IntegerCalls = 0;
  if (!State.IsReady() ||
      !State.Bindings()
           .Register("StrictInteger",
                     [&](int Value) {
                       ++IntegerCalls;
                       return Value;
                     })
           .IsSuccess() ||
      !State.Bindings()
           .Register("ThrowStandard",
                     []() -> int {
                       throw std::runtime_error("matrix exception detail");
                     })
           .IsSuccess())
    return false;

  const auto Count = State.Execute("StrictInteger()");
  const auto Type = State.Execute("StrictInteger('wrong')");
  const auto NonFinite = State.Execute("StrictInteger(math.huge)");
  const auto Range = State.Execute("StrictInteger(2147483648)");
  const auto Fraction = State.Execute("StrictInteger(1.5)");
  const auto Exception = State.Execute("ThrowStandard()");

  return HasExecutionFailure(Count,
                             {"StrictInteger", "expected 1", "received 0"}) &&
         HasExecutionFailure(Type, {"StrictInteger", "argument 1",
                                    "expected signed 32-bit integer",
                                    "received string"}) &&
         HasExecutionFailure(NonFinite,
                             {"StrictInteger", "finite signed 32-bit integer",
                              "positive infinity"}) &&
         HasExecutionFailure(
             Range, {"StrictInteger", "signed 32-bit range", "2147483648"}) &&
         HasExecutionFailure(Fraction,
                             {"StrictInteger", "integral value", "1.5"}) &&
         HasExecutionFailure(Exception,
                             {"ThrowStandard", "matrix exception detail"}) &&
         IntegerCalls == 0;
}
[[nodiscard]] bool TestStateIsolation() {
  Luna::State Owner;
  Luna::State Other;
  int OwnerCalls = 0;
  int OtherCalls = 0;
  if (!Owner.IsReady() || !Other.IsReady() ||
      !Owner.Bindings()
           .Register("IsolatedGlobal",
                     [&]() {
                       ++OwnerCalls;
                       return 11;
                     })
           .IsSuccess())
    return false;

  const auto Absent = Other.Execute("IsolatedGlobal()");
  if (!HasExecutionFailure(Absent, {"attempt to call a nil value"}) ||
      OwnerCalls != 0)
    return false;

  if (!Other.Bindings()
           .Register("IsolatedGlobal",
                     [&]() {
                       ++OtherCalls;
                       return 22;
                     })
           .IsSuccess())
    return false;

  const auto OwnerExecution = Owner.Execute("assert(IsolatedGlobal() == 11)");
  const auto OtherExecution = Other.Execute("assert(IsolatedGlobal() == 22)");
  return OwnerExecution.IsSuccess() && OtherExecution.IsSuccess() &&
         OwnerCalls == 1 && OtherCalls == 1;
}

} // namespace

int RunEndToEndRegistrationInvocationMatrixTests() {
  if (!TestCallableAndReturnMatrix())
    return 1;
  if (!TestRegistrationMatrix())
    return 2;
  if (!TestValidationAndExceptionMatrix())
    return 3;
  if (!TestStateIsolation())
    return 4;
  return 0;
}
