// clang-format off
#include "state/invocation/parameters/parameter_binding.hpp"

#include <luna/binding/conversion.hpp>
#include <luna/binding/parameter_descriptor.hpp>

#include "state/invocation/conversion/argument_reader.hpp"
#include "state/invocation/conversion/return_writer.hpp"
#include "state/invocation/parameters/argument_frame.hpp"
#include "state/invocation/validation/validator.hpp"
#include "state/testing/fault_injector.hpp"
#include "state/testing/fault_point.hpp"
#include "state/type/conversion_outcome.hpp"
#include "state/type/type_record.hpp"

#include <lua.h>

#include <optional>
#include <string>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

// The subject one declared shape names. An instance member names its class and
// member; every other callable keeps the foundation's own wording. The receiver
// travelling in the metadata is exactly what tells the two apart.
[[nodiscard]] ConversionSubject Subject(std::string_view CallableName,
                                        const CallableMetadata *Metadata) {
  return InvocationSubject(CallableName, Metadata);
}

[[nodiscard]] std::string SubjectText(const ConversionSubject &Named) {
  return DescribeConversionSubject(Named);
}

[[nodiscard]] std::string ContextText(const ConversionSubject &Named) {
  return DescribeConversionSubjectContext(Named);
}

// The arity one declared shape accepts, worded so an equivalent shape always
// reports one identical sentence. A shape with one exact arity keeps the
// foundation's wording.
[[nodiscard]] std::string AcceptedArityText(const ParameterArity &Arity) {
  if (!Arity.Maximum)
    return "at least " + std::to_string(Arity.Minimum) + " arguments";
  if (Arity.Minimum == *Arity.Maximum)
    return std::to_string(Arity.Minimum) + " arguments";
  return "between " + std::to_string(Arity.Minimum) + " and " +
         std::to_string(*Arity.Maximum) + " arguments";
}

// Every fixed parameter must name a type the captured generation can read, and
// the declared shape itself must be one Luna described at registration.
[[nodiscard]] bool
ShapeIsConsistent(std::span<const ParameterDescriptor> Parameters,
                  const TypeGeneration &Types) {
  const ParameterShapeIssue Issue = ValidateParameterShape(Parameters);
  if (!Issue.IsValid())
    return false;
  for (const ParameterDescriptor &Parameter : Parameters) {
    if (Parameter.IsVariadic())
      continue;
    const ValueKind *Kind = Parameter.Kind();
    if (!Kind || !Types.IsAvailableForRead(CanonicalValueType(*Kind)))
      return false;
  }
  return true;
}

// One variadic call position. The accepted domain is one Luna-owned policy: a
// boolean, number, string, or nil converts through the captured registry and
// keeps every inherited limit; any other representation is refused whole,
// naming the one-based call position and the path of the value that failed.
[[nodiscard]] bool ReadVariadicElement(const TypeGeneration &Types,
                                       lua_State *State, int StackIndex,
                                       std::size_t Position,
                                       const ConversionSubject &Named,
                                       bool InjectInspectionFailure,
                                       InvocationValidationResult &Validation,
                                       OwnedValue &Element) {
  const int Received = lua_type(State, StackIndex);
  if (Received == LUA_TNIL) {
    if (InjectInspectionFailure) {
      Validation.RecordInternalFailure(
          "Internal error while inspecting " + SubjectText(Named) +
          " argument " + std::to_string(Position) + " for validation.");
      return false;
    }
    Element = OwnedValue::Nil();
    return true;
  }

  ValueKind Expected = ValueKind::String;
  switch (Received) {
  case LUA_TBOOLEAN:
    Expected = ValueKind::Boolean;
    break;
  case LUA_TNUMBER:
    Expected = ValueKind::Number;
    break;
  case LUA_TSTRING:
    Expected = ValueKind::String;
    break;
  default: {
    const char *Name = lua_typename(State, Received);
    Validation.RecordCallerFailure(
        SubjectText(Named) + " argument " + std::to_string(Position) +
        " expected a boolean, number, string, or nil variadic value but "
        "received " +
        std::string(Name ? Name : "unknown") + ".");
    return false;
  }
  }

  const ArgumentReadResult Read =
      ReadArgument(Types, State, StackIndex, Expected, InjectInspectionFailure);
  if (!Read.IsSuccess()) {
    RecordArgumentReadFailure(Validation, Read, Named, Position,
                              Types.PublicNameOf(Expected));
    return false;
  }

  const Value &Converted = *Read.ConvertedValue;
  Element = OwnedValue::FromValue(Converted);
  return true;
}

} // namespace

