// clang-format off
#include <luna/luna.hpp>

#include "state/invocation/conversion/argument_reader.hpp"
#include "state/invocation/conversion/return_writer.hpp"
#include "state/invocation/testing/test_hooks.hpp"
#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace {

using FaultPoint = Luna::Detail::StateFaultPoint;
using Hooks = Luna::Detail::StateTestHooks;
using Observation = Luna::Detail::NativeInvocationObservation;
using PrimitiveHooks = Luna::Detail::InvocationPrimitiveTestHooks;
using ValidationState = Luna::Detail::InvocationValidationState;
using WriteStatus = Luna::Detail::ReturnWriteStatus;

[[nodiscard]] bool Contains(const Observation &Result, std::string_view Text) {
  return Result.ErrorMessage.find(Text) != std::string::npos;
}

[[nodiscard]] bool
HasContexts(const Observation &Result,
            std::initializer_list<std::string_view> Contexts) {
  if (Result.ErrorMessage.empty())
    return false;
  for (const std::string_view Context : Contexts) {
    if (!Contains(Result, Context))
      return false;
  }
  return true;
}

[[nodiscard]] bool IsBalancedSuccess(const Observation &Result,
                                     int ReturnCount) {
  return Result.Succeeded && Result.ReturnCount == ReturnCount &&
         Result.ErrorMessage.empty() &&
         Result.CompletionStackDepth == Result.EntryStackDepth + ReturnCount &&
         Result.FinalStackDepth == Result.EntryStackDepth;
}

[[nodiscard]] bool IsBalancedFailure(const Observation &Result) {
  return !Result.Succeeded && Result.ReturnCount == 0 &&
         !Result.ReturnedValue && !Result.ErrorMessage.empty() &&
         Result.CompletionStackDepth == Result.EntryStackDepth + 1 &&
         Result.FinalStackDepth == Result.EntryStackDepth;
}

[[nodiscard]] bool TestEverySupportedArgumentAndReturn() {
  Luna::State State;
  if (!State.IsReady() || !Hooks::SetRootStackDepth(State, 3))
    return false;

  int ArgumentCalls = 0;
  bool SeenBoolean = false;
  int SeenInteger = 0;
  double SeenNumber = 0.0;
  std::string SeenString;
  if (!State.Bindings()
           .Register("AllArguments",
                     [&](bool Boolean, int Integer, double Number,
                         std::string String) {
                       ++ArgumentCalls;
                       SeenBoolean = Boolean;
                       SeenInteger = Integer;
                       SeenNumber = Number;
                       SeenString = std::move(String);
                     })
           .IsSuccess() ||
      !State.Bindings()
           .Register("BooleanReturn", []() { return true; })
           .IsSuccess() ||
      !State.Bindings()
           .Register("IntegerReturn", []() { return -2147483647; })
           .IsSuccess() ||
      !State.Bindings()
           .Register("NumberReturn", []() { return 19.25; })
           .IsSuccess() ||
      !State.Bindings()
           .Register("StringReturn", []() { return std::string("a\0b", 3); })
           .IsSuccess() ||
      !State.Bindings().Register("VoidReturn", []() {}).IsSuccess())
    return false;

  const std::string Embedded("left\0right", 10);
  const auto Arguments = Hooks::InvokeBinding(
      State, "AllArguments",
      {true, std::numeric_limits<int>::min(), -0.0, Embedded});
  if (!IsBalancedSuccess(Arguments, 0) || ArgumentCalls != 1 || !SeenBoolean ||
      SeenInteger != std::numeric_limits<int>::min() || SeenNumber != 0.0 ||
      !std::signbit(SeenNumber) || SeenString != Embedded)
    return false;

  const auto BoundaryArguments = Hooks::InvokeBinding(
      State, "AllArguments",
      {false, std::numeric_limits<int>::max(), 42.5, std::string{}});
  const auto Boolean = Hooks::InvokeBinding(State, "BooleanReturn", {});
  const auto Integer = Hooks::InvokeBinding(State, "IntegerReturn", {});
  const auto Number = Hooks::InvokeBinding(State, "NumberReturn", {});
  const auto String = Hooks::InvokeBinding(State, "StringReturn", {});
  const auto Void = Hooks::InvokeBinding(State, "VoidReturn", {});

  return IsBalancedSuccess(BoundaryArguments, 0) && ArgumentCalls == 2 &&
         !SeenBoolean && SeenInteger == std::numeric_limits<int>::max() &&
         SeenNumber == 42.5 && SeenString.empty() &&
         IsBalancedSuccess(Boolean, 1) && Boolean.ReturnedValue &&
         std::get<bool>(*Boolean.ReturnedValue) &&
         IsBalancedSuccess(Integer, 1) && Integer.ReturnedValue &&
         std::get<int>(*Integer.ReturnedValue) == -2147483647 &&
         IsBalancedSuccess(Number, 1) && Number.ReturnedValue &&
         std::get<double>(*Number.ReturnedValue) == 19.25 &&
         IsBalancedSuccess(String, 1) && String.ReturnedValue &&
         std::get<std::string>(*String.ReturnedValue) ==
             std::string("a\0b", 3) &&
         IsBalancedSuccess(Void, 0) && !Void.ReturnedValue;
}

