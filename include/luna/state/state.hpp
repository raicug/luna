#pragma once

// clang-format off
#include <luna/core/results/execution_result.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/state/chunk.hpp>
#include <luna/tooling/profiling_hook.hpp>

#include <memory>
#include <string>
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
  [[nodiscard]] ExecutionResult Execute(std::string_view Source,
                                        const ExecutionPolicy &Policy);

  [[nodiscard]] Chunk Load(std::string_view Source);
  [[nodiscard]] Chunk Load(std::string_view Source,
                           const ExecutionPolicy &Policy);
  [[nodiscard]] Chunk Load(std::string_view Source, std::string_view Name);
  [[nodiscard]] Chunk Load(std::string_view Source, std::string_view Name,
                           const ExecutionPolicy &Policy);

  void RequestInterrupt(std::string Reason);
  void ClearInterrupt() noexcept;
  [[nodiscard]] bool IsInterruptPending() const noexcept;

private:
  friend class BindingRegistry;
  friend class Detail::StateTestHooks;

  friend class Detail::NamespaceBuilderState;

  [[nodiscard]] RegistrationResult
  RegisterErased(std::string_view GlobalName,
                 ErasedCallableDescriptor &&Descriptor);

  [[nodiscard]] RegistrationResult Freeze();

  [[nodiscard]] ReflectionSnapshot CaptureReflection() const;

  [[nodiscard]] RegistrationResult InstallProfilingHook(ProfilingHook Hook);
  [[nodiscard]] RegistrationResult ClearProfilingHook();

  class Impl;
  std::unique_ptr<Impl> Implementation;
};

} // namespace Luna
