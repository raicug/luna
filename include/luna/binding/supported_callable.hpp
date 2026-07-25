#pragma once

// clang-format off
#include <concepts>
#include <string>
#include <type_traits>
// clang-format on

namespace Luna {

template <class Type>
concept SupportedValue =
    std::same_as<Type, bool> || std::same_as<Type, int> ||
    std::same_as<Type, double> || std::same_as<Type, std::string>;

namespace Detail {

template <class Signature> struct IsSupportedSignature : std::false_type {};

template <class Return, class... Parameters>
struct IsSupportedSignature<Return(Parameters...)>
    : std::bool_constant<(std::same_as<Return, void> ||
                          SupportedValue<Return>) &&
                         (SupportedValue<Parameters> && ...)> {};

template <class MemberPointer> struct MemberFunctionSignature {};

template <class Return, class Class, class... Parameters>
struct MemberFunctionSignature<Return (Class::*)(Parameters...)> {
  using Type = Return(Parameters...);
};

template <class Return, class Class, class... Parameters>
struct MemberFunctionSignature<Return (Class::*)(Parameters...) const> {
  using Type = Return(Parameters...);
};

template <class Return, class Class, class... Parameters>
struct MemberFunctionSignature<Return (Class::*)(Parameters...) noexcept> {
  using Type = Return(Parameters...);
};

template <class Return, class Class, class... Parameters>
struct MemberFunctionSignature<Return (Class::*)(Parameters...)
                                   const noexcept> {
  using Type = Return(Parameters...);
};

template <class Return, class Class, class... Parameters>
struct MemberFunctionSignature<Return (Class::*)(Parameters..., ...)> {
  using Type = Return(Parameters..., ...);
};

template <class Return, class Class, class... Parameters>
struct MemberFunctionSignature<Return (Class::*)(Parameters..., ...) const> {
  using Type = Return(Parameters..., ...);
};

template <class Return, class Class, class... Parameters>
struct MemberFunctionSignature<Return (Class::*)(Parameters..., ...) noexcept> {
  using Type = Return(Parameters..., ...);
};

template <class Return, class Class, class... Parameters>
struct MemberFunctionSignature<Return (Class::*)(Parameters..., ...)
                                   const noexcept> {
  using Type = Return(Parameters..., ...);
};

template <class Callable, class = void> struct CallableSignature {};

template <class Return, class... Parameters>
struct CallableSignature<Return(Parameters...), void> {
  using Type = Return(Parameters...);
};

template <class Return, class... Parameters>
struct CallableSignature<Return(Parameters...) noexcept, void> {
  using Type = Return(Parameters...);
};

template <class Return, class... Parameters>
struct CallableSignature<Return(Parameters..., ...), void> {
  using Type = Return(Parameters..., ...);
};

template <class Return, class... Parameters>
struct CallableSignature<Return (*)(Parameters...), void> {
  using Type = Return(Parameters...);
};

template <class Return, class... Parameters>
struct CallableSignature<Return (*)(Parameters...) noexcept, void> {
  using Type = Return(Parameters...);
};

template <class Return, class... Parameters>
struct CallableSignature<Return (*)(Parameters..., ...), void> {
  using Type = Return(Parameters..., ...);
};

template <class Callable>
struct CallableSignature<
    Callable, std::void_t<decltype(&Callable::operator()),
                          typename MemberFunctionSignature<
                              decltype(&Callable::operator())>::Type>> {
  using Type =
      typename MemberFunctionSignature<decltype(&Callable::operator())>::Type;
};

template <class Type>
inline constexpr bool IsConcreteCallableObject = std::is_class_v<Type>;

template <class Callable, class = void>
struct IsSupportedCallable : std::false_type {};

template <class Callable>
struct IsSupportedCallable<Callable,
                           std::void_t<typename CallableSignature<
                               std::remove_cvref_t<Callable>>::Type>> {
private:
  using NormalizedCallable = std::remove_cvref_t<Callable>;
  using Signature = typename CallableSignature<NormalizedCallable>::Type;

  static constexpr bool IsFreeFunction = std::is_function_v<NormalizedCallable>;
  static constexpr bool IsFunctionPointer =
      std::is_pointer_v<NormalizedCallable> &&
      std::is_function_v<std::remove_pointer_t<NormalizedCallable>>;
  static constexpr bool IsConcreteCallable =
      IsConcreteCallableObject<NormalizedCallable>;
  static constexpr bool IsStorable =
      std::is_constructible_v<std::decay_t<Callable>, Callable &&>;

public:
  static constexpr bool value =
      (IsFreeFunction || IsFunctionPointer || IsConcreteCallable) &&
      IsSupportedSignature<Signature>::value && IsStorable;
};

} // namespace Detail

template <class Callable>
struct SupportedCallableTrait
    : std::bool_constant<Detail::IsSupportedCallable<Callable>::value> {};

template <class Callable>
concept SupportedCallable = SupportedCallableTrait<Callable>::value;

} // namespace Luna
