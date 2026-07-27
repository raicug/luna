// clang-format off
#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/callable_descriptor.hpp>

#include "state/invocation/conversion/argument_reader.hpp"
#include "state/invocation/conversion/return_writer.hpp"
#include "state/invocation/testing/test_hooks.hpp"
#include "state/invocation/validation/validation_result.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::InvocationPrimitiveTestHooks;
using ReadStatus = Luna::Detail::ArgumentReadStatus;
using ValidationState = Luna::Detail::InvocationValidationState;
using WriteStatus = Luna::Detail::ReturnWriteStatus;

[[nodiscard]] bool Contains(const Luna::ErrorDiagnostic *Diagnostic,
                            std::string_view Text) {
  return Diagnostic && Diagnostic->Message().find(Text) != std::string::npos;
}

[[nodiscard]] Luna::CallableMetadata
Metadata(std::vector<Luna::ValueKind> Parameters,
         Luna::ReturnMetadata Return = Luna::ReturnMetadata::ForVoid()) {
  return Luna::CallableMetadata(std::move(Parameters), std::move(Return));
}

[[nodiscard]] bool TestReaders() {
  const auto False = Hooks::Read(false, Luna::ValueKind::Boolean);
  const auto True = Hooks::Read(true, Luna::ValueKind::Boolean);
  const auto WrongBoolean = Hooks::Read(1.0, Luna::ValueKind::Boolean);
  const auto WrongInteger =
      Hooks::Read(std::string("1"), Luna::ValueKind::Integer);
  if (!False.IsSuccess() || !True.IsSuccess() ||
      std::get<bool>(*False.ConvertedValue) ||
      !std::get<bool>(*True.ConvertedValue) ||
      WrongBoolean.Status != ReadStatus::TypeMismatch ||
      WrongBoolean.ReceivedType != "number" ||
      WrongInteger.Status != ReadStatus::TypeMismatch ||
      WrongInteger.ReceivedType != "string")
    return false;

  for (const int Value : {std::numeric_limits<int>::min(), -1, 0, 1,
                          std::numeric_limits<int>::max()}) {
    const auto Read = Hooks::Read(Value, Luna::ValueKind::Integer);
    if (!Read.IsSuccess() || std::get<int>(*Read.ConvertedValue) != Value)
      return false;
  }

  const auto NonFinite = Hooks::Read(std::numeric_limits<double>::infinity(),
                                     Luna::ValueKind::Integer);
  const auto OutOfRange = Hooks::Read(2147483647.5, Luna::ValueKind::Integer);
  const auto Fractional = Hooks::Read(12.25, Luna::ValueKind::Integer);
  return NonFinite.Status == ReadStatus::IntegerNonFinite &&
         OutOfRange.Status == ReadStatus::IntegerOutOfRange &&
         Fractional.Status == ReadStatus::IntegerFractional;
}

[[nodiscard]] bool TestDoubleAndStringReaders() {
  const auto PositiveZero = Hooks::Read(0.0, Luna::ValueKind::Number);
  const auto NegativeZero = Hooks::Read(-0.0, Luna::ValueKind::Number);
  const auto PositiveInfinity = Hooks::Read(
      std::numeric_limits<double>::infinity(), Luna::ValueKind::Number);
  const auto NegativeInfinity = Hooks::Read(
      -std::numeric_limits<double>::infinity(), Luna::ValueKind::Number);
  const auto FirstNaN = Hooks::Read(std::nan("1"), Luna::ValueKind::Number);
  const auto SecondNaN = Hooks::Read(-std::nan("2"), Luna::ValueKind::Number);
  if (!PositiveZero.IsSuccess() ||
      std::signbit(std::get<double>(*PositiveZero.ConvertedValue)) ||
      !NegativeZero.IsSuccess() ||
      !std::signbit(std::get<double>(*NegativeZero.ConvertedValue)) ||
      !PositiveInfinity.IsSuccess() ||
      std::get<double>(*PositiveInfinity.ConvertedValue) !=
          std::numeric_limits<double>::infinity() ||
      !NegativeInfinity.IsSuccess() ||
      std::get<double>(*NegativeInfinity.ConvertedValue) !=
          -std::numeric_limits<double>::infinity() ||
      !FirstNaN.IsSuccess() ||
      !std::isnan(std::get<double>(*FirstNaN.ConvertedValue)) ||
      !SecondNaN.IsSuccess() ||
      !std::isnan(std::get<double>(*SecondNaN.ConvertedValue)))
    return false;

  const std::string Embedded("a\0b\0c", 5);
  const auto EmbeddedRead = Hooks::Read(Embedded, Luna::ValueKind::String);
  if (!EmbeddedRead.IsSuccess() || EmbeddedRead.ReceivedByteCount != 5 ||
      std::get<std::string>(*EmbeddedRead.ConvertedValue) != Embedded)
    return false;

  const std::string Maximum(Luna::Detail::MaximumInvocationStringBytes, 'm');
  const std::string Oversized(Luna::Detail::MaximumInvocationStringBytes + 1,
                              'x');
  const auto MaximumRead = Hooks::Read(Maximum, Luna::ValueKind::String);
  const auto OversizedRead = Hooks::Read(Oversized, Luna::ValueKind::String);
  const auto InspectionFailure =
      Hooks::Read(Embedded, Luna::ValueKind::String, true);
  return MaximumRead.IsSuccess() &&
         std::get<std::string>(*MaximumRead.ConvertedValue).size() ==
             Luna::Detail::MaximumInvocationStringBytes &&
         OversizedRead.Status == ReadStatus::StringTooLong &&
         !OversizedRead.ConvertedValue &&
         OversizedRead.ReceivedByteCount ==
             Luna::Detail::MaximumInvocationStringBytes + 1 &&
         InspectionFailure.Status == ReadStatus::InternalFailure;
}

