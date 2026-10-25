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
} // namespace detail

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

  // Runs the chunk until it finishes or first yields. A chunk that yields stays
  // in the task list and carries on from State::Tick; Call returns success for
  // it.
  [[nodiscard]] ExecutionResult Call() const;
  [[nodiscard]] ExecutionResult Call(const ValuePack &Arguments) const;

private:
  std::shared_ptr<detail::ChunkState> Data;
  friend class State;
};

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
  // Lets host objects (a Signal) tell if this State is still there.
  [[nodiscard]] std::shared_ptr<detail::Liveness> Life() const noexcept;
  [[nodiscard]] detail::Runtime *GetRuntime() noexcept;

private:
  friend class BindingRegistry;
  friend class Chunk;
  friend struct detail::Runtime;
  std::unique_ptr<detail::Runtime> Core;
};

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
} // namespace luna