[[nodiscard]] bool TestSpecialDoubleArgumentsAndReturns() {
  Luna::State State;
  double Seen = 1.0;
  int Calls = 0;
  if (!State.IsReady() || !State.Bindings()
                               .Register("EchoNumber",
                                         [&](double Value) {
                                           ++Calls;
                                           Seen = Value;
                                           return Value;
                                         })
                               .IsSuccess())
    return false;

  constexpr std::array Values{
      0.0,
      -0.0,
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::bit_cast<double>(std::uint64_t{0x7ff8000000000001ULL}),
      std::bit_cast<double>(std::uint64_t{0xfff8000000001234ULL}),
  };

  for (const double Value : Values) {
    const auto Result = Hooks::InvokeBinding(State, "EchoNumber", {Value});
    if (!IsBalancedSuccess(Result, 1) || !Result.ReturnedValue ||
        !std::holds_alternative<double>(*Result.ReturnedValue))
      return false;

    const double Returned = std::get<double>(*Result.ReturnedValue);
    if (std::isnan(Value)) {
      if (!std::isnan(Seen) || !std::isnan(Returned))
        return false;
    } else if (std::isinf(Value)) {
      if (!std::isinf(Seen) || !std::isinf(Returned) ||
          std::signbit(Seen) != std::signbit(Value) ||
          std::signbit(Returned) != std::signbit(Value))
        return false;
    } else if (Seen != Value || Returned != Value ||
               std::signbit(Seen) != std::signbit(Value) ||
               std::signbit(Returned) != std::signbit(Value)) {
      return false;
    }
  }
  return Calls == static_cast<int>(Values.size());
}

[[nodiscard]] bool TestStringBoundariesAndPartialReturnSuppression() {
  Luna::State State;
  if (!State.IsReady() || !Hooks::SetRootStackDepth(State, 2))
    return false;

  int ArgumentCalls = 0;
  int OversizedReturnCalls = 0;
  const std::string Maximum(Luna::Detail::MaximumInvocationStringBytes, 'm');
  const std::string Oversized(Luna::Detail::MaximumInvocationStringBytes + 1,
                              'x');
  if (!State.Bindings()
           .Register("EchoString",
                     [&](std::string Value) {
                       ++ArgumentCalls;
                       return Value;
                     })
           .IsSuccess() ||
      !State.Bindings()
           .Register("OversizedReturn",
                     [&]() {
                       ++OversizedReturnCalls;
                       return Oversized;
                     })
           .IsSuccess())
    return false;

  const std::string Embedded("\0middle\0", 8);
  const auto Empty = Hooks::InvokeBinding(State, "EchoString", {std::string{}});
  const auto EmbeddedResult =
      Hooks::InvokeBinding(State, "EchoString", {Embedded});
  const auto MaximumResult =
      Hooks::InvokeBinding(State, "EchoString", {Maximum});
  if (!IsBalancedSuccess(Empty, 1) || !Empty.ReturnedValue ||
      !std::get<std::string>(*Empty.ReturnedValue).empty() ||
      !IsBalancedSuccess(EmbeddedResult, 1) || !EmbeddedResult.ReturnedValue ||
      std::get<std::string>(*EmbeddedResult.ReturnedValue) != Embedded ||
      !IsBalancedSuccess(MaximumResult, 1) || !MaximumResult.ReturnedValue ||
      std::get<std::string>(*MaximumResult.ReturnedValue) != Maximum ||
      ArgumentCalls != 3)
    return false;

  const auto OversizedArgument =
      Hooks::InvokeBinding(State, "EchoString", {Oversized});
  const auto OversizedReturned =
      Hooks::InvokeBinding(State, "OversizedReturn", {});
  return IsBalancedFailure(OversizedArgument) &&
         HasContexts(OversizedArgument,
                     {"EchoString", "argument 1", "1048577", "1048576"}) &&
         ArgumentCalls == 3 && IsBalancedFailure(OversizedReturned) &&
         HasContexts(OversizedReturned, {"OversizedReturn", "1048576"}) &&
         OversizedReturnCalls == 1;
}

