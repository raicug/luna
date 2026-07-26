#pragma once

// Test access to the structural converters. A correctness test cannot name a
// virtual-machine type, so it describes the Luau value it wants with a
// `ScriptValue` tree and receives the staged result, the first deterministic
// diagnostic, and the exact stack depth change back.
//
// These hooks exist so nested paths, aggregate shape validation, and the
// no-partial-publication guarantee can be observed directly rather than only
// through a registered callable, which cannot carry a structural signature
// until the signature milestones land.

// clang-format off
#include <luna/type/type_descriptor.hpp>

#include "state/type/structural_types.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>
// clang-format on

namespace Luna::Detail {

// A Luau-free description of one value to place on a scratch stack.
class ScriptValue final {
public:
  enum class ScriptKind { Nil, Boolean, Number, Text, Array, Table };

  [[nodiscard]] static ScriptValue Nil();
  [[nodiscard]] static ScriptValue Boolean(bool Flag);
  [[nodiscard]] static ScriptValue Number(double Value);
  [[nodiscard]] static ScriptValue Text(std::string Value);

  // A table whose keys are exactly 1..N in order.
  [[nodiscard]] static ScriptValue Array(std::vector<ScriptValue> Elements);

  // A table built from alternating key and value descriptions.
  [[nodiscard]] static ScriptValue Table(std::vector<ScriptValue> Entries);

  [[nodiscard]] ScriptKind Kind() const noexcept { return KindValue; }
  [[nodiscard]] bool BooleanValue() const noexcept { return FlagValue; }
  [[nodiscard]] double NumberValue() const noexcept { return NumberStorage; }
  [[nodiscard]] const std::string &TextValue() const noexcept {
    return TextStorage;
  }
  [[nodiscard]] const std::vector<ScriptValue> &Items() const noexcept {
    return ItemValues;
  }

private:
  ScriptKind KindValue = ScriptKind::Nil;
  bool FlagValue = false;
  double NumberStorage = 0.0;
  std::string TextStorage;
  std::vector<ScriptValue> ItemValues;
};

struct StructuralReadObservation final {
  bool Accepted = false;
  StructuredValue ConvertedValue;
  StructuredDiagnostic Diagnostic;

  // Reading never leaves a value behind, so this is always zero.
  int StackDepthDelta = 0;
};

struct StructuralWriteObservation final {
  bool Accepted = false;
  int PublishedCount = 0;
  StructuredDiagnostic Diagnostic;

  // Published values on success, zero on failure: a refused conversion exposes
  // neither a partial table nor a partial pack.
  int StackDepthDelta = 0;

  // The published values read back through the same canonical type equal the
  // staged value.
  bool RoundTripMatches = false;
};

class StructuralConversionTestHooks final {
public:
  // One generation describing the foundation, the planned built-in scalars,
  // every record `Extra` supplies, and every type `Type` needs.
  [[nodiscard]] static std::shared_ptr<const TypeGeneration>
  GenerationFor(const TypeDescriptor &Type, std::vector<TypeRecord> Extra = {});

  [[nodiscard]] static StructuralReadObservation
  Read(const ScriptValue &Input, const TypeDescriptor &Type,
       std::vector<TypeRecord> Extra = {});

  // Reads one argument pack from consecutive call positions.
  [[nodiscard]] static StructuralReadObservation
  ReadPack(const std::vector<ScriptValue> &Inputs, const TypeDescriptor &Type,
           std::size_t FirstArgumentPosition,
           std::vector<TypeRecord> Extra = {});

  [[nodiscard]] static StructuralWriteObservation
  Write(const StructuredValue &Source, const TypeDescriptor &Type,
        std::vector<TypeRecord> Extra = {});

  // Publishes one return shape, so a root pair, tuple, or return pack produces
  // ordered multiple values.
  [[nodiscard]] static StructuralWriteObservation
  PublishReturn(const StructuredValue &Source, const TypeDescriptor &Type,
                std::vector<TypeRecord> Extra = {});
};

} // namespace Luna::Detail
