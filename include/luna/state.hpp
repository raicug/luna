#pragma once

// The VM, its scheduler and its limits.

#include <luna/bound.hpp>
#include <luna/fwd.hpp>
#include <luna/value.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace luna {
namespace detail {
class Model;
struct Runtime;
struct ChunkState;
bool PushChunkFunction(lua_State *L, const Chunk &Loaded);

bool GlobalGet(State &Owner, std::string_view Path,
               const std::function<void(lua_State *, int)> &Read,
               ErrorDiagnostic &Error);
bool GlobalSet(State &Owner, std::string_view Path,
               const std::function<int(lua_State *)> &Push,
               ErrorDiagnostic &Error);
bool GlobalCall(State &Owner, std::string_view Path,
                const std::function<int(lua_State *)> &PushArguments,
                int Results, const std::function<void(lua_State *, int)> &Read,
                ErrorDiagnostic &Error);

template <class T>
using CallArgument =
    std::conditional_t<std::is_convertible_v<const T &, std::string_view>,
                       std::string_view, std::decay_t<T>>;
} // namespace detail

template <class T> class Outcome {
public:
  explicit Outcome(T Produced) : Data(std::move(Produced)) {}
  explicit Outcome(ErrorDiagnostic Failed) : Failure(std::move(Failed)) {}

  [[nodiscard]] bool IsSuccess() const noexcept { return Data.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return IsSuccess(); }
  [[nodiscard]] const ErrorDiagnostic *Diagnostic() const noexcept {
    return Data ? nullptr : &Failure;
  }

  [[nodiscard]] const T &Value() const & noexcept { return *Data; }
  [[nodiscard]] T Value() && { return std::move(*Data); }
  template <class U> [[nodiscard]] T ValueOr(U &&Fallback) const & {
    return Data ? *Data : static_cast<T>(std::forward<U>(Fallback));
  }

private:
  std::optional<T> Data;
  ErrorDiagnostic Failure;
};

template <> class Outcome<void> {
public:
  Outcome() = default;
  explicit Outcome(ErrorDiagnostic Failed) : Failure(std::move(Failed)) {}

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Failure.Category() == ErrorCategory::None;
  }
  [[nodiscard]] explicit operator bool() const noexcept { return IsSuccess(); }
  [[nodiscard]] const ErrorDiagnostic *Diagnostic() const noexcept {
    return IsSuccess() ? nullptr : &Failure;
  }

private:
  ErrorDiagnostic Failure;
};

// A task that failed somewhere nobody is waiting for its result: a Spawn, a
// task resumed by Tick, or one Execute pumped on the way. Task is 0 when the
// source did not compile, because no task was made.
struct TaskFailure {
  std::uint64_t Task = 0;
  ErrorDiagnostic Error;
};

struct StateOptions {
  // Safepoints (loop iterations and calls) a task may use per resume; 0 is no
  // limit.
  std::uint64_t InstructionBudget = 0;
  std::size_t MemoryLimitBytes = 0;
  bool StripUnsafeLibs = false;

  // Reading a member a class does not have raises "X is not a valid member of
  // Y" instead of returning nil, so a misspelt name is caught where it is
  // written.
  bool StrictMembers = false;

  // Called on the thread that is pumping the scheduler. It may call back into
  // the State; an exception it throws is swallowed.
  std::function<void(const TaskFailure &)> OnTaskError;
};

class ReflectionSnapshot {
public:
  ReflectionSnapshot() = default;
  [[nodiscard]] bool IsValid() const noexcept { return List != nullptr; }
  [[nodiscard]] std::size_t Size() const noexcept {
    return List ? List->size() : 0;
  }
  [[nodiscard]] SymbolRange Symbols() const noexcept {
    if (!List)
      return SymbolRange();
    return SymbolRange(List, List->data(), List->size());
  }

private:
  explicit ReflectionSnapshot(std::shared_ptr<const std::vector<Symbol>> Data)
      : List(std::move(Data)) {}

  std::shared_ptr<const std::vector<Symbol>> List;
  friend class State;
};

