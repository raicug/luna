#pragma once

// Binding one call to a callable whose parameters are not the foundation's
// fixed arity.
//
// A required parameter must be supplied. An optional parameter maps both
// omission and an explicit nil to the empty slot. A defaulted parameter uses
// its immutable default only when it is omitted; an explicit nil is a supplied
// value and follows the parameter's own conversion. One final variadic
// parameter consumes every remaining call position.
//
// Two ordering rules matter and are enforced here rather than left to the
// adapter: every supplied argument is validated before any default is
// materialized, so a refused call materializes nothing; and the first
// deterministic variadic failure names the one-based call position and the path
// of the value that failed.

// clang-format off
#include <luna/binding/argument_pack.hpp>
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/instance_receiver.hpp>

#include "state/invocation/validation/validation_result.hpp"
#include "state/type/type_generation.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class FaultInjector;

// One call's arguments, ready for the selected native target.
struct BoundArguments final {
  std::vector<ArgumentSlot> Fixed;
  ArgumentPack Variadic;
  bool HasVariadic = false;
};

struct BoundInvocation final {
  InvocationValidationResult Validation;
  BoundArguments Arguments;
};

// Validates and converts one call against the declared parameter shape and the
// type generation the invocation captured at entry.
//
// `ArgumentBase` is the call position the ordinary arguments start at: one for
// an ordinary callable, and two for one instance member whose receiver was
// already validated at the first position. Reported positions stay one-based
// over the ordinary arguments either way.
[[nodiscard]] BoundInvocation
BindDeclaredParameters(lua_State *State, std::string_view CallableName,
                       const CallableMetadata &Metadata,
                       const TypeGeneration &Types, FaultInjector &Faults,
                       int ArgumentBase = 1) noexcept;

// Outcome of one complete rich invocation: the published return count, or one
// deterministic diagnostic.
struct RichInvocationResult final {
  int ReturnCount = -1;
  std::string Diagnostic;

  [[nodiscard]] bool IsSuccess() const noexcept { return ReturnCount >= 0; }
};

// Binds, invokes, and publishes one call whose callable declares optional,
// defaulted, or variadic parameters. The variadic tail lives in one Luna-owned
// argument frame that ends when the native invocation returns, so a retained
// `ArgumentView` becomes inert instead of reaching released storage. Exceptions
// from native code propagate to the trampoline's own translation.
// `Receiver` is the already validated object of one instance member, or null
// for every ordinary callable. Supplying one shifts the ordinary arguments past
// it and invokes the member on exactly that object.
[[nodiscard]] RichInvocationResult
InvokeDeclaredParameters(lua_State *State, std::string_view CallableName,
                         ErasedCallableDescriptor &Descriptor,
                         const TypeGeneration &Types, FaultInjector &Faults,
                         const InstanceReceiver *Receiver = nullptr);

} // namespace Luna::Detail
