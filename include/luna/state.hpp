#pragma once

// luna : the VM owner.
//
// A State owns one Luau main thread and one committed model. Chunks run inside
// coroutines rather than on a disposable OS thread, which is what lets a native
// binding suspend instead of sleeping and pumping by hand.

#include <luna/bound.hpp>
#include <luna/fwd.hpp>
#include <luna/value.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace luna {

namespace detail {
class Model;
struct Runtime;
struct ChunkState;
} // namespace detail

// Immutable symbol list as of one commit. Owns its strings, so a snapshot stays
// readable after further registration and after the State is gone.
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

// A compiled chunk, re-invocable without recompiling.
class Chunk {
public:
  Chunk() = default;

  [[nodiscard]] bool IsValid() const noexcept { return Data != nullptr; }

  [[nodiscard]] ExecutionResult Call() const;
  [[nodiscard]] ExecutionResult Call(const ValuePack &Arguments) const;

private:
  std::shared_ptr<detail::ChunkState> Data;
  friend class State;
};

class State {
public:
  State();
  ~State();

  State(const State &) = delete;
  State &operator=(const State &) = delete;
  State(State &&) noexcept;
  State &operator=(State &&) noexcept;

  [[nodiscard]] bool IsReady() const noexcept;

  [[nodiscard]] BindingRegistry Bindings();

  // --- execution ---------------------------------------------------------

  // Runs to completion. A suspension defers the rest of the chunk until the
  // scheduler reaches it, and Execute advances the scheduler itself, so a
  // script that waits still finishes.
  [[nodiscard]] ExecutionResult Execute(std::string_view Source);

  [[nodiscard]] Chunk Load(std::string_view Source,
                           std::string_view Name = "");

  // Starts source as an independent task and returns at its first suspension.
  // The handle can be passed to Cancel().
  [[nodiscard]] std::uint64_t Spawn(std::string_view Source);

  // Advances virtual time and resumes every task that is due. Call once per
  // frame from the host loop.
  void Tick(double DeltaSeconds);

  bool Cancel(std::uint64_t Handle);

  [[nodiscard]] std::uint32_t PendingTasks() const noexcept;

  // --- interruption ------------------------------------------------------

  // Safe from any thread. The running chunk stops at its next safepoint.
  void RequestInterrupt(std::string Reason);
  void ClearInterrupt() noexcept;
  [[nodiscard]] bool IsInterruptPending() const noexcept;

  // --- reflection --------------------------------------------------------

  [[nodiscard]] SymbolRange Symbols() const;
  [[nodiscard]] ReflectionSnapshot Reflection() const;

  // --- suspension --------------------------------------------------------

  // A native callable calls Suspend(Delay) then returns lua_yield(L, 0). The
  // scheduler resumes it once Delay virtual seconds have passed.
  void Suspend(double DelaySeconds);

  [[nodiscard]] detail::Runtime *GetRuntime() noexcept;

private:
  friend class BindingRegistry;
  friend class Chunk;
  friend struct detail::Runtime;

  std::unique_ptr<detail::Runtime> Core;
};

// The State that owns a running VM, resolved from inside a native callable.
State *StateFromVM(void *VM) noexcept;

// Suspends the chunk that is currently running, if a Chunk-bound callable
// requested it. Returns true when a suspension was recorded.
bool SuspendCurrentChunk(void *VM, double DelaySeconds);

// The form a native callable uses: suspend the running chunk for DelaySeconds
// of virtual time, then return. The trampoline turns it into a yield, so no
// thread is parked while the script waits.
void Suspend(double DelaySeconds = 0.0);

} // namespace luna
