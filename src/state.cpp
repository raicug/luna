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
constexpr const char *kChunkName = "=luna";
}

namespace detail {
struct ChunkState {
  lua_State *VM = nullptr;
  Runtime *R = nullptr;
  std::shared_ptr<Liveness> Life;
  int FunctionRef = LUA_NOREF;
  std::string Name;
  bool Compiled = false;
  ErrorDiagnostic Failure;

  ~ChunkState() {
    // The state may be gone already; then closing it freed the function.
    if (FunctionRef != LUA_NOREF && VM && Life &&
        Life->Alive.load(std::memory_order_acquire))
      luna::detail::Unpin(VM, FunctionRef);
  }
};

namespace {
// Luau calls this at every safepoint (loop back edges, calls and returns), and
// lua_error here unwinds the running coroutine. It serves both a host
// RequestInterrupt and the budget, and it is cheap enough to leave armed.
void InterruptHook(lua_State *L, int Gc) {
  // A collector step is not a place to raise an error.
  if (Gc >= 0)
    return;

  detail::Runtime *R = detail::RuntimeFromLua(L);
  if (!R)
    return;

  bool Stop = R->Stop.load(std::memory_order_acquire);
  if (!Stop && R->Steps != 0 && --R->Steps == 0) {
    R->BudgetTripped = true;
    R->RequestStop("instruction budget exhausted");
    Stop = true;
  }

  if (!Stop)
    return;

  const std::string Reason = R->StopText();
  // A safepoint runs with the frame full, so the push below needs room made.
  if (!lua_checkstack(L, 2))
    return;
  lua_pushlstring(L, Reason.data(), Reason.size());
  lua_error(L);
}

int GuardEntry(lua_State *L) {
  auto *Work = static_cast<const std::function<void()> *>(lua_touserdata(L, 1));
  try {
    (*Work)();
  } catch (const MarshalError &Thrown) {
    luaL_error(L, "%s", Thrown.what());
  }
  return 0;
}

bool CompileInto(lua_State *Target, std::string_view Source,
                 std::string_view Name, std::string &Error) {
  std::size_t BytecodeSize = 0;
  char *Bytecode =
      luau_compile(Source.data(), Source.size(), nullptr, &BytecodeSize);
  if (!Bytecode) {
    Error = "compilation failed to produce bytecode";
    return false;
  }

  const std::string Chunk =
      Name.empty() ? std::string(kChunkName) : "=" + std::string(Name);
  const int Status =
      luau_load(Target, Chunk.c_str(), Bytecode, BytecodeSize, 0);

  std::free(Bytecode);
  if (Status == 0)
    return true;

  const char *Message = lua_tostring(Target, -1);
  Error = Message ? Message : "compilation failed";
  lua_pop(Target, 1);
  return false;
}

// Luau prefixes a runtime error with "chunk:line: ", so it is split out here.
bool SplitLocation(const std::string &Text, std::string &Chunk, int &Line) {
  const std::size_t Marker = Text.find(": ");
  if (Marker == std::string::npos || Marker == 0)
    return false;

  const std::size_t Colon = Text.rfind(':', Marker - 1);
  if (Colon == std::string::npos || Colon + 1 == Marker)
    return false;

  for (std::size_t Index = Colon + 1; Index < Marker; ++Index) {
    if (Text[Index] < '0' || Text[Index] > '9')
      return false;
  }

  Chunk = Text.substr(0, Colon);
  Line = std::atoi(Text.c_str() + Colon + 1);
  return !Chunk.empty() && Line > 0;
}

ErrorDiagnostic Diagnose(ErrorCategory Category, std::string Text) {
  std::string Chunk;
  int Line = 0;
  if (!SplitLocation(Text, Chunk, Line))
    return ErrorDiagnostic(Category, std::move(Text));
  return ErrorDiagnostic(Category, std::move(Text), std::move(Chunk), Line,
                         std::string());
}

// A coroutine that failed keeps its frames, so the traceback is read from it
// here, once, and only for an error that reached the host.
ErrorDiagnostic MakeError(lua_State *L, ErrorCategory Category) {
  const char *Message = lua_tostring(L, -1);
  std::string Text = Message ? Message : "unknown error";
  lua_pop(L, 1);

  const char *Trace = lua_debugtrace(L);
  ErrorDiagnostic Parsed = Diagnose(Category, std::move(Text));
  if (!Trace || !*Trace)
    return Parsed;
  return ErrorDiagnostic(Parsed.Category(), Parsed.Message(), Parsed.Chunk(),
                         Parsed.Line(), std::string(Trace));
}
} // namespace
} // namespace detail

