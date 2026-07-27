// clang-format off
#include "state/dispatch/closure_slot.hpp"

#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>

#include <cmath>
#include <cstdint>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr const char *DispatchTableSlot = "Luna.DispatchTable";

constexpr double MaximumExactSlot = 9007199254740992.0;

[[nodiscard]] DispatchSlotId DecodeSlot(lua_State *State, int Index) noexcept {
  if (lua_type(State, Index) != LUA_TNUMBER)
    return DispatchSlotId{};
  const double Encoded = lua_tonumber(State, Index);
  if (!(Encoded >= 1.0) || Encoded > MaximumExactSlot ||
      Encoded != std::floor(Encoded))
    return DispatchSlotId{};
  return DispatchSlotId{static_cast<std::uint64_t>(Encoded)};
}

} // namespace

bool PublishDispatchTable(lua_State *State,
                          const DispatchTable *Table) noexcept {
  if (!State || !Table)
    return false;
  if (!lua_checkstack(State, 4))
    return false;

  StackCheckpoint Checkpoint(State);
  lua_pushlightuserdata(State, const_cast<DispatchTable *>(Table));
  lua_rawsetfield(State, LUA_REGISTRYINDEX, DispatchTableSlot);
  return true;
}

const DispatchTable *ObserveDispatchTable(lua_State *State) noexcept {
  if (!State || !lua_checkstack(State, 2))
    return nullptr;

  StackCheckpoint Checkpoint(State);
  lua_rawgetfield(State, LUA_REGISTRYINDEX, DispatchTableSlot);
  return static_cast<const DispatchTable *>(lua_tolightuserdata(State, -1));
}

void PushDispatchSlot(lua_State *State, DispatchSlotId Slot) noexcept {
  if (!State)
    return;
  lua_pushnumber(State, static_cast<double>(Slot.Value));
}

DispatchSlotId ClosureDispatchSlot(lua_State *State) noexcept {
  if (!State)
    return DispatchSlotId{};
  return DecodeSlot(State, lua_upvalueindex(1));
}

DispatchSlotId DispatchSlotAt(lua_State *State, int Index) noexcept {
  if (!State)
    return DispatchSlotId{};
  return DecodeSlot(State, Index);
}

} // namespace Luna::Detail