BoundInvocation BindDeclaredParameters(lua_State *State,
                                       std::string_view CallableName,
                                       const CallableMetadata &Metadata,
                                       const TypeGeneration &Types,
                                       FaultInjector &Faults,
                                       int ArgumentBase) noexcept {
  BoundInvocation Result;
  const ConversionSubject Named = Subject(CallableName, &Metadata);
  try {
    const std::span<const ParameterDescriptor> Parameters =
        Metadata.Parameters();

    if (Faults.Consume(StateFaultPoint::MissingMetadata) ||
        !ShapeIsConsistent(Parameters, Types)) {
      Result.Validation.RecordInternalFailure(
          "Internal error: callable metadata is inconsistent for " +
          ContextText(Named) + ".");
      return Result;
    }

    if (!State) {
      Result.Validation.RecordInternalFailure(
          "Internal error: argument stack is unavailable for " +
          ContextText(Named) + ".");
      return Result;
    }

    const ParameterArity Arity = ArityOf(Parameters);
    const int SuppliedCount =
        lua_gettop(State) - (ArgumentBase > 0 ? ArgumentBase - 1 : 0);
    const int ReceivedCount = SuppliedCount > 0 ? SuppliedCount : 0;
    const bool TooFew = ReceivedCount < static_cast<int>(Arity.Minimum);
    const bool TooMany =
        Arity.Maximum && ReceivedCount > static_cast<int>(*Arity.Maximum);
    if (TooFew || TooMany) {
      Result.Validation.RecordCallerFailure(
          SubjectText(Named) + " expected " + AcceptedArityText(Arity) +
          " but received " + std::to_string(ReceivedCount) + ".");
      return Result;
    }

    // Every supplied argument is validated before any default is materialized,
    // so a refused call materializes no default at all.
    Result.Arguments.Fixed.resize(Arity.FixedCount);
    for (std::size_t Index = 0; Index < Arity.FixedCount; ++Index) {
      if (Index >= static_cast<std::size_t>(ReceivedCount))
        break;

      const ParameterDescriptor &Parameter = Parameters[Index];
      const int StackIndex = static_cast<int>(Index) + ArgumentBase;
      const bool InjectInspectionFailure =
          Faults.Consume(StateFaultPoint::ArgumentInspection);

      // An explicit nil is the empty value exactly when the parameter's own
      // conversion accepts nil; otherwise it is an ordinary supplied value and
      // the parameter's converter refuses it.
      if (!InjectInspectionFailure && Parameter.AcceptsNil() &&
          lua_type(State, StackIndex) == LUA_TNIL)
        continue;

      const ValueKind *Kind = Parameter.Kind();
      if (!Kind) {
        Result.Validation.RecordInternalFailure(
            "Internal error: callable metadata is inconsistent for " +
            ContextText(Named) + ".");
        Result.Arguments = BoundArguments();
        return Result;
      }

      const ArgumentReadResult Read = ReadArgument(
          Types, State, StackIndex, *Kind, InjectInspectionFailure);
      if (!Read.IsSuccess()) {
        RecordArgumentReadFailure(Result.Validation, Read, Named, Index + 1,
                                  Types.PublicNameOf(*Kind));
        Result.Arguments = BoundArguments();
        return Result;
      }
      Result.Arguments.Fixed[Index] =
          ArgumentSlot::Supplied(std::move(*Read.ConvertedValue));
    }

    if (Arity.IsVariadic) {
      const std::size_t FirstPosition = Arity.FixedCount + 1;
      ValuePack Values;
      for (std::size_t Position = FirstPosition;
           Position <= static_cast<std::size_t>(ReceivedCount); ++Position) {
        const bool InjectInspectionFailure =
            Faults.Consume(StateFaultPoint::ArgumentInspection);
        OwnedValue Element;
        const int StackIndex = static_cast<int>(Position) + ArgumentBase - 1;
        if (!ReadVariadicElement(Types, State, StackIndex, Position, Named,
                                 InjectInspectionFailure, Result.Validation,
                                 Element)) {
          Result.Arguments = BoundArguments();
          return Result;
        }
        Values.Append(std::move(Element));
      }
      Result.Arguments.Variadic =
          ArgumentPack(std::move(Values), FirstPosition);
      Result.Arguments.HasVariadic = true;
    }

    // Only now, with every supplied argument accepted, is each omitted default
    // materialized - exactly once.
    for (std::size_t Index = static_cast<std::size_t>(ReceivedCount);
         Index < Arity.FixedCount; ++Index) {
      const ParameterDescriptor &Parameter = Parameters[Index];
      if (const Value *Default = Parameter.Default())
        Result.Arguments.Fixed[Index] = ArgumentSlot::Supplied(*Default);
    }
  } catch (...) {
    Result.Arguments = BoundArguments();
    try {
      Result.Validation.RecordInternalFailure(
          "Internal error while validating " + ContextText(Named) + ".");
    } catch (...) {
    }
  }
  return Result;
}

