// clang-format off
#include "state/execution/executor.hpp"

#include "state/invocation/async/suspended_call.hpp"
#include "state/testing/fault_injector.hpp"
#include "state/vm/stack_checkpoint.hpp"

#include <Luau/Compiler.h>
#include <lua.h>

#include <exception>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr std::string_view CompilationPrefix = "Compilation error:";
constexpr std::string_view RuntimePrefix = "Runtime error:";
constexpr std::string_view InternalPrefix = "Internal error:";

[[nodiscard]] std::string Prefixed(std::string_view Prefix, std::string Reason,
                                   std::string_view Fallback) {
  if (Reason.empty())
    Reason.assign(Fallback);
  if (Reason.starts_with(Prefix))
    return Reason;
  return std::string(Prefix) + " " + Reason;
}

[[nodiscard]] std::string ReadTopError(lua_State *State,
                                       std::string_view Fallback,
                                       bool ForceFallback = false) {
  if (ForceFallback)
    return std::string(Fallback);

  std::size_t Length = 0;
  const char *Message = State ? lua_tolstring(State, -1, &Length) : nullptr;
  return Message ? std::string(Message, Length) : std::string(Fallback);
}

struct ThreadCreationContext final {
  lua_State *Thread = nullptr;
  int Reference = LUA_NOREF;
};

int CreateExecutionThread(lua_State *State) {
  auto *Context =
      static_cast<ThreadCreationContext *>(lua_tolightuserdata(State, 1));
  if (!Context)
    return 0;

  Context->Thread = lua_newthread(State);
  if (Context->Thread)
    Context->Reference = lua_ref(State, -1);
  return 0;
}

class DisposableExecutionThread final {
public:
  DisposableExecutionThread(lua_State *Root,
                            ThreadCreationContext Context) noexcept
      : Root(Root), Thread(Context.Thread), Reference(Context.Reference) {}

  ~DisposableExecutionThread() {
    if (Thread)
      lua_resetthread(Thread);
    if (Root && Reference > LUA_REFNIL)
      lua_unref(Root, Reference);
  }

  DisposableExecutionThread(const DisposableExecutionThread &) = delete;
  DisposableExecutionThread &
  operator=(const DisposableExecutionThread &) = delete;

  [[nodiscard]] lua_State *Get() const noexcept { return Thread; }

private:
  lua_State *Root = nullptr;
  lua_State *Thread = nullptr;
  int Reference = LUA_NOREF;
};

[[nodiscard]] ExecutionResult InternalFailure(std::string Reason) {
  return ExecutionResult::Failure(
      ErrorCategory::Internal,
      Prefixed(InternalPrefix, std::move(Reason),
               "source execution failed without a diagnostic."));
}

class PumpedExecutionThread final {
public:
  PumpedExecutionThread(AsyncCallRegistry *Async, lua_State *Thread) noexcept
      : Async(Async), Thread(Thread) {
    if (Async)
      Async->BindPumpThread(Thread);
  }

  ~PumpedExecutionThread() {
    if (!Async)
      return;
    Async->CancelFor(Thread,
                     "the execution that suspended it already finished");
    Async->BindPumpThread(nullptr);
  }

  PumpedExecutionThread(const PumpedExecutionThread &) = delete;
  PumpedExecutionThread &operator=(const PumpedExecutionThread &) = delete;

private:
  AsyncCallRegistry *Async = nullptr;
  lua_State *Thread = nullptr;
};

} // namespace

ExecutionResult ExecuteSource(lua_State *Root, std::string_view Source,
                              FaultInjector &Faults, AsyncCallRegistry *Async) {
  if (!Root)
    return InternalFailure("execution root is unavailable.");

  StackCheckpoint RootCheckpoint(Root);
  try {
    const std::string OwnedSource(Source);
    const std::string Bytecode = Luau::compile(OwnedSource);

    if (Faults.Consume(StateFaultPoint::ExecutionThreadCreation))
      return InternalFailure("could not create disposable execution thread.");

    ThreadCreationContext Context;
    const int CreationStatus =
        lua_cpcall(Root, CreateExecutionThread, &Context);
    DisposableExecutionThread Disposable(Root, Context);
    if (CreationStatus != LUA_OK || !Context.Thread ||
        Context.Reference <= LUA_REFNIL) {
      return InternalFailure(ReadTopError(
          Root, "could not create and pin disposable execution thread."));
    }

    lua_State *Thread = Disposable.Get();
    if (luau_load(Thread, "=Luna", Bytecode.data(), Bytecode.size(), 0) !=
        LUA_OK) {
      return ExecutionResult::Failure(
          ErrorCategory::Compilation,
          Prefixed(CompilationPrefix,
                   ReadTopError(Thread, "compiler rejected the source."),
                   "compiler rejected the source."));
    }

    const PumpedExecutionThread Pumped(Async, Thread);
    int Status = lua_resume(Thread, nullptr, 0);
    while (Status == LUA_YIELD) {
      if (!Async || !Async->HasPendingFor(Thread)) {
        return ExecutionResult::Failure(
            ErrorCategory::Runtime,
            Prefixed(RuntimePrefix,
                     "the executed chunk yielded without any suspended Luna "
                     "call to resume.",
                     "the executed chunk yielded unexpectedly."));
      }
      static_cast<void>(Async->Advance(Thread));
      Status = lua_resume(Thread, nullptr, 0);
    }

    if (Status != LUA_OK) {
      const bool ForceFallback =
          Faults.Consume(StateFaultPoint::ExecutionErrorDiagnostic);
      return ExecutionResult::Failure(
          ErrorCategory::Runtime,
          Prefixed(RuntimePrefix,
                   ReadTopError(Thread,
                                "execution failed without a Luau diagnostic.",
                                ForceFallback),
                   "execution failed without a Luau diagnostic."));
    }

    return ExecutionResult::Success();
  } catch (const std::exception &Error) {
    return InternalFailure(Error.what());
  } catch (...) {
    return InternalFailure("unknown failure during source execution.");
  }
}

} // namespace Luna::Detail
