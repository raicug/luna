#pragma once

// Macro-free selection of one overloaded C++ target.
//
// `Overload<Signature>(Target)` returns a typed Luna wrapper whose declared
// signature is exactly `Signature`. The wrapper participates in ordinary
// descriptor validation: its declared parameter and return types are the ones
// Luna canonicalizes, so a signature naming a type the registry cannot convert
// is still refused. Nothing here casts through a virtual-machine type and
// nothing here weakens a converter availability check.
//
// Three target forms are accepted. Each one is a non-deduced or constrained
// parameter, so an overloaded C++ name resolves against the declared signature
// instead of failing template argument deduction:
//
//   Bindings.RegisterFunction("Find", Overload<int(int)>(&Find));
//   Bindings.RegisterFunction("Scale", Overload<double(double)>(Scaling));
//   Overload<void(int), Actor>(&Actor::Move);
//
// A free or static function is selected by the declared signature alone. A
// member function pointer additionally names its class, and the wrapper's own
// declared signature gains the receiver as its first parameter, which is the
// shape class-scope registration consumes. Any other callable object is
// accepted when it is invocable with exactly the declared parameter types and
// yields exactly the declared return type.

// clang-format off
#include <functional>
#include <type_traits>
#include <utility>
// clang-format on

namespace Luna {

namespace Detail {

// One target selected by an explicit declared signature. The wrapper's
// `operator()` is exactly the declared signature, so the ordinary callable
// descriptor builder reads the same shape a plain function would declare.
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

// The member-pointer forms of one declared class-scope signature, plus the
// signature the wrapper declares once the receiver is explicit.
template <class Signature, class Class> struct MemberOverload;

template <class Return, class... Parameters, class Class>
struct MemberOverload<Return(Parameters...), Class> {
  using Pointer = Return (Class::*)(Parameters...);
  using ConstPointer = Return (Class::*)(Parameters...) const;
  using ReceiverSignature = Return(Class &, Parameters...);
  using ConstReceiverSignature = Return(const Class &, Parameters...);
};

// A callable object is an overload target when it is invocable with exactly the
// declared parameter types and yields exactly the declared return type. Plain
// functions and function pointers are excluded here because they have their own
// non-deduced form, which is what lets an overloaded name resolve.
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

// One consumer callable that is invocable with exactly the declared signature.
template <class Target, class Signature>
concept ExactOverloadTarget =
    Detail::IsExactOverloadTarget<std::remove_cvref_t<Target>,
                                  Signature>::value;

// One free or static function with exactly the declared signature. The
// parameter type is fully determined by `Signature`, so an overloaded C++ name
// resolves against it instead of failing deduction.
template <class Signature>
[[nodiscard]] Detail::SelectedOverload<Signature, Signature *>
Overload(Signature *Target) {
  using Wrapper = Detail::SelectedOverload<Signature, Signature *>;
  return Wrapper(Target);
}

// One member function pointer of an explicitly named class. The wrapper
// declares the receiver as its first parameter, which is the shape class-scope
// registration consumes.
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

// The same, for one const member function pointer.
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

// Any other callable object invocable with exactly the declared signature: a
// lambda, a functor, a `std::function`, or a member pointer whose declared
// signature already carries its receiver.
template <class Signature, class Callable>
  requires ExactOverloadTarget<Callable, Signature>
[[nodiscard]] Detail::SelectedOverload<Signature, std::remove_cvref_t<Callable>>
Overload(Callable &&Selected) {
  using Wrapper =
      Detail::SelectedOverload<Signature, std::remove_cvref_t<Callable>>;
  return Wrapper(std::forward<Callable>(Selected));
}

} // namespace Luna
