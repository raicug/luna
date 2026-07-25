#pragma once

// clang-format off
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/supported_callable.hpp>

#include <functional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
// clang-format on

namespace Luna::Detail {

template <class Type> [[nodiscard]] constexpr ValueKind ValueKindFor() {
  if constexpr (std::same_as<Type, bool>)
    return ValueKind::Boolean;
  else if constexpr (std::same_as<Type, int>)
    return ValueKind::Integer;
  else if constexpr (std::same_as<Type, double>)
    return ValueKind::Number;
  else
    return ValueKind::String;
}

template <class Signature, class StoredCallable> class CallableAdapter;

template <class Return, class... Parameters, class StoredCallable>
class CallableAdapter<Return(Parameters...), StoredCallable> {
public:
  template <class Callable>
  explicit CallableAdapter(Callable &&Target)
      : TargetValue(std::forward<Callable>(Target)) {}

  [[nodiscard]] bool HasTarget() const noexcept {
    if constexpr (std::is_pointer_v<StoredCallable>)
      return TargetValue != nullptr;
    return true;
  }

  [[nodiscard]] InvocationOutcome Invoke(std::span<const Value> Arguments) {
    if (!HasTarget())
      return InvocationOutcome::InternalFailure("Callable target is null.");
    if (Arguments.size() != sizeof...(Parameters))
      return InvocationOutcome::InternalFailure(
          "Callable argument metadata is inconsistent.");
    return InvokeWithIndices(Arguments,
                             std::index_sequence_for<Parameters...>{});
  }

private:
  template <std::size_t... Indices>
  [[nodiscard]] InvocationOutcome
  InvokeWithIndices(std::span<const Value> Arguments,
                    std::index_sequence<Indices...>) {
    if constexpr (std::same_as<Return, void>) {
      std::invoke(TargetValue, std::get<Parameters>(Arguments[Indices])...);
      return InvocationOutcome::Void();
    } else {
      return InvocationOutcome::WithValue(
          Value(std::in_place_type<Return>,
                std::invoke(TargetValue,
                            std::get<Parameters>(Arguments[Indices])...)));
    }
  }

  StoredCallable TargetValue;
};

template <class Signature> struct DescriptorMetadata;

template <class Return, class... Parameters>
struct DescriptorMetadata<Return(Parameters...)> {
  [[nodiscard]] static CallableMetadata Create() {
    std::vector<ValueKind> ParameterTypes{ValueKindFor<Parameters>()...};
    if constexpr (std::same_as<Return, void>) {
      return CallableMetadata(std::move(ParameterTypes),
                              ReturnMetadata::ForVoid());
    } else {
      return CallableMetadata(std::move(ParameterTypes),
                              ReturnMetadata::ForValue(ValueKindFor<Return>()));
    }
  }
};

template <SupportedCallable Callable>
[[nodiscard]] ErasedCallableDescriptor
MakeErasedCallableDescriptor(Callable &&Target) {
  using NormalizedCallable = std::remove_cvref_t<Callable>;
  using Signature = typename CallableSignature<NormalizedCallable>::Type;
  using StoredCallable = std::decay_t<Callable>;
  using Adapter = CallableAdapter<Signature, StoredCallable>;

  return ErasedCallableDescriptor(DescriptorMetadata<Signature>::Create(),
                                  Adapter(std::forward<Callable>(Target)));
}

} // namespace Luna::Detail
