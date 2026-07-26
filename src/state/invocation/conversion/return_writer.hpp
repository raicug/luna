#pragma once

// Writing one invocation return through the canonical type registry. The writer
// validates the outcome against the canonical type of the declared return kind
// in the type generation the invocation captured, enforces that type's explicit
// size policy, reserves stack capacity, and only then lets the type's
// committing writer publish the value.
//
// The three return shapes share that one order. `void` publishes zero values, a
// scalar publishes one, and an ordered pack - a returned `std::pair`,
// `std::tuple`, or `Luna::ReturnPack` - publishes one value per element. A pack
// arrives as complete unpublished native storage: every element is checked
// against its canonical type and size policy and the whole publication is
// reserved before the first value reaches a result position, and any failure
// restores the callback checkpoint so the call exposes zero return values and
// one deterministic diagnostic naming the one-based return position.

// clang-format off
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include "state/type/type_generation.hpp"

#include <optional>
#include <string>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class FaultInjector;

enum class ReturnWriteStatus {
  ValueWritten,
  VoidCompleted,
  Suppressed,
  // Every element of one ordered return pack was staged, validated, and then
  // published together.
  PackPublished,
  InternalFailure
};

struct ReturnWriteResult final {
  ReturnWriteStatus Status = ReturnWriteStatus::InternalFailure;
  int ReturnCount = 0;
  std::optional<ErrorDiagnostic> Diagnostic;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Status != ReturnWriteStatus::InternalFailure;
  }
};

[[nodiscard]] ReturnWriteResult
WriteInvocationReturn(lua_State *State, const ReturnMetadata &Metadata,
                      const InvocationOutcome &Outcome,
                      const TypeGeneration &Types,
                      FaultInjector &Faults) noexcept;

// The same write against the migrated foundation generation.
[[nodiscard]] ReturnWriteResult
WriteInvocationReturn(lua_State *State, const ReturnMetadata &Metadata,
                      const InvocationOutcome &Outcome,
                      FaultInjector &Faults) noexcept;

} // namespace Luna::Detail
