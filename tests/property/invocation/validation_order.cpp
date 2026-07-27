// clang-format off
#include <luna/binding/callable_metadata.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include "state/invocation/conversion/argument_reader.hpp"
#include "state/invocation/testing/test_hooks.hpp"
#include "state/invocation/validation/validation_result.hpp"

#include <rapidcheck.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::InvocationPrimitiveTestHooks;
using Input = Luna::Detail::InvocationTestValue;
using ReadStatus = Luna::Detail::ArgumentReadStatus;
using ValidationState = Luna::Detail::InvocationValidationState;

constexpr std::string_view GlobalName = "ShortCircuit";
constexpr std::size_t ParameterCount = 4;

enum class ArgumentFault : std::uint8_t {
  Type,
  NonFinite,
  OutOfRange,
  Fractional
};

struct Scenario final {
  bool MissingMetadata = false;
  bool WrongCount = false;
  bool Inspection = false;
  std::uint8_t ActiveArguments = 0;
};
struct ExpectedDiagnostic final {
  Luna::ErrorCategory Category = Luna::ErrorCategory::Internal;
  std::string Message;

  bool operator==(const ExpectedDiagnostic &) const = default;
};

struct ValidationModel final {
  ValidationState State = ValidationState::Success;
  std::optional<ExpectedDiagnostic> Diagnostic;
  std::vector<std::string> Trace;
  std::size_t PendingMissingMetadataFaults = 0;
  std::size_t PendingInspectionFaults = 0;
};

[[nodiscard]] std::string CallableContext() {
  return "Callable '" + std::string(GlobalName) + "'";
}