[[nodiscard]] bool TestValidationFailuresDoNotInvoke() {
  Luna::State State;
  if (!State.IsReady() || !Hooks::SetRootStackDepth(State, 5))
    return false;

  int Calls = 0;
  if (!State.Bindings()
           .Register("StrictInteger",
                     [&](int Value) {
                       ++Calls;
                       return Value;
                     })
           .IsSuccess())
    return false;

  const auto Count = Hooks::InvokeBinding(State, "StrictInteger", {});
  const auto Type = Hooks::InvokeBinding(State, "StrictInteger", {true});
  const auto NaN = Hooks::InvokeBinding(
      State, "StrictInteger", {std::numeric_limits<double>::quiet_NaN()});
  const auto Infinity = Hooks::InvokeBinding(
      State, "StrictInteger", {std::numeric_limits<double>::infinity()});
  const auto AboveRange =
      Hooks::InvokeBinding(State, "StrictInteger", {2147483648.0});
  const auto BelowRange =
      Hooks::InvokeBinding(State, "StrictInteger", {-2147483649.0});
  const auto Fractional = Hooks::InvokeBinding(State, "StrictInteger", {17.5});

  return IsBalancedFailure(Count) &&
         HasContexts(Count, {"StrictInteger", "expected 1", "received 0"}) &&
         IsBalancedFailure(Type) &&
         HasContexts(Type,
                     {"argument 1", "signed 32-bit integer", "boolean"}) &&
         IsBalancedFailure(NaN) && HasContexts(NaN, {"argument 1", "NaN"}) &&
         IsBalancedFailure(Infinity) &&
         HasContexts(Infinity, {"argument 1", "infinity"}) &&
         IsBalancedFailure(AboveRange) &&
         HasContexts(AboveRange, {"argument 1", "[-2147483648, 2147483647]"}) &&
         IsBalancedFailure(BelowRange) &&
         HasContexts(BelowRange, {"argument 1", "[-2147483648, 2147483647]"}) &&
         IsBalancedFailure(Fractional) &&
         HasContexts(Fractional, {"argument 1", "integral"}) && Calls == 0;
}

[[nodiscard]] bool TestAmbiguousAndSuppressedReturnMetadata() {
  // Suppress is the defensive representation for a return that cannot be
  // classified unambiguously as value-producing or void.
  const auto AmbiguousReturn =
      PrimitiveHooks::Write(Luna::ReturnMetadata::Suppressed(),
                            Luna::InvocationOutcome::WithValue(41), 0, 0, 0, 6);
  const auto SuppressedFailure = PrimitiveHooks::Write(
      Luna::ReturnMetadata::Suppressed(),
      Luna::InvocationOutcome::InternalFailure("ambiguous return metadata"), 0,
      0, 0, 7);

  Luna::CallableMetadata Inconsistent(
      {}, Luna::ReturnMetadata::ForValue(static_cast<Luna::ValueKind>(255)));
  const auto Invalid =
      PrimitiveHooks::Validate({}, "AmbiguousMetadata", &Inconsistent);

  return AmbiguousReturn.Result.Status == WriteStatus::Suppressed &&
         AmbiguousReturn.Result.ReturnCount == 0 &&
         !AmbiguousReturn.Result.Diagnostic && !AmbiguousReturn.WrittenValue &&
         AmbiguousReturn.StackDepth == 6 &&
         SuppressedFailure.Result.Status == WriteStatus::Suppressed &&
         SuppressedFailure.Result.ReturnCount == 0 &&
         !SuppressedFailure.WrittenValue && SuppressedFailure.StackDepth == 7 &&
         Invalid.Invocation.Validation.State() ==
             ValidationState::InternalError &&
         Invalid.Invocation.Validation.Diagnostic() &&
         Invalid.Invocation.Validation.Diagnostic()->Message().find(
             "inconsistent") != std::string::npos;
}

