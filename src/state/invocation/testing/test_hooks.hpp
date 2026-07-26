#pragma once

// clang-format off
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/value.hpp>

#include "state/invocation/conversion/argument_reader.hpp"
#include "state/invocation/conversion/return_writer.hpp"
#include "state/invocation/validation/validator.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>
// clang-format on

namespace Luna::Detail {

using InvocationTestValue =
    std::variant<std::monostate, bool, int, double, std::string>;

struct ReturnWriteObservation final {
  ReturnWriteResult Result;
  std::optional<Value> WrittenValue;

  // The ordered values one published pack left in the result positions.
  std::vector<Value> WrittenValues;
  int StackDepth = 0;
};

struct ValidationObservation final {
  ValidatedInvocation Invocation;
  std::size_t PendingMissingMetadataFaults = 0;
  std::size_t PendingArgumentInspectionFaults = 0;
};

class InvocationPrimitiveTestHooks final {
public:
  [[nodiscard]] static ArgumentReadResult
  Read(const InvocationTestValue &Input, ValueKind ExpectedKind,
       bool InjectInspectionFailure = false);

  [[nodiscard]] static ValidationObservation
  Validate(const std::vector<InvocationTestValue> &Inputs,
           std::string GlobalName, const CallableMetadata *Metadata,
           std::size_t MissingMetadataFaults = 0,
           std::size_t ArgumentInspectionFaults = 0);

  [[nodiscard]] static ReturnWriteObservation
  Write(const ReturnMetadata &Metadata, const InvocationOutcome &Outcome,
        std::size_t ReturnWriteFaults = 0,
        std::size_t VoidFinalizationFaults = 0,
        std::size_t ReturnStackCapacityFaults = 0,
        std::size_t InitialStackDepth = 0);
};

} // namespace Luna::Detail