class Chunk {
public:
  Chunk() = default;
  // False when the source did not compile; Call() then reports that failure.
  [[nodiscard]] bool IsValid() const noexcept;
  [[nodiscard]] std::string_view Name() const noexcept;
  // The compile error, or null when the chunk compiled.
  [[nodiscard]] const ErrorDiagnostic *Diagnostic() const noexcept;

  // Runs the chunk until it finishes or first yields. A chunk that yields stays
  // in the task list and carries on from State::Tick; Call returns success for
  // it.
  [[nodiscard]] ExecutionResult Call() const;
  [[nodiscard]] ExecutionResult Call(const ValuePack &Arguments) const;

private:
  std::shared_ptr<detail::ChunkState> Data;
  friend class State;
  friend bool detail::PushChunkFunction(lua_State *, const Chunk &);
};

namespace detail {
// A callable that returns a Chunk hands the script the compiled function, which
// it then calls in its own task, so `wait` works inside it.
template <> struct CustomCodec<Chunk, void> {
  static constexpr bool Enabled = true;
  static bool Read(lua_State *, int, Chunk &, ArgError &Error) {
    Error.Set("a chunk cannot be read from a script");
    return false;
  }
  static bool Accepts(lua_State *, int) { return false; }
  static int Write(lua_State *L, const Chunk &Loaded) {
    if (!PushChunkFunction(L, Loaded))
      throw MarshalError("the chunk did not compile");
    return 1;
  }
};
} // namespace detail

class State {
public:
  State();
  explicit State(StateOptions Options);
  ~State();
  State(const State &) = delete;
  State &operator=(const State &) = delete;
  State(State &&) noexcept;
  State &operator=(State &&) noexcept;
  [[nodiscard]] bool IsReady() const noexcept;
  [[nodiscard]] BindingRegistry Bindings();
  [[nodiscard]] std::size_t MemoryUsed() const noexcept;
  [[nodiscard]] const StateOptions &Options() const noexcept;
  // Locks the globals against scripts. Register everything first: a commit
  // after this is refused.
  void Sandbox();
  [[nodiscard]] ExecutionResult Execute(std::string_view Source);
  [[nodiscard]] Chunk Load(std::string_view Source, std::string_view Name = "");

  template <class T> [[nodiscard]] Outcome<T> Get(std::string_view Path);
  template <class T>
  [[nodiscard]] Outcome<void> Set(std::string_view Path, const T &Value);
  template <class R = void, class... A>
  [[nodiscard]] Outcome<R> Call(std::string_view Path, const A &...Arguments);

  // Starts source as a task and returns at its first suspension. Returns 0 when
  // it does not compile; every failure, that one included, goes to OnTaskError.
  [[nodiscard]] std::uint64_t Spawn(std::string_view Source);
  void Tick(double DeltaSeconds);
  bool Cancel(std::uint64_t Handle);
  [[nodiscard]] std::uint32_t PendingTasks() const noexcept;
  // Entries the scheduler holds, finished ones not yet reclaimed included. A
  // diagnostic: it stays small however many chunks have run.
  [[nodiscard]] std::size_t TaskSlots() const noexcept;
  // Safe from any thread. The running chunk stops at its next safepoint, and
  // the request stays pending until ClearInterrupt().
  void RequestInterrupt(std::string Reason);
  void ClearInterrupt() noexcept;
  [[nodiscard]] bool IsInterruptPending() const noexcept;
  [[nodiscard]] SymbolRange Symbols() const;
  [[nodiscard]] ReflectionSnapshot Reflection() const;

  // A .d.luau body for the registered symbols, with `any` for every type.
  [[nodiscard]] std::string Definitions() const;
  void Suspend(double DelaySeconds);
  void Wait(const void *Key);
  // Resumes the tasks parked on Key at the next Tick. Values, when given,
  // pushes what each one resumes with (what the call that parked it returns).
  void Wake(const void *Key);
  void Wake(const void *Key, std::function<int(void *)> Values);
  [[nodiscard]] bool HasWaiters(const void *Key) const noexcept;
  // Like Wake, and safe from any thread. It takes effect on the state's thread,
  // at its next Tick, so work finished elsewhere can resume a parked task.
  void PostWake(const void *Key, std::function<int(void *)> Values = nullptr);
  // Lets host objects (a Signal) tell if this State is still there.
  [[nodiscard]] std::shared_ptr<detail::Liveness> Life() const noexcept;
  [[nodiscard]] detail::Runtime *GetRuntime() noexcept;

private:
  friend class BindingRegistry;
  friend class Chunk;
  friend struct detail::Runtime;
  std::unique_ptr<detail::Runtime> Core;
};

