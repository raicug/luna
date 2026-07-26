// clang-format off
#include "state/invocation/validation/validator.hpp"

#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/invocation/conversion/argument_reader.hpp"
#include "state/testing/fault_injector.hpp"
#include "state/type/conversion_outcome.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"

#include <lua.h>

#include <cmath>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

// The foundation's own subject wording, for the branches that report before any
// metadata is known at all.
[[nodiscard]] std::string CallableContext(std::string_view GlobalName) {
  return DescribeConversionSubject(SubjectForCallable(GlobalName, false));
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

// Metadata is consistent when the captured type generation describes every
// declared parameter type in the reading direction and the declared return type
// in the writing direction, with `void` as the only valueless return.
[[nodiscard]] bool MetadataIsConsistent(const CallableMetadata &Metadata,
                                        const TypeGeneration &Types) {
  for (const ValueKind Kind : Metadata.ParameterTypes()) {
    if (!Types.IsAvailableForRead(CanonicalValueType(Kind)))
      return false;
  }

  // One instance member additionally reads the object it operates on, so the
  // captured generation must describe that class in the reading direction
  // before the member is invoked at all.
  if (const ReceiverMetadata *Receiver = Metadata.Receiver()) {
    if (!Receiver->IsDeclared() ||
        !Types.IsAvailableForRead(TypeDescriptor::ForClass(Receiver->Class())))
      return false;
  }

  const ReturnMetadata &Return = Metadata.ReturnType();
  if (Return.Disposition() == ReturnDisposition::Value)
    return Return.Kind() &&
           Types.IsAvailableForWrite(CanonicalValueType(*Return.Kind()));

  // An ordered pack publishes one value per element, so consistency is the
  // writability of every element type its shape declares. A dynamic pack
  // declares none and is validated element by element at publication.
  if (Return.Disposition() == ReturnDisposition::Pack) {
    if (Return.Kind())
      return false;
    for (const ValueKind Kind : Return.PackKinds()) {
      if (!Types.IsAvailableForWrite(CanonicalValueType(Kind)))
        return false;
    }
    return true;
  }

  // One value of one registered class: the captured generation must describe
  // that class in the writing direction, exactly as it must describe a scalar.
  if (Return.Disposition() == ReturnDisposition::Instance) {
    if (Return.Kind())
      return false;
    const StableTypeKey *Class = Return.InstanceKey();
    return Class != nullptr &&
           Types.IsAvailableForWrite(TypeDescriptor::ForClass(*Class));
  }
  return Return.Kind() == nullptr;
}

} // namespace

ConversionSubject InvocationSubject(std::string_view GlobalName,
                                    const CallableMetadata *Metadata) {
  return SubjectForCallable(GlobalName,
                            Metadata != nullptr && Metadata->HasReceiver());
}

void RecordArgumentReadFailure(InvocationValidationResult &Validation,
                               const ArgumentReadResult &Read,
                               const ConversionSubject &Subject,
                               std::size_t Position,
                               std::string_view ExpectedTypeName) {
  const std::string Named = DescribeConversionSubject(Subject);
  const std::string Prefix =
      Named + " argument " + std::to_string(Position) + " ";
  switch (Read.Status) {
  case ArgumentReadStatus::TypeMismatch:
    Validation.RecordCallerFailure(Prefix + "expected " +
                                   std::string(ExpectedTypeName) +
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
        Named + ".");
    return;
  }
}

ValidatedInvocation ValidateInvocation(lua_State *State,
                                       std::string_view GlobalName,
                                       const CallableMetadata *Metadata,
                                       const TypeGeneration &Types,
                                       FaultInjector &Faults,
                                       int ArgumentBase) noexcept {
  ValidatedInvocation Result;
  try {
    if (Faults.Consume(StateFaultPoint::MissingMetadata) || !Metadata) {
      Result.Validation.RecordInternalFailure(
          "Internal error: callable metadata is unavailable for " +
          CallableContext(GlobalName) + ".");
      return Result;
    }

    // From here on the callable's own metadata is known, so every diagnostic
    // names it the way it describes itself: one instance member names its class
    // and member, every other callable keeps the foundation's wording.
    const ConversionSubject Subject = InvocationSubject(GlobalName, Metadata);
    const std::string Named = DescribeConversionSubject(Subject);

    if (!MetadataIsConsistent(*Metadata, Types)) {
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
    const int SuppliedCount =
        lua_gettop(State) - (ArgumentBase > 0 ? ArgumentBase - 1 : 0);
    const int ReceivedCount = SuppliedCount > 0 ? SuppliedCount : 0;
    if (ReceivedCount != static_cast<int>(Parameters.size())) {
      Result.Validation.RecordCallerFailure(
          Named + " expected " + std::to_string(Parameters.size()) +
          " arguments but received " + std::to_string(ReceivedCount) + ".");
      return Result;
    }

    Result.Arguments.reserve(Parameters.size());
    for (std::size_t Index = 0; Index < Parameters.size(); ++Index) {
      const bool InjectInspectionFailure =
          Faults.Consume(StateFaultPoint::ArgumentInspection);
      auto Read =
          ReadArgument(Types, State, static_cast<int>(Index) + ArgumentBase,
                       Parameters[Index], InjectInspectionFailure);
      if (!Read.IsSuccess()) {
        RecordArgumentReadFailure(Result.Validation, Read, Subject, Index + 1,
                                  Types.PublicNameOf(Parameters[Index]));
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

ValidatedInvocation ValidateInvocation(lua_State *State,
                                       std::string_view GlobalName,
                                       const CallableMetadata *Metadata,
                                       FaultInjector &Faults) noexcept {
  const std::shared_ptr<const TypeGeneration> Types =
      TypeGeneration::Foundation();
  ValidatedInvocation Result;
  if (!Types) {
    try {
      Result.Validation.RecordInternalFailure(
          "Internal error: the canonical type registry is unavailable for " +
          CallableContext(GlobalName) + ".");
    } catch (...) {
    }
    return Result;
  }
  return ValidateInvocation(State, GlobalName, Metadata, *Types, Faults);
}

} // namespace Luna::Detail