[[nodiscard]] bool TestFaultsExceptionsAndCallbackCheckpoints() {
  Luna::State State;
  if (!State.IsReady() || !Hooks::SetRootStackDepth(State, 4))
    return false;

  int WriterCalls = 0;
  int VoidCalls = 0;
  int ValidationCalls = 0;
  int StandardCalls = 0;
  int StandardDestroyed = 0;
  int UnknownCalls = 0;
  if (!State.Bindings()
           .Register("WriterFault",
                     [&]() {
                       ++WriterCalls;
                       return 9;
                     })
           .IsSuccess() ||
      !State.Bindings()
           .Register("VoidFault", [&]() { ++VoidCalls; })
           .IsSuccess() ||
      !State.Bindings()
           .Register("ValidationFault",
                     [&](int Value) {
                       ++ValidationCalls;
                       return Value;
                     })
           .IsSuccess() ||
      !State.Bindings()
           .Register("StandardException",
                     [&]() -> int {
                       ++StandardCalls;
                       struct Guard final {
                         int *Destroyed;
                         ~Guard() { ++*Destroyed; }
                       } Lifetime{&StandardDestroyed};
                       throw std::runtime_error("standard edge detail");
                     })
           .IsSuccess() ||
      !State.Bindings()
           .Register("UnknownException",
                     [&]() -> int {
                       ++UnknownCalls;
                       throw 27;
                     })
           .IsSuccess())
    return false;

  Hooks::InjectFault(State, FaultPoint::ReturnWrite);
  const auto Writer = Hooks::InvokeBinding(State, "WriterFault", {});
  Hooks::InjectFault(State, FaultPoint::VoidFinalization);
  const auto Void = Hooks::InvokeBinding(State, "VoidFault", {});
  Hooks::InjectFault(State, FaultPoint::MissingMetadata);
  const auto Missing = Hooks::InvokeBinding(State, "ValidationFault", {11});
  Hooks::InjectFault(State, FaultPoint::ArgumentInspection);
  const auto Inspection = Hooks::InvokeBinding(State, "ValidationFault", {12});
  const auto Standard = Hooks::InvokeBinding(State, "StandardException", {});
  const auto Unknown = Hooks::InvokeBinding(State, "UnknownException", {});

  const auto AtSeededCheckpoint = [](const Observation &Result) {
    return Result.EntryStackDepth == 4 && Result.FinalStackDepth == 4;
  };
  return IsBalancedFailure(Writer) && AtSeededCheckpoint(Writer) &&
         HasContexts(Writer, {"WriterFault", "return-writer"}) &&
         WriterCalls == 1 &&
         Hooks::PendingFaults(State, FaultPoint::ReturnWrite) == 0 &&
         IsBalancedFailure(Void) && AtSeededCheckpoint(Void) &&
         HasContexts(Void, {"VoidFault", "void-finalization"}) &&
         VoidCalls == 1 &&
         Hooks::PendingFaults(State, FaultPoint::VoidFinalization) == 0 &&
         IsBalancedFailure(Missing) && AtSeededCheckpoint(Missing) &&
         HasContexts(Missing, {"ValidationFault", "metadata"}) &&
         Hooks::PendingFaults(State, FaultPoint::MissingMetadata) == 0 &&
         IsBalancedFailure(Inspection) && AtSeededCheckpoint(Inspection) &&
         HasContexts(Inspection, {"ValidationFault", "inspecting"}) &&
         Hooks::PendingFaults(State, FaultPoint::ArgumentInspection) == 0 &&
         ValidationCalls == 0 && IsBalancedFailure(Standard) &&
         AtSeededCheckpoint(Standard) &&
         HasContexts(Standard, {"StandardException", "standard edge detail"}) &&
         StandardCalls == 1 && StandardDestroyed == 1 &&
         IsBalancedFailure(Unknown) && AtSeededCheckpoint(Unknown) &&
         HasContexts(Unknown, {"UnknownException", "unknown C++ exception"}) &&
         UnknownCalls == 1;
}

} // namespace

int RunConversionReturnValidationFaultEdgeCaseTests() {
  if (!TestEverySupportedArgumentAndReturn())
    return 1;
  if (!TestSpecialDoubleArgumentsAndReturns())
    return 2;
  if (!TestStringBoundariesAndPartialReturnSuppression())
    return 3;
  if (!TestValidationFailuresDoNotInvoke())
    return 4;
  if (!TestAmbiguousAndSuppressedReturnMetadata())
    return 5;
  if (!TestFaultsExceptionsAndCallbackCheckpoints())
    return 6;
  return 0;
}
