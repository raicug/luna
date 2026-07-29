// clang-format off
#include "state/invocation/async/suspended_call.hpp"

#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>

#include <string>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr const char *AsyncRegistrySlot = "Luna.AsyncCalls";

} // namespace

AsyncCallRegistry::~AsyncCallRegistry() {
  CancelEverything("the State that suspended it is gone");
}

void AsyncCallRegistry::BindPumpThread(lua_State *Thread) noexcept {
  PumpValue = Thread;
}

bool AsyncCallRegistry::PermitsSuspension(lua_State *Thread) const noexcept {
  return Thread != nullptr && Thread == PumpValue;
}

void AsyncCallRegistry::BindOrigin(StateIdentity Origin,
                                   std::uint64_t Generation) noexcept {
  OriginValue = Origin;
  LifecycleValue = Generation;
}

std::uint64_t AsyncCallRegistry::Suspend(SuspendedCall Started) {
  Started.Id = NextId;
  Started.Origin = OriginValue;
  Started.LifecycleGeneration = LifecycleValue;
  NextId += 1;
  CounterValues.Suspensions += 1;
  Pending.push_back(std::move(Started));
  return Pending.back().Id;
}

bool AsyncCallRegistry::HasPendingFor(lua_State *Thread) const noexcept {
  for (const SuspendedCall &Call : Pending) {
    if (Call.Thread == Thread)
      return true;
  }
  return false;
}

SuspendedCall *AsyncCallRegistry::FindFor(lua_State *Thread) noexcept {
  for (SuspendedCall &Call : Pending) {
    if (Call.Thread == Thread)
      return &Call;
  }
  return nullptr;
}

void AsyncCallRegistry::SettleCancelled(SuspendedCall &Call,
                                        const std::string &Reason) noexcept {
  if (Call.Stage != AsyncStage::Pending)
    return;
  try {
    if (Call.Work) {
      Call.Work->RequestCancellation();
      static_cast<void>(Call.Work->Cancel(Reason));
    }
    Call.Diagnostic = Reason;
  } catch (...) {
    Call.Diagnostic.clear();
  }
  Call.Stage = AsyncStage::Cancelled;
  CounterValues.Cancellations += 1;
}

AsyncStage AsyncCallRegistry::Advance(lua_State *Thread) {
  SuspendedCall *Call = FindFor(Thread);
  if (!Call)
    return AsyncStage::Failed;
  if (Call->Stage != AsyncStage::Pending)
    return Call->Stage;
  if (!Call->Work) {
    Call->Stage = AsyncStage::Failed;
    Call->Diagnostic = "the suspended call retained no asynchronous work";
    CounterValues.Failures += 1;
    return Call->Stage;
  }

  Call->Resumptions += 1;

  AsyncStage Reached = AsyncStage::Pending;
  try {
    Reached = Call->Work->Poll();
    if (Reached == AsyncStage::Pending)
      Reached = Call->Work->Await();
  } catch (...) {
    Reached = AsyncStage::Failed;
  }

  switch (Reached) {
  case AsyncStage::Ready:
    try {
      Call->Produced = Call->Work->TakeValues();
    } catch (...) {
      Call->Stage = AsyncStage::Failed;
      Call->Diagnostic = "the completed values could not be retained";
      CounterValues.Failures += 1;
      return Call->Stage;
    }
    Call->Stage = AsyncStage::Ready;
    CounterValues.Completions += 1;
    return Call->Stage;

  case AsyncStage::Cancelled:
    try {
      Call->Diagnostic = Call->Work->Message();
    } catch (...) {
      Call->Diagnostic.clear();
    }
    if (Call->Diagnostic.empty())
      Call->Diagnostic = "the asynchronous work was cancelled";
    Call->Stage = AsyncStage::Cancelled;
    CounterValues.Cancellations += 1;
    return Call->Stage;

  case AsyncStage::Pending:
    // Cancellation was requested while the host still owed a result, so the
    // suspended call settles deterministically instead of waiting forever.
    SettleCancelled(*Call, "the asynchronous work stopped without a result");
    return Call->Stage;

  case AsyncStage::Failed:
    break;
  }

  try {
    Call->Diagnostic = Call->Work->Message();
  } catch (...) {
    Call->Diagnostic.clear();
  }
  if (Call->Diagnostic.empty())
    Call->Diagnostic = "the asynchronous work failed without a reason";
  Call->Stage = AsyncStage::Failed;
  CounterValues.Failures += 1;
  return Call->Stage;
}

std::optional<SuspendedCall> AsyncCallRegistry::Take(lua_State *Thread) {
  for (std::size_t Index = 0; Index < Pending.size(); ++Index) {
    if (Pending[Index].Thread != Thread)
      continue;
    SuspendedCall Taken = std::move(Pending[Index]);
    Pending.erase(Pending.begin() + static_cast<std::ptrdiff_t>(Index));
    return Taken;
  }
  return std::nullopt;
}

void AsyncCallRegistry::CancelFor(lua_State *Thread,
                                  const std::string &Reason) noexcept {
  for (std::size_t Index = Pending.size(); Index > 0; --Index) {
    SuspendedCall &Call = Pending[Index - 1];
    if (Call.Thread != Thread)
      continue;
    SettleCancelled(Call, Reason);
    Pending.erase(Pending.begin() + static_cast<std::ptrdiff_t>(Index - 1));
  }
}

void AsyncCallRegistry::CancelEverything(const std::string &Reason) noexcept {
  for (std::size_t Index = Pending.size(); Index > 0; --Index)
    SettleCancelled(Pending[Index - 1], Reason);
  Pending.clear();
  PumpValue = nullptr;
}

bool PublishAsyncRegistry(lua_State *State,
                          AsyncCallRegistry *Registry) noexcept {
  if (!State || !Registry)
    return false;
  if (!lua_checkstack(State, 4))
    return false;

  StackCheckpoint Checkpoint(State);
  lua_pushlightuserdata(State, Registry);
  lua_rawsetfield(State, LUA_REGISTRYINDEX, AsyncRegistrySlot);
  return true;
}

AsyncCallRegistry *ObserveAsyncRegistry(lua_State *State) noexcept {
  if (!State || !lua_checkstack(State, 2))
    return nullptr;

  StackCheckpoint Checkpoint(State);
  lua_rawgetfield(State, LUA_REGISTRYINDEX, AsyncRegistrySlot);
  return static_cast<AsyncCallRegistry *>(lua_tolightuserdata(State, -1));
}

} // namespace Luna::Detail
