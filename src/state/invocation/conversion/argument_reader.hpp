#pragma once

// Reading one Luau argument through the canonical type registry. The reader
// itself owns no conversion logic any more: it resolves the canonical type of
// the requested value kind in the type generation the invocation captured and
// runs that type's committing reader, so `bool`, signed 32-bit `int`, `double`,
// and `std::string` all convert through the registry while keeping the exact
// foundation acceptance, limits, and integer classification.

// clang-format off
#include <luna/binding/value.hpp>

#include "state/type/conversion_outcome.hpp"
#include "state/type/type_generation.hpp"

#include <cstddef>
// clang-format on

struct lua_State;

namespace Luna::Detail {

// Reads one argument against the captured type generation. An unavailable type,
// a missing stack, or an injected inspection failure is an internal failure; a
// wrong Luau representation and every value-domain rejection come from the
// type's own reader.
[[nodiscard]] ArgumentReadResult
ReadArgument(const TypeGeneration &Types, lua_State *State, int StackIndex,
             ValueKind ExpectedKind,
             bool InjectInspectionFailure = false) noexcept;

// The same read against the migrated foundation generation. It exists for
// call sites that convert outside one captured invocation.
[[nodiscard]] ArgumentReadResult
ReadArgument(lua_State *State, int StackIndex, ValueKind ExpectedKind,
             bool InjectInspectionFailure = false) noexcept;

// Public name of one foundation value kind, as the registry reports it.
[[nodiscard]] const char *ValueKindName(ValueKind Kind) noexcept;

} // namespace Luna::Detail
