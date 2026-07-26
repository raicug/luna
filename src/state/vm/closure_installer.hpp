#pragma once

// Protected installation of one callable closure at the exact canonical path of
// its binding record.
//
// A root-scope global keeps exactly the installation and self-rollback behavior
// the foundation established. A nested canonical path installs into the
// namespace table that path's own scope declaration published: the parent table
// is never created here, and the transaction journal - which captured the exact
// prior value of the path before installation - owns restoration.

// The installed closure itself carries only the permanent dispatch slot of its
// path. The record it dispatches to is whatever the current dispatch generation
// resolves that slot to, so no closure ever holds a record address.

// clang-format off
#include "state/dispatch/generation.hpp"

#include <string>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class BindingRecord;

enum class ClosureInstallationStatus {
  Success,
  StackCapacityFailure,
  ProtectedFailure,
  RollbackFailure
};

[[nodiscard]] ClosureInstallationStatus
InstallBindingClosure(lua_State *State, BindingRecord &Record,
                      bool InjectFailure) noexcept;

// The dispatch slot the given canonical path's closure carries, or an invalid
// slot when the path holds anything else.
[[nodiscard]] DispatchSlotId
ObserveInstalledDispatchSlot(lua_State *State,
                             const std::string &GlobalName) noexcept;

// The binding record the given canonical path currently dispatches to, or null
// when the path holds anything else or its slot is unavailable. Nothing is
// retained and nothing is mutated.
[[nodiscard]] const BindingRecord *
ObserveInstalledBinding(lua_State *State,
                        const std::string &GlobalName) noexcept;

} // namespace Luna::Detail
