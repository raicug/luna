// clang-format off
#include "state/invocation/trampoline.hpp"

#include "state/binding/record.hpp"
#include "state/invocation/conversion/return_writer.hpp"
#include "state/invocation/validation/validator.hpp"
#include "state/testing/fault_injector.hpp"

#include <lua.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

struct InvocationResult final {
  int ReturnCount = -1;
  std::string Diagnostic;

  [[nodiscard]] bool IsSuccess() const noexcept { return ReturnCount >= 0; }
};

[[nodiscard]] std::string CallableContext(std::string_view GlobalName) {
  return "callable '" + std::string(GlobalName) + "'";
}

[[nodiscard]] InvocationResult Failure(std::string Message) {
  if (Message.empty())
    Message = "Internal Luna invocation error.";
  return {.ReturnCount = -1, .Diagnostic = std::move(Message)};
}

[[nodiscard]] InvocationResult InvokeValidated(lua_State *State,
                                               BindingRecord &Record,
                                               FaultInjector &Faults) {
  const std::string_view GlobalName = Record.GlobalName();
  try {
    auto Validated = ValidateInvocation(
        State, GlobalName, &Record.Descriptor().Metadata(), Faults);
    if (!Validated.Validation.IsSuccess()) {
      const auto *Diagnostic = Validated.Validation.Diagnostic();
      return Failure(Diagnostic ? Diagnostic->Message()
                                : "Internal invocation validation error for " +
                                      CallableContext(GlobalName) + ".");
    }

    try {
      auto Outcome = Record.Descriptor().Invoke(Validated.Arguments);
      if (Outcome.Kind() == InvocationOutcomeKind::InternalFailure)
        return Failure("Internal error for " + CallableContext(GlobalName) +
                       ": " + Outcome.FailureMessage());

      auto Written = WriteInvocationReturn(
          State, Record.Descriptor().Metadata().ReturnType(), Outcome, Faults);
      if (!Written.IsSuccess()) {
        const std::string Message = Written.Diagnostic
                                        ? Written.Diagnostic->Message()
                                        : "Return handling failed.";
        return Failure("Internal error for " + CallableContext(GlobalName) +
                       ": " + Message);
      }
      return {.ReturnCount = Written.ReturnCount};
    } catch (const std::exception &Error) {
      return Failure("Runtime error: " + CallableContext(GlobalName) +
                     " threw: " + Error.what());
    } catch (...) {
      return Failure("Internal error: " + CallableContext(GlobalName) +
                     " threw an unknown C++ exception.");
    }
  } catch (const std::exception &Error) {
    return Failure("Internal error while dispatching " +
                   CallableContext(GlobalName) + ": " + Error.what());
  } catch (...) {
    return Failure("Internal error while dispatching " +
                   CallableContext(GlobalName) + ".");
  }
}

} // namespace

int NativeTrampoline(lua_State *State) {
  if (!State)
    return 0;

  constexpr std::size_t LocalDiagnosticCapacity = 512;
  const int EntryDepth = lua_gettop(State);
  char LocalDiagnostic[LocalDiagnosticCapacity]{};
  char *PreparedDiagnostic = LocalDiagnostic;
  std::size_t DiagnosticLength = 0;
  bool HeapDiagnostic = false;
  FaultInjector *Faults = nullptr;

  {
    try {
      InvocationResult Result;
      auto *Record = static_cast<BindingRecord *>(
          lua_tolightuserdata(State, lua_upvalueindex(1)));
      if (!Record || !Record->IsCommitted()) {
        Result = Failure("Internal error: binding record is unavailable.");
      } else if (!Record->Faults()) {
        Result = Failure(
            "Internal error: binding fault context is unavailable for " +
            CallableContext(Record->GlobalName()) + ".");
      } else {
        Faults = Record->Faults();
        Faults->ClearCallbackStackRestoration();
        Result = InvokeValidated(State, *Record, *Faults);
      }

      if (Result.IsSuccess())
        return Result.ReturnCount;

      DiagnosticLength = Result.Diagnostic.size();
      if (DiagnosticLength > LocalDiagnosticCapacity) {
        PreparedDiagnostic = static_cast<char *>(std::malloc(DiagnosticLength));
        HeapDiagnostic = PreparedDiagnostic != nullptr;
      }
      if (!PreparedDiagnostic) {
        static constexpr char Fallback[] =
            "Internal Luna invocation error: diagnostic allocation failed.";
        PreparedDiagnostic = LocalDiagnostic;
        DiagnosticLength = sizeof(Fallback) - 1;
        std::memcpy(PreparedDiagnostic, Fallback, DiagnosticLength);
      } else if (DiagnosticLength != 0) {
        std::memcpy(PreparedDiagnostic, Result.Diagnostic.data(),
                    DiagnosticLength);
      }
    } catch (...) {
      static constexpr char Fallback[] = "Internal Luna invocation error.";
      PreparedDiagnostic = LocalDiagnostic;
      DiagnosticLength = sizeof(Fallback) - 1;
      std::memcpy(PreparedDiagnostic, Fallback, DiagnosticLength);
    }
  }

  if (DiagnosticLength == 0) {
    static constexpr char Fallback[] = "Internal Luna invocation error.";
    PreparedDiagnostic = LocalDiagnostic;
    DiagnosticLength = sizeof(Fallback) - 1;
    std::memcpy(PreparedDiagnostic, Fallback, DiagnosticLength);
  }

  // Only trivially destructible locals remain in this minimal error tail.
  lua_settop(State, EntryDepth);
  const int RestoredDepth = lua_gettop(State);
  lua_pushlstring(State, PreparedDiagnostic, DiagnosticLength);
  if (Faults)
    Faults->RecordCallbackStackRestoration(EntryDepth, RestoredDepth,
                                           lua_gettop(State));
  if (HeapDiagnostic)
    std::free(PreparedDiagnostic);
  lua_error(State);
  return 0;
}

} // namespace Luna::Detail