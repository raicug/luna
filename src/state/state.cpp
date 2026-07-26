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
  // The shared handle token records which owner object holds the
  // implementation, so a builder can detect a move or a destruction instead of
  // dereferencing a pointer that no longer names this State.
  Implementation->AdoptOwner(*this);
}

State::~State() = default;

// A move transfers the implementation, so the logical State identity is
// preserved while the owner-object epoch advances: handles captured against the
// previous owner object are stale from here on.
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

BindingRegistry State::Bindings() noexcept { return BindingRegistry(*this); }

RegistrationResult
State::RegisterErased(std::string_view GlobalName,
                      ErasedCallableDescriptor &&Descriptor) {
  // An invalid identifier is rejected before anything else, exactly as the
  // foundation established. Every later rejection is decided by the one
  // transaction, so the documented precedence lives in one place.
  if (auto Diagnostic = Detail::ValidateGlobalIdentifier(GlobalName))
    return RegistrationResult::Failure(std::move(*Diagnostic));

  // A moved-from State owns no implementation, so it has no owner thread and no
  // lifecycle to capture.
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

ExecutionResult State::Execute(std::string_view Source) {
  // The implementation owns both affinity and readiness checks. In particular,
  // it checks affinity first so a foreign caller never reaches VM readiness or
  // stack state; a moved-from State still has no implementation to ask.
  if (!Implementation)
    return ExecutionResult::Failure(
        ErrorCategory::StateNotReady,
        "State not ready: source execution requires a ready State.");
  return Implementation->Execute(Source);
}

} // namespace Luna
