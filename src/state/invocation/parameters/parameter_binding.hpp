#pragma once

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

struct BoundArguments final {
  std::vector<ArgumentSlot> Fixed;
  ArgumentPack Variadic;
  bool HasVariadic = false;
};

struct BoundInvocation final {
  InvocationValidationResult Validation;
  BoundArguments Arguments;
};

[[nodiscard]] BoundInvocation
BindDeclaredParameters(lua_State *State, std::string_view CallableName,
                       const CallableMetadata &Metadata,
                       const TypeGeneration &Types, FaultInjector &Faults,
                       int ArgumentBase = 1) noexcept;

struct RichInvocationResult final {
  int ReturnCount = -1;
  std::string Diagnostic;

  [[nodiscard]] bool IsSuccess() const noexcept { return ReturnCount >= 0; }
};

[[nodiscard]] RichInvocationResult
InvokeDeclaredParameters(lua_State *State, std::string_view CallableName,
                         ErasedCallableDescriptor &Descriptor,
                         const TypeGeneration &Types, FaultInjector &Faults,
                         const InstanceReceiver *Receiver = nullptr);

} // namespace Luna::Detail
