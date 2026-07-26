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
class ReflectionSnapshot;

namespace Detail {
class StateTestHooks;
class NamespaceBuilderState;
} // namespace Detail

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

  // The pending plan of one namespace builder chain reads the implementation
  // directly, so a builder can classify its captured handle before it stages or
  // commits anything.
  friend class Detail::NamespaceBuilderState;

  [[nodiscard]] RegistrationResult
  RegisterErased(std::string_view GlobalName,
                 ErasedCallableDescriptor &&Descriptor);

  [[nodiscard]] RegistrationResult Freeze();

  // Captures exactly one committed reflection generation. A moved-from or
  // unavailable State yields an empty snapshot instead of failing.
  [[nodiscard]] ReflectionSnapshot CaptureReflection() const;

  class Impl;
  std::unique_ptr<Impl> Implementation;
};

} // namespace Luna
