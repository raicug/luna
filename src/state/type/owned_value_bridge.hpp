#pragma once

// clang-format off
#include <luna/binding/conversion.hpp>

#include <cstddef>
#include <memory>
#include <string>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class TypeGeneration;

[[nodiscard]] Luna::OwnedValue BuildOwnedValueFromStack(lua_State *State,
                                                        int StackIndex);

[[nodiscard]] std::string
ClassifyPendingInstances(const Luna::OwnedValue &Source,
                         const TypeGeneration &Types);

[[nodiscard]] std::shared_ptr<const TypeGeneration>
CaptureOwnedValueTypes(lua_State *State);

[[nodiscard]] bool PushOwnedValueToStack(lua_State *State,
                                         const Luna::OwnedValue &Source);

[[nodiscard]] bool PushOwnedValueToStack(lua_State *State,
                                         const Luna::OwnedValue &Source,
                                         const TypeGeneration &Types);

} // namespace Luna::Detail