namespace detail {
bool RunGuarded(Runtime &R, const std::function<void()> &Work,
                std::string &Failure) {
  lua_State *L = R.L;
  const int Base = lua_gettop(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, R.GuardRef);
  lua_pushlightuserdata(L, const_cast<std::function<void()> *>(&Work));
  const int Status = lua_pcall(L, 1, 0, 0);
  if (Status == 0) {
    lua_settop(L, Base);
    return true;
  }

  const char *Message = lua_tostring(L, -1);
  Failure = Message ? Message : "unknown error";
  lua_settop(L, Base);
  return false;
}
} // namespace detail

State *StateFromVM(void *VM) noexcept {
  detail::Runtime *R = detail::RuntimeFromLua(static_cast<lua_State *>(VM));
  return R ? R->Owner : nullptr;
}

bool SuspendCurrentChunk(void *VM, double DelaySeconds) {
  (void)VM;
  detail::SuspensionRequested = true;
  detail::SuspensionDelay = DelaySeconds > 0.0 ? DelaySeconds : 0.0;
  detail::SuspensionKey = nullptr;
  detail::SuspensionElapsed = false;
  return true;
}

bool SuspendCurrentChunkUntil(void *VM, const void *Key) {
  (void)VM;
  detail::SuspensionRequested = true;
  detail::SuspensionDelay = 0.0;
  detail::SuspensionKey = Key;
  return true;
}

namespace detail {
// The task a coroutine belongs to, if the scheduler already knows it.
Task *TaskOf(Runtime &R, lua_State *L) {
  if (R.HasRunning && R.RunningIndex < R.Tasks.size()) {
    Task &Running = R.Tasks[R.RunningIndex];
    if (!Running.Dead && Running.Co == L)
      return &Running;
  }
  for (Task &Entry : R.Tasks) {
    if (!Entry.Dead && Entry.Co == L)
      return &Entry;
  }
  return nullptr;
}

// A coroutine a script made (coroutine.wrap) is not a task until it waits. From
// then on the scheduler resumes it, and whoever resumed it first sees it yield.
Task *Adopt(Runtime &R, lua_State *L) {
  lua_checkstack(L, 2);
  lua_pushthread(L);
  lua_xmove(L, R.L, 1);
  Task Entry;
  Entry.Co = L;
  Entry.ThreadRef = luna::detail::PinTop(R.L);
  Entry.Id = R.NextId++;
  Entry.Adopted = true;
  R.Tasks.push_back(Entry);
  return &R.Tasks.back();
}

bool CompleteSuspension(lua_State *L) {
  if (!SuspensionRequested)
    return false;

  const double Delay = SuspensionDelay;
  const void *Key = SuspensionKey;
  const bool Elapsed = SuspensionElapsed;
  ClearSuspensionRequest();
  Runtime *R = RuntimeFromLua(L);
  if (!R)
    return false;

  Task *Entry = TaskOf(*R, L);
  if (!Entry)
    Entry = Adopt(*R, L);

  Entry->Elapsed = Elapsed && !Key;
  Entry->Since = R->Clock;
  if (Key) {
    Entry->WaitKey = Key;
    Entry->Due = std::numeric_limits<double>::infinity();
  } else {
    Entry->WaitKey = nullptr;
    Entry->Due = R->Clock + Delay;
  }
  return true;
}
} // namespace detail

State::State() : State(StateOptions{}) {}

