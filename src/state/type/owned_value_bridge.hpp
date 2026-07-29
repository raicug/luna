#pragma once

// clang-format off
#include <luna/binding/conversion.hpp>

#include <cstddef>
// clang-format on

struct lua_State;

namespace Luna::Detail {

[[nodiscard]] Luna::OwnedValue BuildOwnedValueFromStack(lua_State *State,
                                                        int StackIndex);

[[nodiscard]] bool PushOwnedValueToStack(lua_State *State,
                                         const Luna::OwnedValue &Source);

} // namespace Luna::Detail
