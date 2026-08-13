// clang-format off
#include "state/execution/executor.hpp"

#include "state/execution/interrupt.hpp"
#include "state/invocation/async/suspended_call.hpp"
#include "state/testing/fault_injector.hpp"
#include "state/type/owned_value_bridge.hpp"
#include "state/type/type_generation.hpp"
#include "state/vm/stack_checkpoint.hpp"

#include <Luau/Compiler.h>
#include <lua.h>

#include <climits>
#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
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

[[nodiscard]] int ConfigureExecutionEnvironment(lua_State *Root,
                                                lua_State *Thread,
                                                const ExecutionPolicy &Policy) {
  if (!Policy.IsIsolated())
    return 0;

  const std::vector<std::string> &Allowed = Policy.AllowedGlobals();
  if (Allowed.size() > static_cast<std::size_t>(INT_MAX - 2) ||
      !lua_checkstack(Root, 1) ||
      !lua_checkstack(Thread, static_cast<int>(Allowed.size()) + 2))
    return -1;

  lua_newtable(Thread);
  const int Environment = lua_gettop(Thread);
  lua_pushvalue(Thread, Environment);
  lua_rawsetfield(Thread, Environment, "_G");

  for (const std::string &Name : Allowed) {
    if (Name == "_G")
      continue;
    lua_getglobal(Root, Name.c_str());
    lua_xmove(Root, Thread, 1);
    lua_rawsetfield(Thread, Environment, Name.c_str());
  }

  if (Policy.AreGlobalsReadOnly())
    lua_setreadonly(Thread, Environment, 1);
  return Environment;
}

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
                              const ExecutionPolicy &Policy,
                              FaultInjector &Faults, AsyncCallRegistry *Async) {
  if (!Root)
    return InternalFailure("execution root is unavailable.");

  InterruptRequest *const Pending = ObserveInterruptRequest(Root);
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
    const int Environment = ConfigureExecutionEnvironment(Root, Thread, Policy);
    if (Environment < 0)
      return InternalFailure("could not create the execution environment.");
    const int LoadStatus = luau_load(Thread, "=Luna", Bytecode.data(),
                                     Bytecode.size(), Environment);
    if (Environment > 0)
      lua_remove(Thread, Environment);
    if (LoadStatus != LUA_OK) {
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
      if (Pending && Pending->IsPending()) {
        const std::string Composed = Pending->Composed();
        Async->CancelFor(Thread, Composed);
        return ExecutionResult::Interrupted(Composed);
      }
      static_cast<void>(Async->Advance(Thread));
      Status = lua_resume(Thread, nullptr, 0);
    }

    if (Status != LUA_OK) {
      const bool ForceFallback =
          Faults.Consume(StateFaultPoint::ExecutionErrorDiagnostic);
      const std::string Reported = ReadTopError(
          Thread, "execution failed without a Luau diagnostic.", ForceFallback);
      if (Pending && Pending->IsPending())
        return ExecutionResult::Interrupted(Reported.find(InterruptPrefix) !=
                                                    std::string::npos
                                                ? Reported
                                                : Pending->Composed());
      return ExecutionResult::Failure(
          ErrorCategory::Runtime,
          Prefixed(RuntimePrefix, Reported,
                   "execution failed without a Luau diagnostic."));
    }

    return ExecutionResult::Success();
  } catch (const std::exception &Error) {
    return InternalFailure(Error.what());
  } catch (...) {
    return InternalFailure("unknown failure during source execution.");
  }
}

bool CompileChunk(lua_State *Root, std::string_view Source,
                  std::string_view Name, std::string &Bytecode,
                  std::string &Diagnostic) {
  if (!Root) {
    Diagnostic = "execution root is unavailable.";
    return false;
  }

  try {
    Bytecode = Luau::compile(std::string(Source));

    StackCheckpoint RootCheckpoint(Root);
    ThreadCreationContext Context;
    const int CreationStatus =
        lua_cpcall(Root, CreateExecutionThread, &Context);
    DisposableExecutionThread Disposable(Root, Context);
    if (CreationStatus != LUA_OK || !Context.Thread ||
        Context.Reference <= LUA_REFNIL) {
      Diagnostic = "could not create and pin a validation thread.";
      return false;
    }

    lua_State *Thread = Disposable.Get();
    const std::string ChunkName = std::string("=") + std::string(Name);
    if (luau_load(Thread, ChunkName.c_str(), Bytecode.data(), Bytecode.size(),
                  0) != LUA_OK) {
      Diagnostic =
          Prefixed(CompilationPrefix,
                   ReadTopError(Thread, "compiler rejected the source."),
                   "compiler rejected the source.");
      Bytecode.clear();
      return false;
    }
    return true;
  } catch (const std::exception &Error) {
    Diagnostic = Error.what();
    Bytecode.clear();
    return false;
  } catch (...) {
    Diagnostic = "unknown failure while compiling a chunk.";
    Bytecode.clear();
    return false;
  }
}

