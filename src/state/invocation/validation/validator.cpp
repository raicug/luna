// clang-format off
#include "state/invocation/validation/validator.hpp"

#include "state/invocation/conversion/argument_reader.hpp"
#include "state/testing/fault_injector.hpp"

#include <lua.h>

#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] std::string CallableContext(std::string_view GlobalName) {
  return "Callable '" + std::string(GlobalName) + "'";
}

[[nodiscard]] std::string FormatNumber(double Number) {
  if (std::isnan(Number))
    return "NaN";
  if (std::isinf(Number))
    return std::signbit(Number) ? "negative infinity" : "positive infinity";

  std::ostringstream Stream;
  Stream << std::setprecision(std::numeric_limits<double>::max_digits10)
         << Number;
  return Stream.str();
}

[[nodiscard]] bool IsSupportedKind(ValueKind Kind) noexcept {
  switch (Kind) {
  case ValueKind::Boolean:
  case ValueKind::Integer:
  case ValueKind::Number:
  case ValueKind::String:
    return true;
  }
  return false;
}

[[nodiscard]] bool MetadataIsConsistent(const CallableMetadata &Metadata) {
  for (const ValueKind Kind : Metadata.ParameterTypes()) {
    if (!IsSupportedKind(Kind))
      return false;
  }

  const ReturnMetadata &Return = Metadata.ReturnType();
  if (Return.Disposition() == ReturnDisposition::Value)
    return Return.Kind() && IsSupportedKind(*Return.Kind());
  return Return.Kind() == nullptr;
}

void RecordReadFailure(InvocationValidationResult &Validation,
                       const ArgumentReadResult &Read,
                       std::string_view GlobalName, std::size_t Position,
                       ValueKind ExpectedKind) {
  const std::string Prefix = CallableContext(GlobalName) + " argument " +
                             std::to_string(Position) + " ";
  switch (Read.Status) {
  case ArgumentReadStatus::TypeMismatch:
    Validation.RecordCallerFailure(Prefix + "expected " +
                                   ValueKindName(ExpectedKind) +
                                   " but received " + Read.ReceivedType + ".");
    return;
  case ArgumentReadStatus::IntegerNonFinite:
    Validation.RecordCallerFailure(
        Prefix + "expected a finite signed 32-bit integer but received " +
        FormatNumber(Read.ReceivedNumber) + ".");
    return;
  case ArgumentReadStatus::IntegerOutOfRange:
    Validation.RecordCallerFailure(
        Prefix +
        "expected signed 32-bit range [-2147483648, 2147483647] "
        "but received " +
        FormatNumber(Read.ReceivedNumber) + ".");
    return;
  case ArgumentReadStatus::IntegerFractional:
    Validation.RecordCallerFailure(Prefix +
                                   "expected an integral value but received " +
                                   FormatNumber(Read.ReceivedNumber) + ".");
    return;
  case ArgumentReadStatus::StringTooLong:
    Validation.RecordCallerFailure(
        Prefix + "received " + std::to_string(Read.ReceivedByteCount) +
        " string bytes; maximum is " +
        std::to_string(MaximumInvocationStringBytes) + ".");
    return;
  case ArgumentReadStatus::InternalFailure:
    Validation.RecordInternalFailure("Internal error while inspecting " +
                                     Prefix + "for validation.");
    return;
  case ArgumentReadStatus::Success:
    Validation.RecordInternalFailure(
        "Internal error: successful argument conversion had no value for " +
        CallableContext(GlobalName) + ".");
    return;
  }
}

} // namespace

ValidatedInvocation ValidateInvocation(lua_State *State,
                                       std::string_view GlobalName,
                                       const CallableMetadata *Metadata,
                                       FaultInjector &Faults) noexcept {
  ValidatedInvocation Result;
  try {
    if (Faults.Consume(StateFaultPoint::MissingMetadata) || !Metadata) {
      Result.Validation.RecordInternalFailure(
          "Internal error: callable metadata is unavailable for " +
          CallableContext(GlobalName) + ".");
      return Result;
    }

    if (!MetadataIsConsistent(*Metadata)) {
      Result.Validation.RecordInternalFailure(
          "Internal error: callable metadata is inconsistent for " +
          CallableContext(GlobalName) + ".");
      return Result;
    }

    if (!State) {
      Result.Validation.RecordInternalFailure(
          "Internal error: argument stack is unavailable for " +
          CallableContext(GlobalName) + ".");
      return Result;
    }

    const auto Parameters = Metadata->ParameterTypes();
    const int ReceivedCount = lua_gettop(State);
    if (ReceivedCount != static_cast<int>(Parameters.size())) {
      Result.Validation.RecordCallerFailure(
          CallableContext(GlobalName) + " expected " +
          std::to_string(Parameters.size()) + " arguments but received " +
          std::to_string(ReceivedCount) + ".");
      return Result;
    }

    Result.Arguments.reserve(Parameters.size());
    for (std::size_t Index = 0; Index < Parameters.size(); ++Index) {
      const bool InjectInspectionFailure =
          Faults.Consume(StateFaultPoint::ArgumentInspection);
      auto Read = ReadArgument(State, static_cast<int>(Index + 1),
                               Parameters[Index], InjectInspectionFailure);
      if (!Read.IsSuccess()) {
        RecordReadFailure(Result.Validation, Read, GlobalName, Index + 1,
                          Parameters[Index]);
        Result.Arguments.clear();
        return Result;
      }
      Result.Arguments.push_back(std::move(*Read.ConvertedValue));
    }
  } catch (...) {
    Result.Arguments.clear();
    try {
      Result.Validation.RecordInternalFailure(
          "Internal error while validating " + CallableContext(GlobalName) +
          ".");
    } catch (...) {
    }
  }
  return Result;
}

} // namespace Luna::Detail