[[nodiscard]] std::string ArgumentContext(std::size_t Position) {
  return CallableContext() + " argument " + std::to_string(Position) + " ";
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

void RecordFailure(ValidationModel &Model, ValidationState State,
                   Luna::ErrorCategory Category, std::string Message) {
  Model.State = State;
  Model.Diagnostic = ExpectedDiagnostic{Category, std::move(Message)};
}

[[nodiscard]] std::array<ArgumentFault, ParameterCount>
FaultPermutation(std::uint32_t Selector) {
  std::array<ArgumentFault, ParameterCount> Faults{
      ArgumentFault::Type, ArgumentFault::NonFinite, ArgumentFault::OutOfRange,
      ArgumentFault::Fractional};
  Selector %= 24;
  while (Selector-- != 0)
    std::next_permutation(Faults.begin(), Faults.end());
  return Faults;
}

[[nodiscard]] Input FaultInput(ArgumentFault Fault, std::uint32_t Selector) {
  switch (Fault) {
  case ArgumentFault::Type:
    switch (Selector % 3) {
    case 0:
      return std::monostate{};
    case 1:
      return false;
    default:
      return std::string("wrong");
    }
  case ArgumentFault::NonFinite:
    switch (Selector % 3) {
    case 0:
      return std::numeric_limits<double>::quiet_NaN();
    case 1:
      return std::numeric_limits<double>::infinity();
    default:
      return -std::numeric_limits<double>::infinity();
    }
  case ArgumentFault::OutOfRange: {
    const double Magnitude = 2147483648.0 + (Selector % 32);
    return Selector % 2 == 0 ? Magnitude : -Magnitude - 1.0;
  }
  case ArgumentFault::Fractional: {
    const double Magnitude = static_cast<double>(Selector % 64) + 0.5;
    return Selector % 2 == 0 ? Magnitude : -Magnitude;
  }
  }
  return 0;
}

[[nodiscard]] std::vector<Input>
MakeInputs(const Scenario &Case,
           const std::array<ArgumentFault, ParameterCount> &Faults,
           std::uint32_t ValueSelector, std::size_t WrongReceivedCount) {
  std::vector<Input> Inputs;
  Inputs.reserve(std::max(ParameterCount, WrongReceivedCount));
  for (std::size_t Index = 0; Index < ParameterCount; ++Index) {
    if ((Case.ActiveArguments & (1U << Index)) != 0)
      Inputs.push_back(FaultInput(Faults[Index], ValueSelector + Index));
    else
      Inputs.push_back(static_cast<int>(Index + 1));
  }

  const std::size_t ReceivedCount =
      Case.WrongCount ? WrongReceivedCount : ParameterCount;
  Inputs.resize(ReceivedCount, 17);
  return Inputs;
}
[[nodiscard]] std::string ReceivedType(const Input &Value) {
  if (std::holds_alternative<std::monostate>(Value))
    return "nil";
  if (std::holds_alternative<bool>(Value))
    return "boolean";
  if (std::holds_alternative<std::string>(Value))
    return "string";
  return "number";
}

void AddArgumentCheck(ValidationModel &Model, std::size_t Position,
                      std::string_view Check) {
  Model.Trace.push_back("argument " + std::to_string(Position) + " " +
                        std::string(Check));
}

[[nodiscard]] ValidationModel
ReferenceValidate(const Scenario &Case, const std::vector<Input> &Inputs) {
  ValidationModel Model;
  Model.PendingMissingMetadataFaults = Case.MissingMetadata ? 1 : 0;
  Model.PendingInspectionFaults = Case.Inspection ? 1 : 0;

  Model.Trace.emplace_back("metadata availability");
  if (Model.PendingMissingMetadataFaults != 0) {
    --Model.PendingMissingMetadataFaults;
    RecordFailure(Model, ValidationState::InternalError,
                  Luna::ErrorCategory::Internal,
                  "Internal error: callable metadata is unavailable for " +
                      CallableContext() + ".");
    return Model;
  }

  Model.Trace.emplace_back("metadata consistency");
  Model.Trace.emplace_back("argument count");
  if (Inputs.size() != ParameterCount) {
    RecordFailure(
        Model, ValidationState::CallerError, Luna::ErrorCategory::Runtime,
        CallableContext() + " expected " + std::to_string(ParameterCount) +
            " arguments but received " + std::to_string(Inputs.size()) + ".");
    return Model;
  }

  for (std::size_t Index = 0; Index < ParameterCount; ++Index) {
    const std::size_t Position = Index + 1;
    AddArgumentCheck(Model, Position, "inspection");
    if (Model.PendingInspectionFaults != 0) {
      --Model.PendingInspectionFaults;
      RecordFailure(Model, ValidationState::InternalError,
                    Luna::ErrorCategory::Internal,
                    "Internal error while inspecting " +
                        ArgumentContext(Position) + "for validation.");
      return Model;
    }

    AddArgumentCheck(Model, Position, "type");
    const Input &Value = Inputs[Index];
    if (!std::holds_alternative<int>(Value) &&
        !std::holds_alternative<double>(Value)) {
      RecordFailure(Model, ValidationState::CallerError,
                    Luna::ErrorCategory::Runtime,
                    ArgumentContext(Position) +
                        "expected signed 32-bit integer but received " +
                        ReceivedType(Value) + ".");
      return Model;
    }

    const double Number = std::holds_alternative<int>(Value)
                              ? static_cast<double>(std::get<int>(Value))
                              : std::get<double>(Value);
    AddArgumentCheck(Model, Position, "finiteness");
    if (!std::isfinite(Number)) {
      RecordFailure(
          Model, ValidationState::CallerError, Luna::ErrorCategory::Runtime,
          ArgumentContext(Position) +
              "expected a finite signed 32-bit integer but received " +
              FormatNumber(Number) + ".");
      return Model;
    }

    AddArgumentCheck(Model, Position, "range");
    constexpr double Minimum = -2147483648.0;
    constexpr double Maximum = 2147483647.0;
    if (Number < Minimum || Number > Maximum) {
      RecordFailure(
          Model, ValidationState::CallerError, Luna::ErrorCategory::Runtime,
          ArgumentContext(Position) +
              "expected signed 32-bit range [-2147483648, 2147483647] "
              "but received " +
              FormatNumber(Number) + ".");
      return Model;
    }

    AddArgumentCheck(Model, Position, "integrality");
    if (std::trunc(Number) != Number) {
      RecordFailure(Model, ValidationState::CallerError,
                    Luna::ErrorCategory::Runtime,
                    ArgumentContext(Position) +
                        "expected an integral value but received " +
                        FormatNumber(Number) + ".");
      return Model;
    }
  }

  return Model;
}
void AppendObservedReadTrace(std::vector<std::string> &Trace,
                             std::size_t Position, ReadStatus Status) {
  Trace.push_back("argument " + std::to_string(Position) + " inspection");
  if (Status == ReadStatus::InternalFailure)
    return;

  Trace.push_back("argument " + std::to_string(Position) + " type");
  if (Status == ReadStatus::TypeMismatch || Status == ReadStatus::StringTooLong)
    return;

  Trace.push_back("argument " + std::to_string(Position) + " finiteness");
  if (Status == ReadStatus::IntegerNonFinite)
    return;

  Trace.push_back("argument " + std::to_string(Position) + " range");
  if (Status == ReadStatus::IntegerOutOfRange)
    return;

  Trace.push_back("argument " + std::to_string(Position) + " integrality");
}

[[nodiscard]] std::vector<std::string>
ObserveTrace(const Scenario &Case, const std::vector<Input> &Inputs) {
  std::vector<std::string> Trace{"metadata availability"};
  if (Case.MissingMetadata)
    return Trace;

  Trace.emplace_back("metadata consistency");
  Trace.emplace_back("argument count");
  if (Inputs.size() != ParameterCount)
    return Trace;

  for (std::size_t Index = 0; Index < ParameterCount; ++Index) {
    const bool InjectInspection = Case.Inspection && Index == 0;
    const auto Read =
        Hooks::Read(Inputs[Index], Luna::ValueKind::Integer, InjectInspection);
    AppendObservedReadTrace(Trace, Index + 1, Read.Status);
    if (!Read.IsSuccess())
      return Trace;
  }
  return Trace;
}

[[nodiscard]] std::size_t WrongCount(std::uint32_t Selector) {
  std::size_t Count = Selector % 7;
  if (Count == ParameterCount)
    ++Count;
  return Count;
}

} // namespace

