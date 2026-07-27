#pragma once

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

class ScriptValue final {
public:
  enum class ScriptKind { Nil, Boolean, Number, Text, Array, Table };

  [[nodiscard]] static ScriptValue Nil();
  [[nodiscard]] static ScriptValue Boolean(bool Flag);
  [[nodiscard]] static ScriptValue Number(double Value);
  [[nodiscard]] static ScriptValue Text(std::string Value);

  [[nodiscard]] static ScriptValue Array(std::vector<ScriptValue> Elements);

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

  int StackDepthDelta = 0;
};

struct StructuralWriteObservation final {
  bool Accepted = false;
  int PublishedCount = 0;
  StructuredDiagnostic Diagnostic;

  int StackDepthDelta = 0;

  bool RoundTripMatches = false;
};

class StructuralConversionTestHooks final {
public:
  [[nodiscard]] static std::shared_ptr<const TypeGeneration>
  GenerationFor(const TypeDescriptor &Type, std::vector<TypeRecord> Extra = {});

  [[nodiscard]] static StructuralReadObservation
  Read(const ScriptValue &Input, const TypeDescriptor &Type,
       std::vector<TypeRecord> Extra = {});

  [[nodiscard]] static StructuralReadObservation
  ReadPack(const std::vector<ScriptValue> &Inputs, const TypeDescriptor &Type,
           std::size_t FirstArgumentPosition,
           std::vector<TypeRecord> Extra = {});

  [[nodiscard]] static StructuralWriteObservation
  Write(const StructuredValue &Source, const TypeDescriptor &Type,
        std::vector<TypeRecord> Extra = {});

  [[nodiscard]] static StructuralWriteObservation
  PublishReturn(const StructuredValue &Source, const TypeDescriptor &Type,
                std::vector<TypeRecord> Extra = {});
};

} // namespace Luna::Detail
