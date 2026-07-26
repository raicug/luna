#pragma once

// The virtual-machine side of dispatch indirection.
//
// An installed native closure carries exactly one payload: the permanent
// `DispatchSlotId` of its canonical callable path. The dispatch table itself is
// named once per State by one Luna-private virtual-machine slot, so an
// invocation resolves its slot without any closure ever holding a record,
// target, or metadata address.

// clang-format off
#include "state/dispatch/generation.hpp"
// clang-format on

struct lua_State;

namespace Luna::Detail {

// Names one State's dispatch table in this virtual machine. Publishing the same
// table again changes nothing.
[[nodiscard]] bool PublishDispatchTable(lua_State *State,
                                        const DispatchTable *Table) noexcept;

// The dispatch table this virtual machine resolves slots through, or null when
// no callable has ever been installed in it. Nothing is retained.
[[nodiscard]] const DispatchTable *
ObserveDispatchTable(lua_State *State) noexcept;

// Pushes the closure payload of one slot: the slot identity and nothing else.
void PushDispatchSlot(lua_State *State, DispatchSlotId Slot) noexcept;

// The slot the running native closure carries, or an invalid slot when the
// payload is absent or is not a slot identity.
[[nodiscard]] DispatchSlotId ClosureDispatchSlot(lua_State *State) noexcept;

// The slot one already pushed closure payload carries. Used to observe what an
// installed path holds without invoking it.
[[nodiscard]] DispatchSlotId DispatchSlotAt(lua_State *State,
                                            int Index) noexcept;

} // namespace Luna::Detail