int RunValidationShortCircuitingProperties() {
  // clang-format off
  const bool Passed = rc::check(
      // clang-format on
      "Validation agrees with the short-circuit reference model",
      [](std::uint32_t GeneratedPermutation, std::uint32_t GeneratedValues,
         std::uint32_t GeneratedCount, std::uint8_t GeneratedCombination,
         std::uint8_t GeneratedArgumentMask) {
        const auto Faults = FaultPermutation(GeneratedPermutation);
        const std::size_t ReceivedCount = WrongCount(GeneratedCount);
        const std::uint8_t GeneratedMask = GeneratedArgumentMask & 0x0fU;
        const Scenario Generated{
            .MissingMetadata = (GeneratedCombination & 0x01U) != 0,
            .WrongCount = (GeneratedCombination & 0x02U) != 0,
            .Inspection = (GeneratedCombination & 0x04U) != 0,
            .ActiveArguments = GeneratedMask};
        const std::uint8_t LaterPositionMask =
            static_cast<std::uint8_t>(1U << (GeneratedValues % ParameterCount));

        const std::array<Scenario, 7> Scenarios{
            Scenario{true, true, true, 0x0fU},
            Scenario{false, true, true, 0x0fU},
            Scenario{false, false, true, 0x0fU},
            Scenario{false, false, false, 0x0fU},
            Scenario{false, false, false, LaterPositionMask},
            Generated,
            Scenario{false, false, false, 0x00U}};

        const Luna::CallableMetadata Metadata(
            std::vector<Luna::ValueKind>(ParameterCount,
                                         Luna::ValueKind::Integer),
            Luna::ReturnMetadata::ForVoid());

        for (const Scenario &Case : Scenarios) {
          const auto Inputs =
              MakeInputs(Case, Faults, GeneratedValues, ReceivedCount);
          const ValidationModel Expected = ReferenceValidate(Case, Inputs);
          const auto Actual = Hooks::Validate(
              Inputs, std::string(GlobalName), &Metadata,
              Case.MissingMetadata ? 1 : 0, Case.Inspection ? 1 : 0);

          RC_ASSERT(Actual.Invocation.Validation.State() == Expected.State);
          const Luna::ErrorDiagnostic *ActualDiagnostic =
              Actual.Invocation.Validation.Diagnostic();
          RC_ASSERT((ActualDiagnostic != nullptr) ==
                    Expected.Diagnostic.has_value());
          if (Expected.Diagnostic) {
            RC_ASSERT(ActualDiagnostic->Category() ==
                      Expected.Diagnostic->Category);
            RC_ASSERT(ActualDiagnostic->Message() ==
                      Expected.Diagnostic->Message);
          }

          RC_ASSERT(ObserveTrace(Case, Inputs) == Expected.Trace);
          RC_ASSERT(Actual.PendingMissingMetadataFaults ==
                    Expected.PendingMissingMetadataFaults);
          RC_ASSERT(Actual.PendingArgumentInspectionFaults ==
                    Expected.PendingInspectionFaults);
        }
      });

  return Passed ? 0 : 1;
}