ChunkResult InvokeChunk(lua_State *Root, std::string_view Bytecode,
                        std::string_view Name, const ValuePack &Arguments,
                        FaultInjector *Faults, AsyncCallRegistry *Async,
                        const ExecutionPolicy &Policy) {
  if (!Root)
    return ChunkResult::Failure(
        ErrorCategory::Internal,
        Prefixed(InternalPrefix, "execution root is unavailable.", ""));

  InterruptRequest *const Pending = ObserveInterruptRequest(Root);
  StackCheckpoint RootCheckpoint(Root);
  try {
    if (Faults && Faults->Consume(StateFaultPoint::ExecutionThreadCreation))
      return ChunkResult::Failure(
          ErrorCategory::Internal,
          Prefixed(InternalPrefix,
                   "could not create disposable execution thread.", ""));

    ThreadCreationContext Context;
    const int CreationStatus =
        lua_cpcall(Root, CreateExecutionThread, &Context);
    DisposableExecutionThread Disposable(Root, Context);
    if (CreationStatus != LUA_OK || !Context.Thread ||
        Context.Reference <= LUA_REFNIL)
      return ChunkResult::Failure(
          ErrorCategory::Internal,
          Prefixed(InternalPrefix,
                   "could not create and pin disposable execution thread.",
                   ""));

    lua_State *Thread = Disposable.Get();
    const int Environment = ConfigureExecutionEnvironment(Root, Thread, Policy);
    if (Environment < 0)
      return ChunkResult::Failure(
          ErrorCategory::Internal,
          Prefixed(InternalPrefix,
                   "could not create the execution environment.", ""));
    const std::string ChunkName = std::string("=") + std::string(Name);
    const int LoadStatus = luau_load(Thread, ChunkName.c_str(), Bytecode.data(),
                                     Bytecode.size(), Environment);
    if (Environment > 0)
      lua_remove(Thread, Environment);
    if (LoadStatus != LUA_OK)
      return ChunkResult::Failure(
          ErrorCategory::Compilation,
          Prefixed(CompilationPrefix,
                   ReadTopError(Thread, "loader rejected the bytecode."),
                   "loader rejected the bytecode."));

    const std::shared_ptr<const TypeGeneration> Types =
        CaptureOwnedValueTypes(Thread);
    if (!Types)
      return ChunkResult::Failure(
          ErrorCategory::Internal,
          Prefixed(InternalPrefix, "the chunk has no captured type registry.",
                   ""));

    for (std::size_t Index = 0; Index < Arguments.Size(); ++Index) {
      const std::string Refusal =
          ClassifyPendingInstances(Arguments.At(Index), *Types);
      if (!Refusal.empty())
        return ChunkResult::Failure(
            ErrorCategory::Internal,
            Prefixed(InternalPrefix,
                     "chunk argument " + std::to_string(Index + 1) +
                         " cannot be published: " + Refusal,
                     ""));
    }

    if (!lua_checkstack(Thread, static_cast<int>(Arguments.Size()) + 4))
      return ChunkResult::Failure(
          ErrorCategory::Internal,
          Prefixed(InternalPrefix,
                   "could not reserve stack capacity for the chunk arguments.",
                   ""));

    for (std::size_t Index = 0; Index < Arguments.Size(); ++Index) {
      if (!PushOwnedValueToStack(Thread, Arguments.At(Index), *Types))
        return ChunkResult::Failure(ErrorCategory::Internal,
                                    Prefixed(InternalPrefix,
                                             "chunk argument " +
                                                 std::to_string(Index + 1) +
                                                 " could not be published.",
                                             ""));
    }

    const PumpedExecutionThread Pumped(Async, Thread);
    int Status =
        lua_resume(Thread, nullptr, static_cast<int>(Arguments.Size()));
    while (Status == LUA_YIELD) {
      if (!Async || !Async->HasPendingFor(Thread))
        return ChunkResult::Failure(
            ErrorCategory::Runtime,
            Prefixed(RuntimePrefix,
                     "the invoked chunk yielded without any suspended Luna "
                     "call to resume.",
                     ""));
      if (Pending && Pending->IsPending()) {
        const std::string Composed = Pending->Composed();
        Async->CancelFor(Thread, Composed);
        return ChunkResult::Failure(
            ErrorDiagnostic::Create(ErrorCategory::Interrupted, Composed));
      }
      static_cast<void>(Async->Advance(Thread));
      Status = lua_resume(Thread, nullptr, 0);
    }

    if (Status != LUA_OK) {
      const std::string Reported =
          ReadTopError(Thread, "the chunk failed without a Luau diagnostic.");
      if (Pending && Pending->IsPending())
        return ChunkResult::Failure(ErrorDiagnostic::Create(
            ErrorCategory::Interrupted,
            Reported.find(InterruptPrefix) != std::string::npos
                ? Reported
                : Pending->Composed()));
      return ChunkResult::Failure(
          ErrorCategory::Runtime,
          Prefixed(RuntimePrefix, Reported,
                   "the chunk failed without a Luau diagnostic."));
    }

    ValuePack Produced;
    const int ResultCount = lua_gettop(Thread);
    for (int Index = 1; Index <= ResultCount; ++Index)
      Produced.Append(BuildOwnedValueFromStack(Thread, Index));
    return ChunkResult::Delivered(std::move(Produced));
  } catch (const std::exception &Error) {
    return ChunkResult::Failure(
        ErrorCategory::Internal,
        Prefixed(InternalPrefix, Error.what(), "chunk invocation failed."));
  } catch (...) {
    return ChunkResult::Failure(
        ErrorCategory::Internal,
        Prefixed(InternalPrefix, "unknown failure during chunk invocation.",
                 ""));
  }
}

} // namespace Luna::Detail
