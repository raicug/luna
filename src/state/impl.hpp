#pragma once

// clang-format off
#include <luna/state/state.hpp>

#include "state/binding/store.hpp"
#include "state/testing/fault_injector.hpp"
#include "state/vm/owner.hpp"
// clang-format on

namespace Luna {

class State::Impl final {
public:
  Impl() noexcept = default;
  ~Impl() = default;

  [[nodiscard]] bool IsReady() const noexcept;
  [[nodiscard]] RegistrationResult
  RegisterErased(std::string_view GlobalName,
                 ErasedCallableDescriptor &&Descriptor);
  [[nodiscard]] ExecutionResult Execute(std::string_view Source);

private:
  friend class Detail::StateTestHooks;

  // Destruction is reverse declaration order: the VM closes before records die.
  Detail::BindingStore Bindings;
  Detail::FaultInjector Faults;
  Detail::VirtualMachineOwner VirtualMachine;
};

} // namespace Luna
