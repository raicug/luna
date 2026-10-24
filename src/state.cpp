#include <luna/state.hpp>

#include "runtime.hpp"

#include <lua.h>
#include <luacode.h>
#include <lualib.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace luna {
namespace {

// The runtime currently being resumed, so a binding can suspend without being
// handed the State.
thread_local detail::Runtime *tRunningRuntime = nullptr;

constexpr const char *kChunkName = "=luna";
} // namespace

namespace detail {

// A compiled chunk: the closure reference plus the VM that owns it.
struct ChunkState {
  lua_State *VM = nullptr;
  Runtime *R = nullptr;
  int FunctionRef = LUA_NOREF;
};

namespace {

void InterruptHook(lua_State *L, int Gc) {
  if (Gc)
    return;

  detail::Runtime *R = detail::RuntimeFromLua(L);
  if (!R || !R->Stop.load(std::memory_order_acquire))
    return;

  const std::string &Reason = R->StopReason;
  lua_pushlstring(L, Reason.data(), Reason.size());
  lua_error(L); // unwinds out of the VM at this safepoint
}

bool CompileInto(lua_State *Target, std::string_view Source,
                 std::string &Error) {
  std::size_t BytecodeSize = 0;
  char *Bytecode =
      luau_compile(Source.data(), Source.size(), nullptr, &BytecodeSize);
  if (!Bytecode) {
    Error = "compilation failed to produce bytecode";
    return false;
  }

  const int Status =
      luau_load(Target, kChunkName, Bytecode, BytecodeSize, 0);

  std::free(Bytecode);

  if (Status == 0)
    return true;

  const char *Message = lua_tostring(Target, -1);
  Error = Message ? Message : "compilation failed";
  lua_pop(Target, 1);
  return false;
}

std::string TakeError(lua_State *L) {
  const char *Message = lua_tostring(L, -1);
  std::string Text = Message ? Message : "unknown error";
  lua_pop(L, 1);
  return Text;
}

} // namespace

} // namespace detail

// ---------------------------------------------------------------------------
// Runtime plumbing
// ---------------------------------------------------------------------------

State *StateFromVM(void *VM) noexcept {
  detail::Runtime *R = detail::RuntimeFromLua(static_cast<lua_State *>(VM));
  return R ? R->Owner : nullptr;
}

bool SuspendCurrentChunk(void *VM, double DelaySeconds) {
  (void)VM;
  // Consumed by the trampoline that is about to return.
  detail::SuspensionRequested = true;
  detail::SuspensionDelay = DelaySeconds > 0.0 ? DelaySeconds : 0.0;
  return true;
}

namespace detail {

// Called by the trampoline after a callable returns. When the callable asked to
// suspend, this records the due time against the running task and tells the
// trampoline to yield.
bool CompleteSuspension(lua_State *L) {
  if (!SuspensionRequested)
    return false;

  SuspensionRequested = false;
  const double Delay = SuspensionDelay;
  SuspensionDelay = 0.0;

  Runtime *R = RuntimeFromLua(L);
  if (!R || !R->HasRunning)
    return false;

  R->Tasks[R->RunningIndex].Due = R->Clock + Delay;
  return true;
}

} // namespace detail

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

State::State() : Core(std::make_unique<detail::Runtime>(this)) {
  lua_State *L = luaL_newstate();
  if (!L)
    return;

  luaL_openlibs(L);

  Core->L = L;
  Core->World.Attach(L);

  lua_Callbacks *Callbacks = lua_callbacks(L);
  Callbacks->userdata = Core.get();
  Callbacks->interrupt = &detail::InterruptHook;
}

State::~State() = default;

State::State(State &&Other) noexcept : Core(std::move(Other.Core)) {
  if (Core) {
    Core->Owner = this;
    if (Core->L) {
      lua_Callbacks *Callbacks = lua_callbacks(Core->L);
      Callbacks->userdata = Core.get();
    }
  }
}

State &State::operator=(State &&Other) noexcept {
  if (this == &Other)
    return *this;
  Core = std::move(Other.Core);
  if (Core) {
    Core->Owner = this;
    if (Core->L) {
      lua_Callbacks *Callbacks = lua_callbacks(Core->L);
      Callbacks->userdata = Core.get();
    }
  }
  return *this;
}

bool State::IsReady() const noexcept { return Core && Core->L != nullptr; }

BindingRegistry State::Bindings() { return BindingRegistry(this); }

detail::Runtime *State::GetRuntime() noexcept { return Core.get(); }

