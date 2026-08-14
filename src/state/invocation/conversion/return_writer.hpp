#pragma once

// clang-format off
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include "state/type/type_generation.hpp"

#include <optional>
#include <span>
#include <string>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class FaultInjector;

enum class ReturnWriteStatus {
  ValueWritten,
  VoidCompleted,
  Suppressed,
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
WriteDynamicReturnPack(lua_State *State, const ReturnMetadata &Metadata,
                       std::span<const Value> Values,
                       const TypeGeneration &Types,
                       FaultInjector &Faults) noexcept;

[[nodiscard]] ReturnWriteResult
WriteInvocationReturn(lua_State *State, const ReturnMetadata &Metadata,
                      const InvocationOutcome &Outcome,
                      const TypeGeneration &Types,
                      FaultInjector &Faults) noexcept;

[[nodiscard]] ReturnWriteResult
WriteInvocationReturn(lua_State *State, const ReturnMetadata &Metadata,
                      const InvocationOutcome &Outcome,
                      FaultInjector &Faults) noexcept;

} // namespace Luna::Detail
