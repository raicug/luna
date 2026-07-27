#pragma once

// clang-format off
#include <luna/binding/value.hpp>

#include "state/type/conversion_outcome.hpp"
#include "state/type/type_generation.hpp"

#include <cstddef>
// clang-format on

struct lua_State;

namespace Luna::Detail {

[[nodiscard]] ArgumentReadResult
ReadArgument(const TypeGeneration &Types, lua_State *State, int StackIndex,
             ValueKind ExpectedKind,
             bool InjectInspectionFailure = false) noexcept;

[[nodiscard]] ArgumentReadResult
ReadArgument(lua_State *State, int StackIndex, ValueKind ExpectedKind,
             bool InjectInspectionFailure = false) noexcept;

[[nodiscard]] const char *ValueKindName(ValueKind Kind) noexcept;

} // namespace Luna::Detail
