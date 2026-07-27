#pragma once

// clang-format off
#include <luna/binding/conversion.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/type/type_generation.hpp"

#include <string>
// clang-format on

struct lua_State;

namespace Luna::Detail {

struct ArgumentProbe final {
  bool IsViable = false;
  ConversionRank Rank = ConversionRank::User;

  std::string Rejection;
};

[[nodiscard]] ArgumentProbe ProbeArgument(const TypeGeneration &Types,
                                          lua_State *State, int StackIndex,
                                          const TypeDescriptor &Target);

[[nodiscard]] TypeDescriptor CanonicalReceivedType(lua_State *State,
                                                   int StackIndex);

[[nodiscard]] std::string ReceivedTypeName(lua_State *State, int StackIndex);

} // namespace Luna::Detail
