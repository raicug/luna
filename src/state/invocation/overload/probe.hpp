#pragma once

// Side-effect-free viability and rank probing of one received argument.
//
// A probe inspects the immutable shape and value domain of one Luau value
// against one canonical type of the type generation the invocation captured at
// entry. It never runs the type's committing reader, never converts, never
// constructs a Lua-owned native object, never invokes a native target, and
// never mutates virtual-machine or native state. Its whole result is
// Luna-owned: a viability answer, one rank category, and the first
// deterministic rejection reason when it is not viable.
//
// Rank follows one rule: the received Luau value has one canonical type, and a
// target that is exactly that type is an exact match. Anything else the
// registry can still read is a safe built-in conversion, unless the target type
// declares itself a registered user conversion. The canonical type of a Luau
// value is nil for none, boolean for a boolean, signed 32-bit integer for a
// finite integral number inside the 32-bit range, number for every other
// number, and string for a string.

// clang-format off
#include <luna/binding/conversion.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/type/type_generation.hpp"

#include <string>
// clang-format on

struct lua_State;

namespace Luna::Detail {

// Result of one viability and rank probe.
struct ArgumentProbe final {
  bool IsViable = false;
  ConversionRank Rank = ConversionRank::User;

  // The first deterministic reason the argument is not viable, phrased as the
  // tail of a diagnostic sentence. Empty when the probe is viable.
  std::string Rejection;
};

// Probes the value at `StackIndex` against `Target` in the captured generation.
[[nodiscard]] ArgumentProbe ProbeArgument(const TypeGeneration &Types,
                                          lua_State *State, int StackIndex,
                                          const TypeDescriptor &Target);

// The canonical type of the received Luau value, or an unsupported descriptor
// when its representation has no single canonical type.
[[nodiscard]] TypeDescriptor CanonicalReceivedType(lua_State *State,
                                                   int StackIndex);

// The Luau type name of one received value, as diagnostics report it.
[[nodiscard]] std::string ReceivedTypeName(lua_State *State, int StackIndex);

} // namespace Luna::Detail