RichInvocationResult
InvokeDeclaredParameters(lua_State *State, std::string_view CallableName,
                         ErasedCallableDescriptor &Descriptor,
                         const TypeGeneration &Types, FaultInjector &Faults,
                         const InstanceReceiver *Receiver) {
  const int ArgumentBase = Receiver != nullptr ? 2 : 1;
  const ConversionSubject Named = Subject(CallableName, &Descriptor.Metadata());
  BoundInvocation Bound = BindDeclaredParameters(
      State, CallableName, Descriptor.Metadata(), Types, Faults, ArgumentBase);
  if (!Bound.Validation.IsSuccess()) {
    const ErrorDiagnostic *Diagnostic = Bound.Validation.Diagnostic();
    std::string Message = Diagnostic
                              ? Diagnostic->Message()
                              : "Internal invocation validation error for " +
                                    ContextText(Named) + ".";
    return {.ReturnCount = -1, .Diagnostic = std::move(Message)};
  }

  std::optional<InvocationOutcome> Outcome;
  if (Bound.Arguments.HasVariadic) {
    // The variadic frame ends with this scope, so a retained view becomes inert
    // instead of reaching released storage.
    ArgumentFrame Frame(std::move(Bound.Arguments.Variadic));
    const InvocationArguments Arguments(Bound.Arguments.Fixed, Frame.View(),
                                        &Frame.Arguments());
    Outcome = Receiver
                  ? Descriptor.InvokeDeclaredWithReceiver(*Receiver, Arguments)
                  : Descriptor.InvokeDeclared(Arguments);
  } else {
    const InvocationArguments Arguments(Bound.Arguments.Fixed);
    Outcome = Receiver
                  ? Descriptor.InvokeDeclaredWithReceiver(*Receiver, Arguments)
                  : Descriptor.InvokeDeclared(Arguments);
  }

  if (Outcome->Kind() == InvocationOutcomeKind::InternalFailure)
    return {.ReturnCount = -1,
            .Diagnostic = "Internal error for " + ContextText(Named) + ": " +
                          Outcome->FailureMessage()};

  const ReturnWriteResult Written = WriteInvocationReturn(
      State, Descriptor.Metadata().ReturnType(), *Outcome, Types, Faults);
  if (!Written.IsSuccess()) {
    const std::string Message = Written.Diagnostic
                                    ? Written.Diagnostic->Message()
                                    : "Return handling failed.";
    return {.ReturnCount = -1,
            .Diagnostic =
                "Internal error for " + ContextText(Named) + ": " + Message};
  }
  return {.ReturnCount = Written.ReturnCount};
}

} // namespace Luna::Detail