[[nodiscard]] bool TestWriteOnceValidationResult() {
  Luna::Detail::InvocationValidationResult Result;
  if (!Result.IsSuccess() || Result.Diagnostic())
    return false;
  if (!Result.RecordCallerFailure("first caller failure") ||
      Result.RecordInternalFailure("second internal failure") ||
      Result.State() != ValidationState::CallerError ||
      !Contains(Result.Diagnostic(), "first caller failure") ||
      Contains(Result.Diagnostic(), "second") ||
      Result.Diagnostic()->Category() != Luna::ErrorCategory::Runtime)
    return false;

  Luna::Detail::InvocationValidationResult Internal;
  if (!Internal.RecordInternalFailure("metadata unavailable") ||
      Internal.RecordCallerFailure("later caller failure") ||
      Internal.State() != ValidationState::InternalError ||
      !Internal.Diagnostic() ||
      Internal.Diagnostic()->Category() != Luna::ErrorCategory::Internal ||
      !Contains(Internal.Diagnostic(), "metadata unavailable"))
    return false;

  Luna::Detail::InvocationValidationResult Fallback;
  return Fallback.RecordCallerFailure({}) && Fallback.Diagnostic() &&
         !Fallback.Diagnostic()->Message().empty();
}

[[nodiscard]] bool TestValidationOrderingAndFaults() {
  auto TwoIntegers =
      Metadata({Luna::ValueKind::Integer, Luna::ValueKind::Integer});

  const auto CountFirst = Hooks::Validate({1}, "Counted", &TwoIntegers, 0, 1);
  if (CountFirst.Invocation.Validation.State() !=
          ValidationState::CallerError ||
      !Contains(CountFirst.Invocation.Validation.Diagnostic(), "expected 2") ||
      !Contains(CountFirst.Invocation.Validation.Diagnostic(), "received 1") ||
      CountFirst.PendingArgumentInspectionFaults != 1)
    return false;

  const auto FirstMismatch = Hooks::Validate(
      {std::string("bad"), std::monostate{}}, "Ordered", &TwoIntegers);
  if (FirstMismatch.Invocation.Validation.State() !=
          ValidationState::CallerError ||
      !Contains(FirstMismatch.Invocation.Validation.Diagnostic(),
                "argument 1") ||
      Contains(FirstMismatch.Invocation.Validation.Diagnostic(), "argument 2"))
    return false;

  const auto Inspection =
      Hooks::Validate({1, 2}, "Inspected", &TwoIntegers, 0, 1);
  if (Inspection.Invocation.Validation.State() !=
          ValidationState::InternalError ||
      !Contains(Inspection.Invocation.Validation.Diagnostic(), "Inspected") ||
      Inspection.PendingArgumentInspectionFaults != 0)
    return false;

  const auto Missing = Hooks::Validate({1, 2}, "Missing", &TwoIntegers, 1, 1);
  if (Missing.Invocation.Validation.State() != ValidationState::InternalError ||
      !Contains(Missing.Invocation.Validation.Diagnostic(), "metadata") ||
      Missing.PendingMissingMetadataFaults != 0 ||
      Missing.PendingArgumentInspectionFaults != 1)
    return false;

  const auto Absent = Hooks::Validate({1, 2}, "Absent", nullptr);
  if (Absent.Invocation.Validation.State() != ValidationState::InternalError ||
      !Contains(Absent.Invocation.Validation.Diagnostic(), "Absent") ||
      !Absent.Invocation.Arguments.empty())
    return false;

  auto InvalidReturn = Metadata(
      {}, Luna::ReturnMetadata::ForValue(static_cast<Luna::ValueKind>(255)));
  const auto Inconsistent = Hooks::Validate({}, "Inconsistent", &InvalidReturn);
  if (Inconsistent.Invocation.Validation.State() !=
          ValidationState::InternalError ||
      !Contains(Inconsistent.Invocation.Validation.Diagnostic(),
                "inconsistent"))
    return false;

  const auto Valid = Hooks::Validate({-9, 27}, "Valid", &TwoIntegers);
  return Valid.Invocation.Validation.IsSuccess() &&
         Valid.Invocation.Arguments.size() == 2 &&
         std::get<int>(Valid.Invocation.Arguments[0]) == -9 &&
         std::get<int>(Valid.Invocation.Arguments[1]) == 27;
}

