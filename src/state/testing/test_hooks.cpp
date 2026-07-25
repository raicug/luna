// clang-format off
#include "state/testing/test_hooks.hpp"

#include <luna/state/state.hpp>

#include "state/binding/record.hpp"
#include "state/invocation/conversion/argument_reader.hpp"
#include "state/impl.hpp"
#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>

#include <type_traits>
#include <utility>
#include <variant>
// clang-format on

namespace Luna::Detail {

void StateTestHooks::ResetLifecycle() noexcept {
  StateTestControl::ResetLifecycle();
}

void StateTestHooks::FailNextCreations(std::size_t Count) noexcept {
  StateTestControl::FailNextCreations(Count);
}

StateLifecycleCounters StateTestHooks::Lifecycle() noexcept {
  return StateTestControl::Counters();
}

std::optional<int>
StateTestHooks::ObserveRootStackDepth(const State &Owner) noexcept {
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return std::nullopt;
  return Owner.Implementation->VirtualMachine.StackDepth();
}

bool StateTestHooks::SetRootStackDepth(State &Owner, int Depth) noexcept {
  return Owner.Implementation && Owner.Implementation->IsReady() &&
         Owner.Implementation->VirtualMachine.SetStackDepth(Depth);
}

std::size_t StateTestHooks::BindingCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->Bindings.Count();
}

std::size_t StateTestHooks::PendingBindingCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->Bindings.PendingCount();
}

bool StateTestHooks::BindingIsCommitted(const State &Owner,
                                        std::string_view GlobalName) noexcept {
  if (!Owner.Implementation)
    return false;
  const auto *Record = Owner.Implementation->Bindings.Find(GlobalName);
  return Record && Record->IsCommitted();
}

std::optional<std::uintptr_t>
StateTestHooks::BindingRecordAddress(const State &Owner,
                                     std::string_view GlobalName) noexcept {
  if (!Owner.Implementation)
    return std::nullopt;
  const auto *Record = Owner.Implementation->Bindings.Find(GlobalName);
  if (!Record)
    return std::nullopt;
  return reinterpret_cast<std::uintptr_t>(Record);
}

std::optional<std::uintptr_t> StateTestHooks::InstalledBindingRecordAddress(
    const State &Owner, std::string_view GlobalName) noexcept {
  if (!Owner.Implementation)
    return std::nullopt;
  const auto *Record = Owner.Implementation->Bindings.Find(GlobalName);
  if (!Record)
    return std::nullopt;
  const auto *Installed =
      Owner.Implementation->VirtualMachine.ObserveInstalledBinding(
          Record->GlobalName());
  if (!Installed)
    return std::nullopt;
  return reinterpret_cast<std::uintptr_t>(Installed);
}

bool StateTestHooks::SetIntegerGlobal(State &Owner,
                                      const std::string &GlobalName,
                                      int Value) noexcept {
  return Owner.Implementation && Owner.Implementation->IsReady() &&
         Owner.Implementation->VirtualMachine.SetIntegerGlobal(GlobalName,
                                                               Value);
}

std::optional<int>
StateTestHooks::ObserveIntegerGlobal(const State &Owner,
                                     const std::string &GlobalName) noexcept {
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return std::nullopt;
  return Owner.Implementation->VirtualMachine.ObserveIntegerGlobal(GlobalName);
}

NativeInvocationObservation
StateTestHooks::InvokeBinding(State &Owner, std::string_view GlobalName,
                              const std::vector<Value> &Arguments) {
  NativeInvocationObservation Observation;
  if (!Owner.Implementation || !Owner.Implementation->IsReady()) {
    Observation.ErrorMessage = "State is not ready.";
    return Observation;
  }

  auto *Record = Owner.Implementation->Bindings.Find(GlobalName);
  lua_State *Vm = Owner.Implementation->VirtualMachine.Handle;
  if (!Record || !Record->IsCommitted() || !Vm) {
    Observation.ErrorMessage = "Binding is not available.";
    return Observation;
  }

  StackCheckpoint Checkpoint(Vm);
  Observation.EntryStackDepth = Checkpoint.EntryDepth();
  if (!lua_checkstack(Vm, static_cast<int>(Arguments.size()) + 1)) {
    Observation.ErrorMessage = "Could not reserve invocation stack.";
    Observation.FinalStackDepth = Observation.EntryStackDepth;
    return Observation;
  }

  lua_getglobal(Vm, Record->GlobalName().c_str());
  for (const auto &Argument : Arguments) {
    std::visit(
        [Vm](const auto &TypedValue) {
          using Type = std::decay_t<decltype(TypedValue)>;
          if constexpr (std::is_same_v<Type, bool>)
            lua_pushboolean(Vm, TypedValue ? 1 : 0);
          else if constexpr (std::is_same_v<Type, int>)
            lua_pushinteger(Vm, TypedValue);
          else if constexpr (std::is_same_v<Type, double>)
            lua_pushnumber(Vm, TypedValue);
          else
            lua_pushlstring(Vm, TypedValue.data(), TypedValue.size());
        },
        Argument);
  }

  const int Status =
      lua_pcall(Vm, static_cast<int>(Arguments.size()), LUA_MULTRET, 0);
  if (Status == LUA_OK) {
    Observation.Succeeded = true;
    Observation.ReturnCount = lua_gettop(Vm) - Observation.EntryStackDepth;
    const auto &Return = Record->Descriptor().Metadata().ReturnType();
    if (Observation.ReturnCount == 1 && Return.Kind()) {
      auto Read = ReadArgument(Vm, -1, *Return.Kind());
      if (Read.IsSuccess())
        Observation.ReturnedValue = std::move(*Read.ConvertedValue);
    }
  } else {
    std::size_t Length = 0;
    const char *Message = lua_tolstring(Vm, -1, &Length);
    Observation.ErrorMessage.assign(Message ? Message : "Luau error.",
                                    Message ? Length : 11);
  }

  Observation.CompletionStackDepth = lua_gettop(Vm);
  lua_settop(Vm, Observation.EntryStackDepth);
  Observation.FinalStackDepth = lua_gettop(Vm);
  return Observation;
}

std::optional<CallbackStackRestorationObservation>
StateTestHooks::ObserveLastCallbackStackRestoration(
    const State &Owner) noexcept {
  if (!Owner.Implementation)
    return std::nullopt;
  return Owner.Implementation->Faults.LastCallbackStackRestoration();
}

void StateTestHooks::InjectFault(State &Owner, StateFaultPoint Point,
                                 std::size_t Count) noexcept {
  if (Owner.Implementation)
    Owner.Implementation->Faults.Inject(Point, Count);
}

bool StateTestHooks::ConsumeFault(State &Owner,
                                  StateFaultPoint Point) noexcept {
  return Owner.Implementation && Owner.Implementation->Faults.Consume(Point);
}

std::size_t StateTestHooks::PendingFaults(const State &Owner,
                                          StateFaultPoint Point) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->Faults.Pending(Point);
}

} // namespace Luna::Detail