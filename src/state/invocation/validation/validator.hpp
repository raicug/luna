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

// Validates one invocation against the type generation the invocation captured
// at entry. Arity is rejected before any type, types are rejected in ascending
// argument position, and every diagnostic names the type by the public name the
// registry reports.
//
// `ArgumentBase` is the call position the ordinary arguments start at. It is
// one for every ordinary callable, and two for one instance member, whose
// receiver was already validated at the first position; the reported argument
// positions stay one-based over the ordinary arguments either way, so a colon
// call and a dot call with an explicit receiver report exactly the same
// diagnostic.
[[nodiscard]] ValidatedInvocation
ValidateInvocation(lua_State *State, std::string_view GlobalName,
                   const CallableMetadata *Metadata,
                   const TypeGeneration &Types, FaultInjector &Faults,
                   int ArgumentBase = 1) noexcept;

// The same validation against the migrated foundation generation.
[[nodiscard]] ValidatedInvocation
ValidateInvocation(lua_State *State, std::string_view GlobalName,
                   const CallableMetadata *Metadata,
                   FaultInjector &Faults) noexcept;

// The one place one argument read failure is worded. Every caller that converts
// one call position - the foundation's fixed arity and the richer optional,
// defaulted, and variadic shapes alike - reports it through here, so no failure
// family ever gets a second, slightly different message.
//
// `Subject` names what the position belongs to. An instance member names itself
// as a member of its class, exactly as its receiver, its getter, and its setter
// do; every other callable keeps the foundation's own wording.
void RecordArgumentReadFailure(InvocationValidationResult &Validation,
                               const ArgumentReadResult &Read,
                               const ConversionSubject &Subject,
                               std::size_t OneBasedPosition,
                               std::string_view ExpectedTypeName);

// The subject one callable's diagnostics name, derived from its own metadata: a
// declared receiver is what makes a callable an instance member.
[[nodiscard]] ConversionSubject
InvocationSubject(std::string_view GlobalName,
                  const CallableMetadata *Metadata);

} // namespace Luna::Detail