// Extracts a task's completion status, releasing its thread when done.
namespace {

int ResumeTask(detail::Runtime &R, detail::Task &Entry) {
  R.HasRunning = true;
  detail::Runtime *Previous = tRunningRuntime;
  tRunningRuntime = &R;

  const int Status = lua_resume(Entry.Co, R.L, 0);

  tRunningRuntime = Previous;
  R.HasRunning = false;
  return Status;
}

// Resumes one task and reports whether it finished.
bool PumpOnce(detail::Runtime &R, std::size_t Index, std::string &Error) {
  detail::Task &Entry = R.Tasks[Index];
  if (Entry.Dead)
    return true;

  const int Status = ResumeTask(R, Entry);

  if (Status == 0)
    return false; // finished, still needs collection

  if (Status == LUA_YIELD)
    return true;

  Error = detail::TakeError(Entry.Co);
  return false;
}

void ReleaseTask(detail::Runtime &R, std::size_t Index) {
  detail::Task &Entry = R.Tasks[Index];
  if (Entry.Dead)
    return;
  if (Entry.ThreadRef != LUA_NOREF)
    luna::detail::Unpin(R.L, Entry.ThreadRef);
  Entry.Dead = true;
}

} // namespace

ExecutionResult State::Execute(std::string_view Source) {
  if (!IsReady())
    return ExecutionResult(
        ErrorDiagnostic(ErrorCategory::Runtime, "the state is not ready"));

  detail::Runtime &R = *Core;

  if (R.Stop.load(std::memory_order_acquire)) {
    const std::string Reason = R.StopReason;
    return ExecutionResult(ErrorDiagnostic(ErrorCategory::Interrupted, Reason));
  }

  lua_State *Co = lua_newthread(R.L);
  const int ThreadRef = luna::detail::PinTop(R.L);

  std::string Error;
  if (!detail::CompileInto(Co, Source, Error)) {
    luna::detail::Unpin(R.L, ThreadRef);
    return ExecutionResult(
        ErrorDiagnostic(ErrorCategory::Compile, std::move(Error)));
  }

  detail::Task Entry;
  Entry.Co = Co;
  Entry.ThreadRef = ThreadRef;
  Entry.Id = R.NextId++;
  Entry.Due = R.Clock;

  R.Tasks.push_back(Entry);
  const std::size_t Index = R.Tasks.size() - 1;

  std::string Failure;
  for (;;) {
    // Give other ready tasks a turn before resuming this one, so a spawned task
    // is not starved by a chunk that runs to completion.
    for (std::size_t Other = 0; Other < R.Tasks.size(); ++Other) {
      if (Other == Index || R.Tasks[Other].Dead)
        continue;
      if (R.Tasks[Other].Due > R.Clock)
        continue;
      std::string Ignored;
      if (!PumpOnce(R, Other, Ignored))
        ReleaseTask(R, Other);
    }

    const bool Alive = PumpOnce(R, Index, Failure);
    if (!Failure.empty()) {
      ReleaseTask(R, Index);
      return ExecutionResult(
          ErrorDiagnostic(ErrorCategory::Runtime, std::move(Failure)));
    }
    if (!Alive) {
      ReleaseTask(R, Index);
      return ExecutionResult();
    }

    // Suspended: jump the clock to this task's due time and continue. A task
    // that never set a due time resumes immediately.
    if (R.Tasks[Index].Due > R.Clock)
      R.Clock = R.Tasks[Index].Due;
  }
}

Chunk State::Load(std::string_view Source, std::string_view Name) {
  if (!IsReady())
    return Chunk();

  detail::Runtime &R = *Core;
  (void)Name;

  lua_State *Co = lua_newthread(R.L);
  const int ThreadRef = luna::detail::PinTop(R.L);

  std::string Error;
  if (!detail::CompileInto(Co, Source, Error)) {
    luna::detail::Unpin(R.L, ThreadRef);
    return Chunk();
  }

  // Move the compiled closure into the registry and drop the coroutine; Call()
  // makes a fresh one each time.
  const int FunctionRef = luna::detail::PinTop(Co);
  luna::detail::Unpin(R.L, ThreadRef);

  auto State = std::make_shared<detail::ChunkState>();
  State->VM = R.L;
  State->R = &R;
  State->FunctionRef = FunctionRef;

  Chunk Result;
  Result.Data = std::move(State);
  return Result;
}

std::uint64_t State::Spawn(std::string_view Source) {
  if (!IsReady())
    return 0;

  detail::Runtime &R = *Core;

  lua_State *Co = lua_newthread(R.L);
  const int ThreadRef = luna::detail::PinTop(R.L);

  std::string Error;
  if (!detail::CompileInto(Co, Source, Error)) {
    luna::detail::Unpin(R.L, ThreadRef);
    return 0;
  }

  detail::Task Entry;
  Entry.Co = Co;
  Entry.ThreadRef = ThreadRef;
  Entry.Id = R.NextId++;
  Entry.Due = R.Clock;

  R.Tasks.push_back(Entry);
  const std::size_t Index = R.Tasks.size() - 1;

  std::string Failure;
  if (!PumpOnce(R, Index, Failure))
    ReleaseTask(R, Index);

  return R.Tasks[Index].Id;
}

