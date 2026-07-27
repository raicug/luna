#pragma once

// clang-format off
#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/value.hpp>

#include "state/invocation/validation/validation_result.hpp"
#include "state/type/conversion_outcome.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/type_generation.hpp"

#include <cstddef>
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
                   const TypeGeneration &Types, FaultInjector &Faults,
                   int ArgumentBase = 1) noexcept;

[[nodiscard]] ValidatedInvocation
ValidateInvocation(lua_State *State, std::string_view GlobalName,
                   const CallableMetadata *Metadata,
                   FaultInjector &Faults) noexcept;

void RecordArgumentReadFailure(InvocationValidationResult &Validation,
                               const ArgumentReadResult &Read,
                               const ConversionSubject &Subject,
                               std::size_t OneBasedPosition,
                               std::string_view ExpectedTypeName);

[[nodiscard]] ConversionSubject
InvocationSubject(std::string_view GlobalName,
                  const CallableMetadata *Metadata);

} // namespace Luna::Detail