State::State(StateOptions Options)
    : Core(std::make_unique<detail::Runtime>(this)) {
  Core->Settings = Options;
  Core->Memory.Limit = Options.MemoryLimitBytes;
  lua_State *L = lua_newstate(&detail::LunaAllocate, &Core->Memory);
  if (!L)
    return;

  luaL_openlibs(L);
  if (Options.StripUnsafeLibs) {
    // Luau's os library is clock, time, date and difftime, so it stays.
    for (const char *Name : {"io", "package", "debug"}) {
      lua_pushnil(L);
      lua_setglobal(L, Name);
    }
  }

  Core->L = L;
  Core->World.Attach(L);
  lua_pushcfunction(L, &detail::GuardEntry, "luna_guard");
  Core->GuardRef = luna::detail::PinTop(L);
  lua_Callbacks *Callbacks = lua_callbacks(L);
  Callbacks->userdata = Core.get();
  // Without a budget the hook is installed on the first RequestInterrupt, so a
  // state that never asks to be stopped pays nothing at its safepoints.
  if (Options.InstructionBudget != 0)
    Callbacks->interrupt = &detail::InterruptHook;
}

std::size_t State::MemoryUsed() const noexcept {
  return Core ? Core->Memory.Used : 0;
}

const StateOptions &State::Options() const noexcept {
  static const StateOptions Default{};
  return Core ? Core->Settings : Default;
}

