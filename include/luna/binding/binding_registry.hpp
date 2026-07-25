#pragma once

// clang-format off
#include <luna/binding/supported_callable.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/detail/callable_adapter.hpp>
#include <luna/state/state.hpp>

#include <string_view>
#include <utility>
// clang-format on

namespace Luna {

class BindingRegistry {
public:
  template <class Callable>
    requires SupportedCallable<Callable>
  [[nodiscard]] RegistrationResult Register(std::string_view GlobalName,
                                            Callable &&Target) {
    return Owner->RegisterErased(
        GlobalName,
        Detail::MakeErasedCallableDescriptor(std::forward<Callable>(Target)));
  }

private:
  friend class State;

  explicit BindingRegistry(State &Owner) noexcept : Owner(&Owner) {}

  State *Owner;
};

} // namespace Luna
