#pragma once

// clang-format off
#include <luna/core/results/execution_result.hpp>
#include <luna/core/results/registration_result.hpp>

#include <memory>
#include <string_view>
// clang-format on

namespace Luna {

class BindingRegistry;
class ErasedCallableDescriptor;

namespace Detail {
class StateTestHooks;
}

class State {
public:
  State();
  ~State();

  State(const State &) = delete;
  State &operator=(const State &) = delete;
  State(State &&) noexcept;
  State &operator=(State &&) noexcept;

  [[nodiscard]] bool IsReady() const noexcept;
  [[nodiscard]] BindingRegistry Bindings() noexcept;
  [[nodiscard]] ExecutionResult Execute(std::string_view Source);

private:
  friend class BindingRegistry;
  friend class Detail::StateTestHooks;

  [[nodiscard]] RegistrationResult
  RegisterErased(std::string_view GlobalName,
                 ErasedCallableDescriptor &&Descriptor);

  class Impl;
  std::unique_ptr<Impl> Implementation;
};

} // namespace Luna