void State::Tick(double DeltaSeconds) {
  if (!IsReady())
    return;

  detail::Runtime &R = *Core;
  if (DeltaSeconds > 0.0)
    R.Clock += DeltaSeconds;

  // One pass is enough: a resumed task that suspends again pushed its due time
  // into the future, so it is not due in this same frame.
  const std::size_t Count = R.Tasks.size();
  for (std::size_t Index = 0; Index < Count; ++Index) {
    detail::Task &Entry = R.Tasks[Index];
    if (Entry.Dead || Entry.Due > R.Clock)
      continue;

    std::string Failure;
    if (!PumpOnce(R, Index, Failure))
      ReleaseTask(R, Index);
  }

  // Compact finished tasks so the vector does not grow without bound.
  if (R.Tasks.size() > 32) {
    std::vector<detail::Task> Live;
    Live.reserve(R.Tasks.size());
    for (detail::Task &Entry : R.Tasks) {
      if (!Entry.Dead)
        Live.push_back(Entry);
    }
    R.Tasks = std::move(Live);
  }
}

bool State::Cancel(std::uint64_t Handle) {
  if (!IsReady())
    return false;

  detail::Runtime &R = *Core;
  for (std::size_t Index = 0; Index < R.Tasks.size(); ++Index) {
    if (R.Tasks[Index].Id != Handle || R.Tasks[Index].Dead)
      continue;
    ReleaseTask(R, Index);
    return true;
  }
  return false;
}

std::uint32_t State::PendingTasks() const noexcept {
  if (!Core)
    return 0;
  std::uint32_t Live = 0;
  for (const detail::Task &Entry : Core->Tasks) {
    if (!Entry.Dead)
      ++Live;
  }
  return Live;
}

void State::RequestInterrupt(std::string Reason) {
  if (!Core)
    return;
  Core->StopReason = std::move(Reason);
  Core->Stop.store(true, std::memory_order_release);
}

void State::ClearInterrupt() noexcept {
  if (!Core)
    return;
  Core->Stop.store(false, std::memory_order_release);
  Core->StopReason.clear();
}

bool State::IsInterruptPending() const noexcept {
  return Core && Core->Stop.load(std::memory_order_acquire);
}

void State::Suspend(double DelaySeconds) {
  SuspendCurrentChunk(nullptr, DelaySeconds);
}

// The form a native callable uses. The thread-local it sets is read by
// CompleteSuspension, which the trampoline calls right after the callable
// returns.
void Suspend(double DelaySeconds) { SuspendCurrentChunk(nullptr, DelaySeconds); }

SymbolRange State::Symbols() const {
  if (!Core)
    return SymbolRange();
  const auto List = Core->World.Snapshot();
  return SymbolRange(List, List->data(), List->size());
}

ReflectionSnapshot State::Reflection() const {
  if (!Core)
    return ReflectionSnapshot();
  return ReflectionSnapshot(Core->World.Snapshot());
}

// ---------------------------------------------------------------------------
// Chunk
// ---------------------------------------------------------------------------

ExecutionResult Chunk::Call() const {
  static const ValuePack Empty;
  return Call(Empty);
}

ExecutionResult Chunk::Call(const ValuePack &Arguments) const {
  if (!Data)
    return ExecutionResult(
        ErrorDiagnostic(ErrorCategory::Runtime, "the chunk is not loaded"));
  if (!Data->R || !Data->VM)
    return ExecutionResult(ErrorDiagnostic(ErrorCategory::Runtime,
                                           "the owning state is gone"));

  detail::Runtime &R = *Data->R;
  lua_State *Co = lua_newthread(R.L);
  const int ThreadRef = luna::detail::PinTop(R.L);

  // Push the compiled function and its arguments onto the fresh coroutine.
  lua_rawgeti(Co, LUA_REGISTRYINDEX, Data->FunctionRef);

  int Pushed = 0;
  for (const OwnedValue &Value : Arguments.Values())
    Pushed += detail::PushOwnedValue(Co, Value);

  detail::Task Entry;
  Entry.Co = Co;
  Entry.ThreadRef = ThreadRef;
  Entry.Id = R.NextId++;
  Entry.Due = R.Clock;

  R.Tasks.push_back(Entry);
  const std::size_t Index = R.Tasks.size() - 1;

  R.HasRunning = true;
  R.RunningIndex = Index;
  const int Status = lua_resume(Co, R.L, Pushed);

  R.HasRunning = false;

  if (Status == 0 || Status == LUA_YIELD) {
    ReleaseTask(R, Index);
    return ExecutionResult();
  }

  const std::string Error = detail::TakeError(Co);
  ReleaseTask(R, Index);
  return ExecutionResult(
      ErrorDiagnostic(ErrorCategory::Runtime, std::move(Error)));
}

} // namespace luna

