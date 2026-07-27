#pragma once

// clang-format off
#include <functional>
#include <type_traits>
#include <utility>
// clang-format on

namespace Luna {

namespace Detail {

template <class Signature, class Target> class SelectedOverload;

template <class Return, class... Parameters, class Target>
class SelectedOverload<Return(Parameters...), Target> final {
public:
  explicit SelectedOverload(Target Selected)
      : TargetValue(std::move(Selected)) {}

  Return operator()(Parameters... Arguments) {
    return std::invoke(TargetValue, std::forward<Parameters>(Arguments)...);
  }

private:
  Target TargetValue;
};

template <class Signature, class Class> struct MemberOverload;

template <class Return, class... Parameters, class Class>
struct MemberOverload<Return(Parameters...), Class> {
  using Pointer = Return (Class::*)(Parameters...);
  using ConstPointer = Return (Class::*)(Parameters...) const;
  using ReceiverSignature = Return(Class &, Parameters...);
  using ConstReceiverSignature = Return(const Class &, Parameters...);
};

template <class Target, class Signature>
struct IsExactOverloadTarget : std::false_type {};

template <class Target, class Return, class... Parameters>
struct IsExactOverloadTarget<Target, Return(Parameters...)>
    : std::bool_constant<
          !std::is_function_v<std::remove_pointer_t<Target>> &&
          (std::is_void_v<Return>
               ? std::is_invocable_v<Target &, Parameters...>
               : std::is_invocable_r_v<Return, Target &, Parameters...>)> {};

} // namespace Detail

template <class Target, class Signature>
concept ExactOverloadTarget =
    Detail::IsExactOverloadTarget<std::remove_cvref_t<Target>,
                                  Signature>::value;

template <class Signature>
[[nodiscard]] Detail::SelectedOverload<Signature, Signature *>
Overload(Signature *Target) {
  using Wrapper = Detail::SelectedOverload<Signature, Signature *>;
  return Wrapper(Target);
}

template <class Signature, class Class>
[[nodiscard]] Detail::SelectedOverload<
    typename Detail::MemberOverload<Signature, Class>::ReceiverSignature,
    typename Detail::MemberOverload<Signature, Class>::Pointer>
Overload(typename Detail::MemberOverload<Signature, Class>::Pointer Target) {
  using Wrapper = Detail::SelectedOverload<
      typename Detail::MemberOverload<Signature, Class>::ReceiverSignature,
      typename Detail::MemberOverload<Signature, Class>::Pointer>;
  return Wrapper(Target);
}

template <class Signature, class Class>
[[nodiscard]] Detail::SelectedOverload<
    typename Detail::MemberOverload<Signature, Class>::ConstReceiverSignature,
    typename Detail::MemberOverload<Signature, Class>::ConstPointer>
Overload(
    typename Detail::MemberOverload<Signature, Class>::ConstPointer Target) {
  using Wrapper = Detail::SelectedOverload<
      typename Detail::MemberOverload<Signature, Class>::ConstReceiverSignature,
      typename Detail::MemberOverload<Signature, Class>::ConstPointer>;
  return Wrapper(Target);
}

template <class Signature, class Callable>
  requires ExactOverloadTarget<Callable, Signature>
[[nodiscard]] Detail::SelectedOverload<Signature, std::remove_cvref_t<Callable>>
Overload(Callable &&Selected) {
  using Wrapper =
      Detail::SelectedOverload<Signature, std::remove_cvref_t<Callable>>;
  return Wrapper(std::forward<Callable>(Selected));
}

} // namespace Luna
