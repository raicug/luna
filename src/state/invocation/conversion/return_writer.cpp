// clang-format off
#include "state/invocation/conversion/return_writer.hpp"

#include "state/invocation/conversion/argument_reader.hpp"
#include "state/testing/fault_injector.hpp"

#include <lua.h>

#include <string>
#include <type_traits>
#include <utility>
#include <variant>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] ReturnWriteResult Failure(lua_State *State, int EntryDepth,
                                        std::string Message) {
  if (State)
    lua_settop(State, EntryDepth);
  return {.Status = ReturnWriteStatus::InternalFailure,
          .ReturnCount = 0,
          .Diagnostic = ErrorDiagnostic::Create(ErrorCategory::Internal,
                                                std::move(Message))};
}

[[nodiscard]] bool ValueMatches(ValueKind Kind, const Value &ReturnedValue) {
  switch (Kind) {
  case ValueKind::Boolean:
    return std::holds_alternative<bool>(ReturnedValue);
  case ValueKind::Integer:
    return std::holds_alternative<int>(ReturnedValue);
  case ValueKind::Number:
    return std::holds_alternative<double>(ReturnedValue);
  case ValueKind::String:
    return std::holds_alternative<std::string>(ReturnedValue);
  }
  return false;
}

void PushValue(lua_State *State, const Value &ReturnedValue) {
  std::visit(
      [State](const auto &TypedValue) {
        using Type = std::decay_t<decltype(TypedValue)>;
        if constexpr (std::is_same_v<Type, bool>)
          lua_pushboolean(State, TypedValue ? 1 : 0);
        else if constexpr (std::is_same_v<Type, int>)
          lua_pushinteger(State, TypedValue);
        else if constexpr (std::is_same_v<Type, double>)
          lua_pushnumber(State, TypedValue);
        else
          lua_pushlstring(State, TypedValue.data(), TypedValue.size());
      },
      ReturnedValue);
}

} // namespace

ReturnWriteResult WriteInvocationReturn(lua_State *State,
                                        const ReturnMetadata &Metadata,
                                        const InvocationOutcome &Outcome,
                                        FaultInjector &Faults) noexcept {
  const int EntryDepth = State ? lua_gettop(State) : 0;
  try {
    switch (Metadata.Disposition()) {
    case ReturnDisposition::Suppress:
      return {.Status = ReturnWriteStatus::Suppressed, .ReturnCount = 0};

    case ReturnDisposition::Void:
      if (Faults.Consume(StateFaultPoint::VoidFinalization))
        return Failure(State, EntryDepth,
                       "Injected internal void-finalization failure.");
      if (Outcome.Kind() != InvocationOutcomeKind::Void)
        return Failure(State, EntryDepth,
                       "Void return metadata did not match callable outcome.");
      return {.Status = ReturnWriteStatus::VoidCompleted, .ReturnCount = 0};

    case ReturnDisposition::Value:
      if (!State)
        return Failure(State, EntryDepth,
                       "Return writer has no invocation stack.");
      if (!Metadata.Kind() || Outcome.Kind() != InvocationOutcomeKind::Value ||
          !Outcome.ReturnedValue())
        return Failure(State, EntryDepth,
                       "Value return metadata did not match callable outcome.");
      if (!ValueMatches(*Metadata.Kind(), *Outcome.ReturnedValue()))
        return Failure(State, EntryDepth,
                       "Returned value type did not match callable metadata.");
      if (const auto *String =
              std::get_if<std::string>(Outcome.ReturnedValue());
          String && String->size() > MaximumInvocationStringBytes)
        return Failure(State, EntryDepth,
                       "Returned string exceeds the 1048576-byte maximum.");
      if (Faults.Consume(StateFaultPoint::ReturnStackCapacity) ||
          !lua_checkstack(State, 1))
        return Failure(State, EntryDepth,
                       "Could not reserve stack capacity for return value.");

      PushValue(State, *Outcome.ReturnedValue());
      if (Faults.Consume(StateFaultPoint::ReturnWrite))
        return Failure(State, EntryDepth,
                       "Injected internal return-writer failure.");
      return {.Status = ReturnWriteStatus::ValueWritten, .ReturnCount = 1};
    }
  } catch (...) {
    try {
      return Failure(State, EntryDepth,
                     "Unexpected internal return conversion failure.");
    } catch (...) {
      if (State)
        lua_settop(State, EntryDepth);
      return {};
    }
  }

  return Failure(State, EntryDepth,
                 "Unknown return disposition prevented conversion.");
}

} // namespace Luna::Detail