template <class T> Outcome<T> State::Get(std::string_view Path) {
  static_assert(!std::is_reference_v<T>, "luna: Get returns a value");
  std::optional<T> Found;
  ErrorDiagnostic Error;
  const bool Ok = detail::GlobalGet(
      *this, Path,
      [&Found](lua_State *L, int Index) {
        detail::SlotType<T> Slot{};
        detail::ArgError Problem;
        if (!detail::ReadValue<T>(L, Index, Slot,
                                  detail::ElementClassId<T>(L), Problem))
          throw detail::MarshalError(Problem.Text);
        Found.emplace(std::move(Slot));
      },
      Error);
  if (!Ok)
    return Outcome<T>(std::move(Error));
  return Outcome<T>(std::move(*Found));
}

template <class T>
Outcome<void> State::Set(std::string_view Path, const T &Value) {
  ErrorDiagnostic Error;
  const bool Ok = detail::GlobalSet(
      *this, Path,
      [&Value](lua_State *L) {
        if (detail::WriteValue(L, Value, NoType) != 1)
          throw detail::MarshalError("the value does not become one value");
        return 1;
      },
      Error);
  return Ok ? Outcome<void>() : Outcome<void>(std::move(Error));
}

template <class R, class... A>
Outcome<R> State::Call(std::string_view Path, const A &...Arguments) {
  static_assert(!std::is_reference_v<R>, "luna: Call returns a value");
  std::optional<std::conditional_t<std::is_void_v<R>, int, R>> Found;
  ErrorDiagnostic Error;
  const bool Ok = detail::GlobalCall(
      *this, Path,
      [&Arguments...](lua_State *L) {
        int Pushed = 0;
        ((Pushed += detail::WriteValue(
              L, static_cast<detail::CallArgument<A>>(Arguments), NoType)),
         ...);
        return Pushed;
      },
      std::is_void_v<R> ? 0 : 1,
      [&Found](lua_State *L, int Index) {
        if constexpr (!std::is_void_v<R>) {
          detail::SlotType<R> Slot{};
          detail::ArgError Problem;
          if (!detail::ReadValue<R>(L, Index, Slot,
                                    detail::ElementClassId<R>(L), Problem))
            throw detail::MarshalError("the return value: " + Problem.Text);
          Found.emplace(std::move(Slot));
        }
      },
      Error);
  if constexpr (std::is_void_v<R>)
    return Ok ? Outcome<void>() : Outcome<void>(std::move(Error));
  else
    return Ok ? Outcome<R>(std::move(*Found)) : Outcome<R>(std::move(Error));
}

State *StateFromVM(void *VM) noexcept;

namespace detail {
// The State a runtime belongs to; follows the State through a move.
State *OwnerOf(Runtime *Core) noexcept;
} // namespace detail

// Emits a .d.luau body for a snapshot: one declaration per name and per group.
std::string GenerateDefinitions(const ReflectionSnapshot &Snapshot);

bool SuspendCurrentChunk(void *VM, double DelaySeconds);

bool SuspendCurrentChunkUntil(void *VM, const void *Key);

void Suspend(double DelaySeconds = 0.0);

// Like Suspend, but the call returns the seconds that actually passed.
void SuspendMeasured(double DelaySeconds);

// Registers task.spawn, task.defer, task.delay, task.cancel and task.wait. With
// GlobalAliases it also registers wait, spawn and delay.
RegistrationResult RegisterTaskLibrary(BindingRegistry Registry,
                                       bool GlobalAliases = true);
} // namespace luna
