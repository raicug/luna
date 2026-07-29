#pragma once

// clang-format off
#include <luna/binding/argument_pack.hpp>
#include <luna/binding/async_task.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/reflection/ids.hpp>

#include "state/dispatch/generation.hpp"
#include "state/transaction/lifecycle.hpp"
#include "state/type/type_generation.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class FaultInjector;

// One suspended native call. It owns every value the resumption needs and
// retains the immutable dispatch generation it was dispatched through, so it
// never refers back to the transient stack, an argument view, or a conversion
// context.
struct SuspendedCall final {
  std::uint64_t Id = 0;

  lua_State *Thread = nullptr;
  int EntryStackDepth = 0;

  DispatchSlotId Slot;
  std::string QualifiedName;
  SymbolId Symbol;
  TypeId ReceiverType;

  ArgumentPack Arguments;
  std::optional<ReturnMetadata> Awaited;

  std::shared_ptr<const TypeGeneration> Types;
  DispatchRetention Retained;
  FaultInjector *Faults = nullptr;

  StateIdentity Origin;
  std::uint64_t LifecycleGeneration = 0;

  std::unique_ptr<PendingAsyncWork> Work;

  AsyncStage Stage = AsyncStage::Pending;
  std::string Diagnostic;
  std::vector<Value> Produced;
  std::size_t Resumptions = 0;
};

// What an invocation path hands back when the callable started work it did
// not finish. It carries only owned values.
struct StartedAsyncCall final {
  std::unique_ptr<PendingAsyncWork> Work;
  ArgumentPack Arguments;
  std::optional<ReturnMetadata> Awaited;
  SymbolId Symbol;
};

struct AsyncCallCounters final {
  std::size_t Suspensions = 0;
  std::size_t Completions = 0;
  std::size_t Failures = 0;
  std::size_t Cancellations = 0;
  std::size_t Refusals = 0;
};

// Owner-thread-only registry of suspended calls. It is published to the
// virtual machine as an opaque pointer so the trampoline and its continuation
// resolve the same suspended work the pump advanced.
class AsyncCallRegistry final {
public:
  AsyncCallRegistry() = default;
  ~AsyncCallRegistry();

  AsyncCallRegistry(const AsyncCallRegistry &) = delete;
  AsyncCallRegistry &operator=(const AsyncCallRegistry &) = delete;

  void BindPumpThread(lua_State *Thread) noexcept;
  [[nodiscard]] lua_State *PumpThread() const noexcept { return PumpValue; }
  [[nodiscard]] bool PermitsSuspension(lua_State *Thread) const noexcept;

  void BindOrigin(StateIdentity Origin, std::uint64_t Generation) noexcept;

  [[nodiscard]] std::uint64_t Suspend(SuspendedCall Started);

  void RecordRefusal() noexcept { CounterValues.Refusals += 1; }

  [[nodiscard]] bool HasPendingFor(lua_State *Thread) const noexcept;
  [[nodiscard]] std::size_t PendingCount() const noexcept {
    return Pending.size();
  }

  // Advances the suspended call for one thread on the owner thread until it
  // settles. Returns the reached stage.
  [[nodiscard]] AsyncStage Advance(lua_State *Thread);

  [[nodiscard]] std::optional<SuspendedCall> Take(lua_State *Thread);

  void CancelFor(lua_State *Thread, const std::string &Reason) noexcept;
  void CancelEverything(const std::string &Reason) noexcept;

  [[nodiscard]] AsyncCallCounters Counters() const noexcept {
    return CounterValues;
  }

private:
  [[nodiscard]] SuspendedCall *FindFor(lua_State *Thread) noexcept;
  void SettleCancelled(SuspendedCall &Call, const std::string &Reason) noexcept;

  lua_State *PumpValue = nullptr;
  StateIdentity OriginValue;
  std::uint64_t LifecycleValue = 0;
  std::uint64_t NextId = 1;
  std::vector<SuspendedCall> Pending;
  AsyncCallCounters CounterValues;
};

[[nodiscard]] bool PublishAsyncRegistry(lua_State *State,
                                        AsyncCallRegistry *Registry) noexcept;

[[nodiscard]] AsyncCallRegistry *
ObserveAsyncRegistry(lua_State *State) noexcept;

} // namespace Luna::Detail
