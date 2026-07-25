#pragma once

// clang-format off
#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/value.hpp>

#include "state/invocation/validation/validation_result.hpp"

#include <string_view>
#include <vector>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class FaultInjector;

struct ValidatedInvocation final {
  InvocationValidationResult Validation;
  std::vector<Value> Arguments;
};

[[nodiscard]] ValidatedInvocation
ValidateInvocation(lua_State *State, std::string_view GlobalName,
                   const CallableMetadata *Metadata,
                   FaultInjector &Faults) noexcept;

} // namespace Luna::Detail
