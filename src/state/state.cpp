// clang-format off
#include <luna/state/state.hpp>

#include <luna/binding/binding_registry.hpp>
#include <luna/binding/callable_descriptor.hpp>
#include <luna/reflection/reflection_snapshot.hpp>

#include "state/impl.hpp"
#include "state/registration/checks.hpp"

#include <memory>
#include <utility>
// clang-format on

namespace Luna {

State::State() : Implementation(std::make_unique<Impl>()) {
  Implementation->AdoptOwner(*this);
}

State::~State() = default;

State::State(State &&Other) noexcept
    : Implementation(std::move(Other.Implementation)) {
  if (Implementation) {
    Implementation->AdvanceOwnerEpoch();
    Implementation->AdoptOwner(*this);
  }
}

State &State::operator=(State &&Other) noexcept {
  if (this != &Other) {
    Implementation = std::move(Other.Implementation);
    if (Implementation) {
      Implementation->AdvanceOwnerEpoch();
      Implementation->AdoptOwner(*this);
    }
  }
  return *this;
}

bool State::IsReady() const noexcept {
  return Implementation && Implementation->IsReady();
}

BindingRegistry State::Bindings() noexcept {
  return BindingRegistry(*this);
}

RegistrationResult
State::RegisterErased(std::string_view GlobalName,
                      ErasedCallableDescriptor &&Descriptor) {
  if (auto Diagnostic = Detail::ValidateGlobalIdentifier(GlobalName))
    return RegistrationResult::Failure(std::move(*Diagnostic));

  if (!Implementation)
    return RegistrationResult::Failure(
        Detail::StateNotReadyDiagnostic(Detail::GlobalSubject(GlobalName)));

  return Implementation->RegisterErased(GlobalName, std::move(Descriptor));
}

RegistrationResult State::Freeze() {
  if (!Implementation)
    return RegistrationResult::Failure(
        ErrorCategory::StateNotReady,
        "Cannot freeze State: State is not ready.");
  return Implementation->Freeze();
}

ReflectionSnapshot State::CaptureReflection() const {
  if (!Implementation)
    return ReflectionSnapshot();
  return Implementation->CaptureReflection();
}

RegistrationResult State::InstallProfilingHook(ProfilingHook Hook) {
  if (!Implementation)
    return RegistrationResult::Failure(
        ErrorCategory::StateNotReady,
        "Cannot install profiling hook: State is not ready.");
  return Implementation->InstallProfilingHook(std::move(Hook));
}

RegistrationResult State::ClearProfilingHook() {
  if (!Implementation)
    return RegistrationResult::Failure(
        ErrorCategory::StateNotReady,
        "Cannot clear profiling hook: State is not ready.");
  return Implementation->ClearProfilingHook();
}

ExecutionResult State::Execute(std::string_view Source) {
  if (!Implementation)
    return ExecutionResult::Failure(
        ErrorCategory::StateNotReady,
        "State not ready: source execution requires a ready State.");
  return Implementation->Execute(Source);
}

} // namespace Luna
