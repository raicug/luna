#pragma once

// clang-format off
#include "state/userdata/class_operators.hpp"

#include <span>
// clang-format on

struct lua_State;

namespace Luna::Detail {

// Installs the declared operators of one class as metamethods of its metatable.
// `MetatableIndex` names the class metatable and `ClassTableIndex` names the
// Luna-owned class table every member of the class is reached through; both
// indices must be absolute.
//
// Each installed metamethod forwards exactly the operands its operator declares
// to the ordinary member candidate published under that operator's Luna-owned
// segment, so an operator resolves through the same receiver validation and the
// same overload rules as any other member. The two operators Luna's own
// reserved dispatch answers install nothing here.
[[nodiscard]] bool
InstallClassOperatorDispatch(lua_State *State, int MetatableIndex,
                             int ClassTableIndex,
                             std::span<const RegisteredOperator> Operators);

} // namespace Luna::Detail