void State::Sandbox() {
  if (!IsReady())
    return;
  luaL_sandbox(Core->L);
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

bool State::IsReady() const noexcept {
  return Core && Core->L != nullptr;
}

BindingRegistry State::Bindings() {
  if (!Core)
    return BindingRegistry();
  return BindingRegistry(Core.get(), Core->Token);
}

namespace detail {
State *OwnerOf(Runtime *Core) noexcept {
  return Core ? Core->Owner : nullptr;
}
} // namespace detail

std::shared_ptr<detail::Liveness> State::Life() const noexcept {
  return Core ? Core->Token : nullptr;
}

detail::Runtime *State::GetRuntime() noexcept {
  return Core.get();
}

namespace {
int ResumeTask(detail::Runtime &R, std::size_t Index, int Arguments = 0) {
  lua_State *Co = R.Tasks[Index].Co;

  // A resume can start another one (Spawn, Execute or Chunk::Call from a native
  // callable), so the outer run's bookkeeping is saved and put back.
  const bool OuterRunning = R.HasRunning;
  const std::size_t OuterIndex = R.RunningIndex;
  const std::uint64_t OuterSteps = R.Steps;
  const bool OuterTripped = R.BudgetTripped;

  R.Steps = R.Settings.InstructionBudget;
  R.BudgetTripped = false;
  R.HasRunning = true;
  // Set per resume: a suspension is charged to whichever task is running.
  R.RunningIndex = Index;
  R.Tasks[Index].Running = true;
  // A Wake may have values for the call that parked this task.
  int Pushed = Arguments + R.Tasks[Index].StartArgs;
  R.Tasks[Index].StartArgs = 0;
  if (R.Tasks[Index].Elapsed) {
    R.Tasks[Index].Elapsed = false;
    lua_pushnumber(Co, R.Clock - R.Tasks[Index].Since);
    ++Pushed;
  }
  if (R.Tasks[Index].Resume) {
    const std::function<int(void *)> Make = std::move(R.Tasks[Index].Resume);
    R.Tasks[Index].Resume = nullptr;
    Pushed += Make(static_cast<void *>(Co));
  }
  const int Status = lua_resume(Co, R.L, Pushed);
  R.Tasks[Index].Running = false;

  const bool StoppedMidRun = R.Stop.load(std::memory_order_acquire);
  R.LastInterrupted = StoppedMidRun && Status != 0;
  if (R.BudgetTripped) {
    R.BudgetTripped = false;
    R.ClearStop();
  }

  R.HasRunning = OuterRunning;
  R.RunningIndex = OuterIndex;
  R.Steps = OuterSteps;
  R.BudgetTripped = OuterTripped;
  return Status;
}

bool PumpOnce(detail::Runtime &R, std::size_t Index, ErrorDiagnostic &Failure,
              int Arguments = 0) {
  // A task that is already inside a resume is not suspended, so it cannot be
  // resumed again from a nested pump.
  if (R.Tasks[Index].Dead || R.Tasks[Index].Running)
    return true;

  // The script may have resumed a coroutine of its own again, ahead of us.
  if (R.Tasks[Index].Adopted &&
      lua_costatus(R.L, R.Tasks[Index].Co) != LUA_COSUS)
    return false;

  // The task list can grow while the script runs, so nothing here may hold a
  // reference into it across the resume.
  lua_State *Co = R.Tasks[Index].Co;
  const int Status = ResumeTask(R, Index, Arguments);
  if (Status == 0)
    return false;

  if (Status == LUA_YIELD)
    return true;

  Failure = detail::MakeError(Co, R.LastInterrupted ? ErrorCategory::Interrupted
                                                    : ErrorCategory::Runtime);
  return false;
}

void ReleaseTask(detail::Runtime &R, std::size_t Index) {
  detail::Task &Entry = R.Tasks[Index];
  if (Entry.Dead)
    return;
  if (Entry.ThreadRef != LUA_NOREF)
    luna::detail::Unpin(R.L, Entry.ThreadRef);
  Entry.Dead = true;
  ++R.DeadTasks;
}

// Holds the scheduler's indices still: nothing may reclaim entries while a loop
// is walking them.
struct PumpScope {
  detail::Runtime &R;
  explicit PumpScope(detail::Runtime &InR) : R(InR) { ++R.PumpDepth; }
  ~PumpScope() { --R.PumpDepth; }
  PumpScope(const PumpScope &) = delete;
  PumpScope &operator=(const PumpScope &) = delete;
};

// Drops finished entries once they outnumber the live ones. Without it a host
// that only calls Execute grows the list forever, and each call walks all of
// it.
void CompactTasks(detail::Runtime &R) {
  if (R.PumpDepth != 0 || R.HasRunning || R.DeadTasks < 16 ||
      R.DeadTasks * 2 < R.Tasks.size())
    return;

  std::vector<detail::Task> Live;
  Live.reserve(R.Tasks.size() - R.DeadTasks);
  for (detail::Task &Entry : R.Tasks) {
    if (!Entry.Dead)
      Live.push_back(Entry);
  }
  R.Tasks = std::move(Live);
  R.DeadTasks = 0;
}

// Applies the wakes other threads posted, on this one.
void DrainInbox(detail::Runtime &R) {
  if (!R.InboxFull.exchange(false, std::memory_order_acq_rel))
    return;

  std::vector<std::pair<const void *, std::function<int(void *)>>> Taken;
  {
    std::lock_guard<std::mutex> Guard(R.InboxLock);
    Taken.swap(R.Inbox);
  }
  for (auto &Entry : Taken)
    R.Owner->Wake(Entry.first, std::move(Entry.second));
}

void ReportFailure(detail::Runtime &R, std::uint64_t Task,
                   ErrorDiagnostic Error) {
  if (!R.Settings.OnTaskError)
    return;
  try {
    R.Settings.OnTaskError(TaskFailure{Task, std::move(Error)});
  } catch (...) {
  }
}
} // namespace

namespace detail {
std::uint64_t StartTask(lua_State *VM, StartMode Mode, double Delay,
                        const std::function<int(lua_State *)> &Prepare) {
  Runtime *Core = RuntimeFromLua(VM);
  if (!Core || !Core->L)
    return 0;

  Runtime &R = *Core;
  CompactTasks(R);
  PumpScope Scope(R);
  lua_State *Co = nullptr;
  int ThreadRef = LUA_NOREF;
  int Count = 0;
  std::string Fault;
  if (!RunGuarded(
          R,
          [&] {
            Co = lua_newthread(R.L);
            ThreadRef = luna::detail::PinTop(R.L);
            Count = Prepare(Co);
          },
          Fault)) {
    if (ThreadRef != LUA_NOREF)
      luna::detail::Unpin(R.L, ThreadRef);
    ReportFailure(R, 0, ErrorDiagnostic(ErrorCategory::Runtime, Fault));
    return 0;
  }

  Task Entry;
  Entry.Co = Co;
  Entry.ThreadRef = ThreadRef;
  Entry.Id = R.NextId++;
  Entry.Due = R.Clock + (Mode == StartMode::After && Delay > 0.0 ? Delay : 0.0);
  Entry.StartArgs = Count;
  R.Tasks.push_back(Entry);
  const std::size_t Index = R.Tasks.size() - 1;
  const std::uint64_t Id = Entry.Id;
  if (Mode == StartMode::Now) {
    ErrorDiagnostic Failure;
    if (!PumpOnce(R, Index, Failure)) {
      if (Failure.Category() != ErrorCategory::None)
        ReportFailure(R, Id, std::move(Failure));
      ReleaseTask(R, Index);
    }
  }
  return Id;
}
} // namespace detail

ExecutionResult State::Execute(std::string_view Source) {
  if (!IsReady())
    return ExecutionResult(
        ErrorDiagnostic(ErrorCategory::Runtime, "the state is not ready"));

  detail::Runtime &R = *Core;
  if (R.Stop.load(std::memory_order_acquire)) {
    return ExecutionResult(
        ErrorDiagnostic(ErrorCategory::Interrupted, R.StopText()));
  }

  CompactTasks(R);
  PumpScope Scope(R);
  lua_State *Co = nullptr;
  int ThreadRef = LUA_NOREF;
  bool Compiled = false;
  std::string Error;
  std::string Fault;
  // Making the thread and compiling into it can run out of memory.
  if (!detail::RunGuarded(
          R,
          [&] {
            Co = lua_newthread(R.L);
            ThreadRef = luna::detail::PinTop(R.L);
            Compiled =
                detail::CompileInto(Co, Source, std::string_view(), Error);
          },
          Fault)) {
    if (ThreadRef != LUA_NOREF)
      luna::detail::Unpin(R.L, ThreadRef);
    return ExecutionResult(ErrorDiagnostic(ErrorCategory::Runtime, Fault));
  }
  if (!Compiled) {
    luna::detail::Unpin(R.L, ThreadRef);
    return ExecutionResult(
        detail::Diagnose(ErrorCategory::Compile, std::move(Error)));
  }

  detail::Task Entry;
  Entry.Co = Co;
  Entry.ThreadRef = ThreadRef;
  Entry.Id = R.NextId++;
  Entry.Due = R.Clock;
  R.Tasks.push_back(Entry);
  const std::size_t Index = R.Tasks.size() - 1;
  ErrorDiagnostic Failure;
  for (;;) {
    DrainInbox(R);
    for (std::size_t Other = 0; Other < R.Tasks.size(); ++Other) {
      if (Other == Index || R.Tasks[Other].Dead || R.Tasks[Other].Running)
        continue;
      if (R.Tasks[Other].Due > R.Clock)
        continue;
      ErrorDiagnostic Failed;
      if (!PumpOnce(R, Other, Failed)) {
        if (Failed.Category() != ErrorCategory::None)
          ReportFailure(R, R.Tasks[Other].Id, std::move(Failed));
        ReleaseTask(R, Other);
      }
    }

    const bool Alive = PumpOnce(R, Index, Failure);
    if (Failure.Category() != ErrorCategory::None) {
      ReleaseTask(R, Index);
      return ExecutionResult(std::move(Failure));
    }
    if (!Alive) {
      ReleaseTask(R, Index);
      return ExecutionResult();
    }

    if (R.Tasks[Index].WaitKey != nullptr) {
      return ExecutionResult::Pending();
    }
    if (R.Tasks[Index].Due > R.Clock)
      R.Clock = R.Tasks[Index].Due;
  }
}

Chunk State::Load(std::string_view Source, std::string_view Name) {
  if (!IsReady())
    return Chunk();

  detail::Runtime &R = *Core;
  auto State = std::make_shared<detail::ChunkState>();
  State->VM = R.L;
  State->R = &R;
  State->Life = R.Token;
  State->Name = std::string(Name);

  int ThreadRef = LUA_NOREF;
  int Function = LUA_NOREF;
  bool Compiled = false;
  std::string Error;
  std::string Fault;
  if (!detail::RunGuarded(
          R,
          [&] {
            lua_State *Co = lua_newthread(R.L);
            ThreadRef = luna::detail::PinTop(R.L);
            Compiled = detail::CompileInto(Co, Source, Name, Error);
            if (Compiled)
              Function = luna::detail::PinTop(Co);
          },
          Fault)) {
    if (ThreadRef != LUA_NOREF)
      luna::detail::Unpin(R.L, ThreadRef);
    if (Function != LUA_NOREF)
      luna::detail::Unpin(R.L, Function);
    State->Failure = ErrorDiagnostic(ErrorCategory::Runtime, Fault);
    Chunk Failed;
    Failed.Data = std::move(State);
    return Failed;
  }

  luna::detail::Unpin(R.L, ThreadRef);
  if (!Compiled) {
    // Kept so the failure is reported when the caller runs the chunk.
    State->Failure = detail::Diagnose(ErrorCategory::Compile, std::move(Error));
    Chunk Failed;
    Failed.Data = std::move(State);
    return Failed;
  }

  State->FunctionRef = Function;
  State->Compiled = true;
  Chunk Result;
  Result.Data = std::move(State);
  return Result;
}

std::uint64_t State::Spawn(std::string_view Source) {
  if (!IsReady())
    return 0;

  detail::Runtime &R = *Core;
  CompactTasks(R);
  PumpScope Scope(R);
  lua_State *Co = nullptr;
  int ThreadRef = LUA_NOREF;
  bool Compiled = false;
  std::string Error;
  std::string Fault;
  if (!detail::RunGuarded(
          R,
          [&] {
            Co = lua_newthread(R.L);
            ThreadRef = luna::detail::PinTop(R.L);
            Compiled =
                detail::CompileInto(Co, Source, std::string_view(), Error);
          },
          Fault)) {
    if (ThreadRef != LUA_NOREF)
      luna::detail::Unpin(R.L, ThreadRef);
    ReportFailure(R, 0, ErrorDiagnostic(ErrorCategory::Runtime, Fault));
    return 0;
  }
  if (!Compiled) {
    luna::detail::Unpin(R.L, ThreadRef);
    ReportFailure(R, 0,
                  detail::Diagnose(ErrorCategory::Compile, std::move(Error)));
    return 0;
  }

  detail::Task Entry;
  Entry.Co = Co;
  Entry.ThreadRef = ThreadRef;
  Entry.Id = R.NextId++;
  Entry.Due = R.Clock;
  R.Tasks.push_back(Entry);
  const std::size_t Index = R.Tasks.size() - 1;
  ErrorDiagnostic Failure;
  const std::uint64_t Id = R.Tasks[Index].Id;
  if (!PumpOnce(R, Index, Failure)) {
    if (Failure.Category() != ErrorCategory::None)
      ReportFailure(R, Id, std::move(Failure));
    ReleaseTask(R, Index);
  }

  return Id;
}

void State::Tick(double DeltaSeconds) {
  if (!IsReady())
    return;

  detail::Runtime &R = *Core;
  if (DeltaSeconds > 0.0)
    R.Clock += DeltaSeconds;

  DrainInbox(R);
  {
    PumpScope Scope(R);
    const std::size_t Count = R.Tasks.size();
    for (std::size_t Index = 0; Index < Count; ++Index) {
      if (R.Tasks[Index].Dead || R.Tasks[Index].Due > R.Clock)
        continue;

      ErrorDiagnostic Failure;
      if (!PumpOnce(R, Index, Failure)) {
        if (Failure.Category() != ErrorCategory::None)
          ReportFailure(R, R.Tasks[Index].Id, std::move(Failure));
        ReleaseTask(R, Index);
      }
    }
  }

  CompactTasks(R);
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

std::size_t State::TaskSlots() const noexcept {
  return Core ? Core->Tasks.size() : 0;
}

void State::RequestInterrupt(std::string Reason) {
  if (!Core)
    return;
  Core->RequestStop(std::move(Reason));
  // Safe from any thread, per Luau; it takes effect at the next safepoint.
  if (Core->L)
    lua_callbacks(Core->L)->interrupt = &detail::InterruptHook;
}

void State::ClearInterrupt() noexcept {
  if (!Core)
    return;
  Core->ClearStop();
}

bool State::IsInterruptPending() const noexcept {
  return Core && Core->Stop.load(std::memory_order_acquire);
}

void State::Suspend(double DelaySeconds) {
  SuspendCurrentChunk(nullptr, DelaySeconds);
}

void State::Wait(const void *Key) {
  SuspendCurrentChunkUntil(nullptr, Key);
}

void State::Wake(const void *Key) {
  Wake(Key, nullptr);
}

void State::PostWake(const void *Key, std::function<int(void *)> Values) {
  if (!Core || !Key)
    return;
  {
    std::lock_guard<std::mutex> Guard(Core->InboxLock);
    Core->Inbox.emplace_back(Key, std::move(Values));
  }
  Core->InboxFull.store(true, std::memory_order_release);
}

void State::Wake(const void *Key, std::function<int(void *)> Values) {
  if (!Core || !Key)
    return;
  for (detail::Task &Entry : Core->Tasks) {
    if (Entry.Dead || Entry.WaitKey != Key)
      continue;
    Entry.WaitKey = nullptr;
    Entry.Due = Core->Clock;
    Entry.Resume = Values;
  }
}

bool State::HasWaiters(const void *Key) const noexcept {
  if (!Core || !Key)
    return false;
  for (const detail::Task &Entry : Core->Tasks) {
    if (!Entry.Dead && Entry.WaitKey == Key)
      return true;
  }
  return false;
}

void SuspendMeasured(double DelaySeconds) {
  SuspendCurrentChunk(nullptr, DelaySeconds);
  detail::SuspensionElapsed = true;
}

void Suspend(double DelaySeconds) {
  SuspendCurrentChunk(nullptr, DelaySeconds);
}

void WaitForSignal(const void *Key) {
  SuspendCurrentChunkUntil(nullptr, Key);
}

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

const ErrorDiagnostic *Chunk::Diagnostic() const noexcept {
  return Data && !Data->Compiled ? &Data->Failure : nullptr;
}

namespace detail {
bool PushChunkFunction(lua_State *L, const Chunk &Loaded) {
  const auto &Data = Loaded.Data;
  if (!Data || !Data->Compiled || !Data->Life ||
      !Data->Life->Alive.load(std::memory_order_acquire))
    return false;
  lua_rawgeti(L, LUA_REGISTRYINDEX, Data->FunctionRef);
  return true;
}
} // namespace detail

bool Chunk::IsValid() const noexcept {
  return Data && Data->Compiled;
}

std::string_view Chunk::Name() const noexcept {
  return Data ? std::string_view(Data->Name) : std::string_view();
}

ExecutionResult Chunk::Call() const {
  static const ValuePack Empty;
  return Call(Empty);
}

ExecutionResult Chunk::Call(const ValuePack &Arguments) const {
  if (!Data)
    return ExecutionResult(
        ErrorDiagnostic(ErrorCategory::Runtime, "the chunk is not loaded"));
  if (!Data->R || !Data->VM ||
      (Data->Life && !Data->Life->Alive.load(std::memory_order_acquire)))
    return ExecutionResult(
        ErrorDiagnostic(ErrorCategory::Runtime, "the owning state is gone"));
  if (!Data->Compiled)
    return ExecutionResult(Data->Failure);

  detail::Runtime &R = *Data->R;
  if (R.Stop.load(std::memory_order_acquire))
    return ExecutionResult(
        ErrorDiagnostic(ErrorCategory::Interrupted, R.StopText()));

  CompactTasks(R);
  PumpScope Scope(R);
  lua_State *Co = nullptr;
  int ThreadRef = LUA_NOREF;
  int Pushed = 0;
  std::string Fault;
  // Making the thread and pushing the arguments can run out of memory, and an
  // argument can be an instance whose class was never registered.
  if (!detail::RunGuarded(
          R,
          [&] {
            Co = lua_newthread(R.L);
            ThreadRef = luna::detail::PinTop(R.L);
            lua_rawgeti(Co, LUA_REGISTRYINDEX, Data->FunctionRef);
            for (const OwnedValue &Value : Arguments.Values())
              Pushed += detail::PushOwnedValue(Co, Value);
          },
          Fault)) {
    if (ThreadRef != LUA_NOREF)
      luna::detail::Unpin(R.L, ThreadRef);
    return ExecutionResult(ErrorDiagnostic(ErrorCategory::Runtime, Fault));
  }

  detail::Task Entry;
  Entry.Co = Co;
  Entry.ThreadRef = ThreadRef;
  Entry.Id = R.NextId++;
  Entry.Due = R.Clock;
  R.Tasks.push_back(Entry);
  const std::size_t Index = R.Tasks.size() - 1;

  // The same path Execute and Spawn use. A chunk that yields stays in the task
  // list and carries on from Tick; only a finished or failed one is released.
  ErrorDiagnostic Failure;
  const bool Alive = PumpOnce(R, Index, Failure, Pushed);
  if (Failure.Category() != ErrorCategory::None) {
    ReleaseTask(R, Index);
    return ExecutionResult(std::move(Failure));
  }
  if (!Alive)
    ReleaseTask(R, Index);
  return ExecutionResult();
}
} // namespace luna
