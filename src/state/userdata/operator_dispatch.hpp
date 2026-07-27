#pragma once

// clang-format off
#include "state/userdata/class_operators.hpp"

#include <span>
// clang-format on

struct lua_State;

namespace Luna::Detail {

[[nodiscard]] bool
InstallClassOperatorDispatch(lua_State *State, int MetatableIndex,
                             int ClassTableIndex,
                             std::span<const RegisteredOperator> Operators);

}
