// clang-format off
#include "state/execution/interrupt.hpp"

#include <lua.h>
#include <lualib.h>

#include <cstring>
#include <mutex>
#include <string>
// clang-format on

namespace Luna::Detail {
namespace {

void RaiseInterrupt(lua_State *State, int Collecting) {
  if (State == nullptr || Collecting >= 0)
    return;

  InterruptRequest *Pending = ObserveInterruptRequest(State);
  if (Pending == nullptr || !Pending->IsPending())
    return;

  char Composed[InterruptRequest::MaximumComposedBytes]{};
  const std::size_t Length = Pending->CopyComposed(Composed, sizeof(Composed));
  if (Length == 0)
    return;

  luaL_error(State, "%s", Composed);
}

} // namespace

void InterruptRequest::Request(std::string Reason) {
  if (Reason.empty())
    Reason.assign(DefaultInterruptReason);

  std::string Composed = std::string(InterruptPrefix) + " " + std::move(Reason);
  if (Composed.size() >= MaximumComposedBytes)
    Composed.resize(MaximumComposedBytes - 1);

  const std::lock_guard<std::mutex> Held(Guard);
  ComposedValue = std::move(Composed);
  PendingValue = true;
}

void InterruptRequest::Clear() noexcept {
  const std::lock_guard<std::mutex> Held(Guard);
  PendingValue = false;
  ComposedValue.clear();
}

bool InterruptRequest::IsPending() const noexcept {
  const std::lock_guard<std::mutex> Held(Guard);
  return PendingValue;
}

std::string InterruptRequest::Composed() const {
  const std::lock_guard<std::mutex> Held(Guard);
  return PendingValue ? ComposedValue : std::string();
}

std::size_t
InterruptRequest::CopyComposed(char *Target,
                               std::size_t Capacity) const noexcept {
  if (Target == nullptr || Capacity == 0)
    return 0;

  const std::lock_guard<std::mutex> Held(Guard);
  if (!PendingValue || ComposedValue.empty())
    return 0;

  const std::size_t Length =
      ComposedValue.size() < Capacity - 1 ? ComposedValue.size() : Capacity - 1;
  std::memcpy(Target, ComposedValue.data(), Length);
  Target[Length] = '\0';
  return Length;
}

void InstallInterruptCallback(lua_State *Root,
                              InterruptRequest *Pending) noexcept {
  if (Root == nullptr || Pending == nullptr)
    return;
  lua_Callbacks *Callbacks = lua_callbacks(Root);
  if (Callbacks == nullptr)
    return;
  Callbacks->userdata = Pending;
  Callbacks->interrupt = &RaiseInterrupt;
}

void ClearInterruptCallback(lua_State *Root) noexcept {
  if (Root == nullptr)
    return;
  lua_Callbacks *Callbacks = lua_callbacks(Root);
  if (Callbacks == nullptr)
    return;
  Callbacks->interrupt = nullptr;
  Callbacks->userdata = nullptr;
}

InterruptRequest *ObserveInterruptRequest(lua_State *State) noexcept {
  if (State == nullptr)
    return nullptr;
  lua_Callbacks *Callbacks = lua_callbacks(State);
  if (Callbacks == nullptr)
    return nullptr;
  return static_cast<InterruptRequest *>(Callbacks->userdata);
}

} // namespace Luna::Detail
