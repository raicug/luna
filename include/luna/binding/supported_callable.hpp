#pragma once

// clang-format off
#include <luna/binding/argument_pack.hpp>
#include <luna/binding/async_task.hpp>
#include <luna/binding/delegate.hpp>
#include <luna/binding/registered_class.hpp>
#include <luna/binding/return_pack.hpp>
#include <luna/state/chunk.hpp>
#include <luna/type/stable_type_key.hpp>

#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
// clang-format on

namespace Luna {

template <class Type>
concept SupportedValue =
    std::same_as<Type, bool> || std::same_as<Type, int> ||
    std::same_as<Type, double> || std::same_as<Type, std::string>;

namespace Detail {

template <class Type> struct IsFixedReturnPack : std::false_type {};

template <class First, class Second>
struct IsFixedReturnPack<std::pair<First, Second>>
    : std::bool_constant<SupportedValue<First> && SupportedValue<Second>> {};

template <class... Elements>
struct IsFixedReturnPack<std::tuple<Elements...>>
    : std::bool_constant<(SupportedValue<Elements> && ...)> {};

template <class Type>
inline constexpr bool IsDynamicReturnPack =
    std::same_as<std::remove_cvref_t<Type>, ReturnPack>;

template <class Type>
inline constexpr bool IsOwnedValueReturn =
    std::same_as<std::remove_cvref_t<Type>, OwnedValue>;

template <class Type>
inline constexpr bool IsOwnedPackReturn =
    std::same_as<std::remove_cvref_t<Type>, ValuePack>;

template <class Type>
inline constexpr bool IsChunkReturn =
    std::same_as<std::remove_cvref_t<Type>, Luna::Chunk>;

template <class Type>
inline constexpr bool IsAsyncResult =
    std::same_as<Type, void> || SupportedValue<Type> ||
    IsDynamicReturnPack<Type>;

template <class Type> struct IsSupportedAsyncReturn : std::false_type {};

template <class Result>
struct IsSupportedAsyncReturn<AsyncTask<Result>>
    : std::bool_constant<IsAsyncResult<Result>> {};

template <class Result>
struct IsSupportedAsyncReturn<std::future<Result>>
    : std::bool_constant<IsAsyncResult<Result>> {};

} // namespace Detail

template <class Type>
concept SupportedAsyncReturn = Detail::IsSupportedAsyncReturn<Type>::value;

template <class Type>
concept SupportedReturn =
    std::same_as<Type, void> || SupportedValue<Type> ||
    Detail::IsDynamicReturnPack<Type> || Detail::IsOwnedValueReturn<Type> ||
    Detail::IsOwnedPackReturn<Type> || Detail::IsInstanceReturnType<Type> ||
    Detail::IsChunkReturn<Type> || Detail::IsFixedReturnPack<Type>::value ||
    SupportedAsyncReturn<Type>;

namespace Detail {

template <class Type> struct IsOptionalValueParameter : std::false_type {};

template <class Type>
struct IsOptionalValueParameter<std::optional<Type>>
    : std::bool_constant<SupportedValue<Type>> {};

template <class Type>
inline constexpr bool IsVariadicParameterType =
    std::same_as<std::remove_cvref_t<Type>, ArgumentView> ||
    std::same_as<std::remove_cvref_t<Type>, ArgumentPack>;

template <class Type> struct DelegateParameterSignature {
  static constexpr bool IsDeclared = false;
  using DeclaredSignature = void;
};

template <class Signature>
struct DelegateParameterSignature<Delegate<Signature>> {
  static constexpr bool IsDeclared =
      DelegateSignatureShape<Signature>::IsSupported;
  using DeclaredSignature = Signature;
};

template <class Signature>
struct DelegateParameterSignature<std::function<Signature>> {
  static constexpr bool IsDeclared =
      DelegateSignatureShape<Signature>::IsSupported;
  using DeclaredSignature = Signature;
};

template <class Type>
inline constexpr bool IsDelegateParameterType =
    DelegateParameterSignature<std::remove_cvref_t<Type>>::IsDeclared &&
    (!std::is_reference_v<Type> ||
     std::is_same_v<Type, const std::remove_cvref_t<Type> &>);

template <class Type>
using DelegateParameterSignatureOf = typename DelegateParameterSignature<
    std::remove_cvref_t<Type>>::DeclaredSignature;

template <class Type>
inline constexpr bool IsConvertedParameterType =
    (!SupportedValue<std::remove_cvref_t<Type>>) &&
    std::is_class_v<std::remove_cvref_t<Type>> &&
    (!RegisteredClassType<Type>) &&
    ConversionCapable<std::remove_cvref_t<Type>> &&
    (!std::is_reference_v<Type> ||
     std::is_same_v<Type, const std::remove_cvref_t<Type> &>);

template <class Parameter> struct InstanceParameterTrait {
  static constexpr bool IsDeclared = false;
  static constexpr bool RequiresMutation = false;
  static constexpr bool IsPointer = false;
  static constexpr bool IsCopied = false;
  using Native = void;
};

template <RegisteredClassType Type> struct InstanceParameterTrait<Type> {
  static constexpr bool IsDeclared = std::is_copy_constructible_v<Type>;
  static constexpr bool RequiresMutation = false;
  static constexpr bool IsPointer = false;
  static constexpr bool IsCopied = true;
  using Native = Type;
};

template <RegisteredClassType Type>
struct InstanceParameterTrait<const Type &> {
  static constexpr bool IsDeclared = true;
  static constexpr bool RequiresMutation = false;
  static constexpr bool IsPointer = false;
  static constexpr bool IsCopied = false;
  using Native = Type;
};

template <RegisteredClassType Type> struct InstanceParameterTrait<Type &> {
  static constexpr bool IsDeclared = true;
  static constexpr bool RequiresMutation = true;
  static constexpr bool IsPointer = false;
  static constexpr bool IsCopied = false;
  using Native = Type;
};

template <RegisteredClassType Type>
struct InstanceParameterTrait<const Type *> {
  static constexpr bool IsDeclared = true;
  static constexpr bool RequiresMutation = false;
  static constexpr bool IsPointer = true;
  static constexpr bool IsCopied = false;
  using Native = Type;
};

template <RegisteredClassType Type> struct InstanceParameterTrait<Type *> {
  static constexpr bool IsDeclared = true;
  static constexpr bool RequiresMutation = true;
  static constexpr bool IsPointer = true;
  static constexpr bool IsCopied = false;
  using Native = Type;
};

template <class Type>
inline constexpr bool IsInstanceParameterType =
    InstanceParameterTrait<Type>::IsDeclared;

template <class Type>
using InstanceParameterNative = typename InstanceParameterTrait<Type>::Native;

} // namespace Detail

template <class Type>
concept SupportedDelegate = Detail::IsDelegateParameterType<Type>;

template <class Type>
concept SupportedParameter =
    SupportedValue<Type> || Detail::IsOptionalValueParameter<Type>::value ||
    Detail::IsDelegateParameterType<Type> ||
    Detail::IsConvertedParameterType<Type> ||
    Detail::IsInstanceParameterType<Type> || std::same_as<Type, ArgumentView> ||
    std::same_as<Type, const ArgumentView &> ||
    std::same_as<Type, ArgumentPack> ||
    std::same_as<Type, const ArgumentPack &>;

namespace Detail {

template <class... Parameters> struct VariadicParameterShape;

template <> struct VariadicParameterShape<> {
  static constexpr bool IsValid = true;
};

template <class Final> struct VariadicParameterShape<Final> {
  static constexpr bool IsValid = true;
};

template <class First, class... Rest>
struct VariadicParameterShape<First, Rest...> {
  static constexpr bool IsValid = !IsVariadicParameterType<First> &&
                                  VariadicParameterShape<Rest...>::IsValid;
};

template <class Candidate> struct OptionalParameterInner {
  using Type = void;
};

template <class Inner> struct OptionalParameterInner<std::optional<Inner>> {
  using Type = Inner;
};

template <class Type>
inline constexpr bool IsRelaxedParameter =
    IsOptionalValueParameter<Type>::value || IsVariadicParameterType<Type> ||
    IsDelegateParameterType<Type> || IsConvertedParameterType<Type> ||
    IsInstanceParameterType<Type>;

template <class Signature> struct IsSupportedSignature : std::false_type {};

template <class Return, class... Parameters>
struct IsSupportedSignature<Return(Parameters...)>
    : std::bool_constant<SupportedReturn<Return> &&
                         (SupportedParameter<Parameters> && ...) &&
                         VariadicParameterShape<Parameters...>::IsValid> {};

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
