// clang-format off
#include <luna/state/state.hpp>

#include <luna/binding/binding_registry.hpp>
#include <luna/binding/callable_descriptor.hpp>

#include "state/binding/registration_checks.hpp"
#include "state/impl.hpp"

#include <memory>
#include <utility>
// clang-format on

namespace Luna {

State::State() : Implementation(std::make_unique<Impl>()) {}
State::~State() = default;

State::State(State &&Other) noexcept
    : Implementation(std::move(Other.Implementation)) {}

State &State::operator=(State &&Other) noexcept {
  if (this != &Other)
    Implementation = std::move(Other.Implementation);
  return *this;
}

bool State::IsReady() const noexcept {
  return Implementation && Implementation->IsReady();
}

BindingRegistry State::Bindings() noexcept { return BindingRegistry(*this); }

RegistrationResult
State::RegisterErased(std::string_view GlobalName,
                      ErasedCallableDescriptor &&Descriptor) {
  if (auto Diagnostic = Detail::CheckRegistrationPreconditions(
          GlobalName, IsReady(), Descriptor.HasTarget(), false))
    return RegistrationResult::Failure(std::move(*Diagnostic));

  return Implementation->RegisterErased(GlobalName, std::move(Descriptor));
}

ExecutionResult State::Execute(std::string_view Source) {
  if (!IsReady())
    return ExecutionResult::Failure(
        ErrorCategory::StateNotReady,
        "State not ready: source execution requires a ready State.");
  return Implementation->Execute(Source);
}

} // namespace Luna
