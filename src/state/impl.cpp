// clang-format off
#include "state/impl.hpp"

#include <luna/binding/callable_descriptor.hpp>

#include "state/binding/registration_checks.hpp"
#include "state/vm/closure_installer.hpp"

#include <exception>
#include <string>
#include <utility>
// clang-format on

namespace Luna {

bool State::Impl::IsReady() const noexcept { return VirtualMachine.IsReady(); }

RegistrationResult
State::Impl::RegisterErased(std::string_view GlobalName,
                            ErasedCallableDescriptor &&Descriptor) {
  if (auto Diagnostic = Detail::CheckRegistrationPreconditions(
          GlobalName, IsReady(), Descriptor.HasTarget(), false))
    return RegistrationResult::Failure(std::move(*Diagnostic));

  if (auto Diagnostic = Detail::CheckRegistrationPreconditions(
          GlobalName, true, true, Bindings.Contains(GlobalName)))
    return RegistrationResult::Failure(std::move(*Diagnostic));

  if (Faults.Consume(Detail::StateFaultPoint::BindingRecordAllocation))
    return RegistrationResult::Failure(
        ErrorCategory::Internal,
        "Could not allocate binding record for global '" +
            std::string(GlobalName) + "'.");

  Detail::BindingRecord *Pending = nullptr;
  try {
    Pending = Bindings.Prepare(std::string(GlobalName), std::move(Descriptor),
                               Faults);
  } catch (const std::exception &) {
    return RegistrationResult::Failure(
        ErrorCategory::Internal,
        "Could not prepare binding record for global '" +
            std::string(GlobalName) + "'.");
  } catch (...) {
    return RegistrationResult::Failure(
        ErrorCategory::Internal,
        "Unknown failure while preparing binding record for global '" +
            std::string(GlobalName) + "'.");
  }

  if (!Pending)
    return RegistrationResult::Failure(
        ErrorCategory::Internal, "Binding store rejected pending global '" +
                                     std::string(GlobalName) + "'.");

  const bool InjectInstallationFailure =
      Faults.Consume(Detail::StateFaultPoint::BindingInstallation);
  const auto Installation =
      VirtualMachine.InstallBindingClosure(*Pending, InjectInstallationFailure);
  if (Installation != Detail::ClosureInstallationStatus::Success) {
    const bool RolledBack = Bindings.Rollback(GlobalName, Pending);
    const std::string Context = RolledBack
                                    ? "installation failed and was rolled back"
                                    : "internal rollback failed";
    return RegistrationResult::Failure(ErrorCategory::Internal,
                                       "Binding " + Context + " for global '" +
                                           std::string(GlobalName) + "'.");
  }

  Bindings.Commit(*Pending);
  return RegistrationResult::Success();
}

ExecutionResult State::Impl::Execute(std::string_view Source) {
  return VirtualMachine.ExecuteSource(Source, Faults);
}

} // namespace Luna