[[nodiscard]] bool TestIntegerValidationPrecedenceAndStringDiagnostic() {
  auto Integer = Metadata({Luna::ValueKind::Integer});
  const auto NonFinite = Hooks::Validate(
      {std::numeric_limits<double>::quiet_NaN()}, "Integer", &Integer);
  const auto Range = Hooks::Validate({2147483647.5}, "Integer", &Integer);
  const auto Fraction = Hooks::Validate({3.5}, "Integer", &Integer);
  const std::string Oversized(Luna::Detail::MaximumInvocationStringBytes + 1,
                              's');
  auto String = Metadata({Luna::ValueKind::String});
  const auto TooLong = Hooks::Validate({Oversized}, "Stringy", &String);

  return Contains(NonFinite.Invocation.Validation.Diagnostic(), "NaN") &&
         Contains(Range.Invocation.Validation.Diagnostic(),
                  "[-2147483648, 2147483647]") &&
         Contains(Fraction.Invocation.Validation.Diagnostic(), "integral") &&
         Contains(TooLong.Invocation.Validation.Diagnostic(), "argument 1") &&
         Contains(TooLong.Invocation.Validation.Diagnostic(), "1048577") &&
         Contains(TooLong.Invocation.Validation.Diagnostic(), "1048576");
}

[[nodiscard]] bool TestReturnWriters() {
  const auto Boolean =
      Hooks::Write(Luna::ReturnMetadata::ForValue(Luna::ValueKind::Boolean),
                   Luna::InvocationOutcome::WithValue(true));
  const auto Integer =
      Hooks::Write(Luna::ReturnMetadata::ForValue(Luna::ValueKind::Integer),
                   Luna::InvocationOutcome::WithValue(-31));
  const auto NegativeZero =
      Hooks::Write(Luna::ReturnMetadata::ForValue(Luna::ValueKind::Number),
                   Luna::InvocationOutcome::WithValue(-0.0));
  const auto Infinity =
      Hooks::Write(Luna::ReturnMetadata::ForValue(Luna::ValueKind::Number),
                   Luna::InvocationOutcome::WithValue(
                       std::numeric_limits<double>::infinity()));
  const auto NaN =
      Hooks::Write(Luna::ReturnMetadata::ForValue(Luna::ValueKind::Number),
                   Luna::InvocationOutcome::WithValue(std::nan("3")));
  const std::string Embedded("r\0s", 3);
  const auto String =
      Hooks::Write(Luna::ReturnMetadata::ForValue(Luna::ValueKind::String),
                   Luna::InvocationOutcome::WithValue(Embedded));
  const std::string Maximum(Luna::Detail::MaximumInvocationStringBytes, 'q');
  const auto MaximumString =
      Hooks::Write(Luna::ReturnMetadata::ForValue(Luna::ValueKind::String),
                   Luna::InvocationOutcome::WithValue(Maximum));

  if (Boolean.Result.Status != WriteStatus::ValueWritten ||
      Boolean.Result.ReturnCount != 1 || Boolean.StackDepth != 1 ||
      !Boolean.WrittenValue || !std::get<bool>(*Boolean.WrittenValue) ||
      !Integer.WrittenValue || std::get<int>(*Integer.WrittenValue) != -31 ||
      !NegativeZero.WrittenValue ||
      !std::signbit(std::get<double>(*NegativeZero.WrittenValue)) ||
      !Infinity.WrittenValue ||
      std::get<double>(*Infinity.WrittenValue) !=
          std::numeric_limits<double>::infinity() ||
      !NaN.WrittenValue || !std::isnan(std::get<double>(*NaN.WrittenValue)) ||
      !String.WrittenValue ||
      std::get<std::string>(*String.WrittenValue) != Embedded ||
      !MaximumString.WrittenValue ||
      std::get<std::string>(*MaximumString.WrittenValue).size() !=
          Luna::Detail::MaximumInvocationStringBytes)
    return false;

  const auto Void = Hooks::Write(Luna::ReturnMetadata::ForVoid(),
                                 Luna::InvocationOutcome::Void());
  const auto Suppressed = Hooks::Write(
      Luna::ReturnMetadata::Suppressed(),
      Luna::InvocationOutcome::InternalFailure("defensively suppressed"));
  return Void.Result.Status == WriteStatus::VoidCompleted &&
         Void.Result.ReturnCount == 0 && Void.StackDepth == 0 &&
         Suppressed.Result.Status == WriteStatus::Suppressed &&
         Suppressed.Result.ReturnCount == 0 && Suppressed.StackDepth == 0;
}

