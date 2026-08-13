#pragma once

// clang-format off
#include "state/dispatch/generation.hpp"
// clang-format on

struct lua_State;

namespace Luna::Detail {

class BindingRecord;

[[nodiscard]] bool PublishDispatchTable(lua_State *State,
                                        const DispatchTable *Table) noexcept;

[[nodiscard]] const DispatchTable *
ObserveDispatchTable(lua_State *State) noexcept;

void PushDispatchSlot(lua_State *State, DispatchSlotId Slot) noexcept;

[[nodiscard]] DispatchSlotId ClosureDispatchSlot(lua_State *State) noexcept;

[[nodiscard]] const DispatchTable *
ClosureDispatchTable(lua_State *State) noexcept;

[[nodiscard]] BindingRecord *ClosureBindingRecord(lua_State *State) noexcept;

[[nodiscard]] DispatchSlotId DispatchSlotAt(lua_State *State,
                                            int Index) noexcept;

} // namespace Luna::Detail