[[nodiscard]] bool TestReturnWriterFaults() {
  const auto WriterFault =
      Hooks::Write(Luna::ReturnMetadata::ForValue(Luna::ValueKind::Integer),
                   Luna::InvocationOutcome::WithValue(7), 1, 0, 0, 3);
  const auto CapacityFault =
      Hooks::Write(Luna::ReturnMetadata::ForValue(Luna::ValueKind::Integer),
                   Luna::InvocationOutcome::WithValue(8), 0, 0, 1, 2);
  const auto VoidFault =
      Hooks::Write(Luna::ReturnMetadata::ForVoid(),
                   Luna::InvocationOutcome::Void(), 0, 1, 0, 4);
  const auto Mismatch =
      Hooks::Write(Luna::ReturnMetadata::ForValue(Luna::ValueKind::Integer),
                   Luna::InvocationOutcome::WithValue(std::string("wrong")));
  const auto FailedOutcome =
      Hooks::Write(Luna::ReturnMetadata::ForValue(Luna::ValueKind::Integer),
                   Luna::InvocationOutcome::InternalFailure("callable failed"));
  const std::string Oversized(Luna::Detail::MaximumInvocationStringBytes + 1,
                              'z');
  const auto OversizedReturn =
      Hooks::Write(Luna::ReturnMetadata::ForValue(Luna::ValueKind::String),
                   Luna::InvocationOutcome::WithValue(Oversized));

  const auto FailedAtDepth = [](const auto &Observation, int Depth) {
    return Observation.Result.Status == WriteStatus::InternalFailure &&
           Observation.Result.ReturnCount == 0 &&
           Observation.StackDepth == Depth && Observation.Result.Diagnostic &&
           !Observation.WrittenValue;
  };

  return FailedAtDepth(WriterFault, 3) &&
         Contains(&*WriterFault.Result.Diagnostic, "return-writer") &&
         FailedAtDepth(CapacityFault, 2) &&
         Contains(&*CapacityFault.Result.Diagnostic, "reserve stack") &&
         FailedAtDepth(VoidFault, 4) && FailedAtDepth(Mismatch, 0) &&
         FailedAtDepth(FailedOutcome, 0) && FailedAtDepth(OversizedReturn, 0);
}

} // namespace

int RunInvocationPrimitiveTests() {
  if (!TestReaders())
    return 1;
  if (!TestDoubleAndStringReaders())
    return 2;
  if (!TestWriteOnceValidationResult())
    return 3;
  if (!TestValidationOrderingAndFaults())
    return 4;
  if (!TestIntegerValidationPrecedenceAndStringDiagnostic())
    return 5;
  if (!TestReturnWriters())
    return 6;
  if (!TestReturnWriterFaults())
    return 7;
  return 0;
}
