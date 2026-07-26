#pragma once

// clang-format off
#include <luna/binding/argument_pack.hpp>
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/class_construction.hpp>
#include <luna/binding/parameter_descriptor.hpp>
#include <luna/binding/return_pack.hpp>
#include <luna/binding/supported_callable.hpp>
#include <luna/type/stable_type_key.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
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

// The declared element types of one returned pair or tuple, in return order.
template <class Pack> struct FixedReturnPackKinds;

template <class First, class Second>
struct FixedReturnPackKinds<std::pair<First, Second>> {
  [[nodiscard]] static std::vector<ValueKind> Kinds() {
    return {ValueKindFor<First>(), ValueKindFor<Second>()};
  }
};

template <class... Elements>
struct FixedReturnPackKinds<std::tuple<Elements...>> {
  [[nodiscard]] static std::vector<ValueKind> Kinds() {
    return {ValueKindFor<Elements>()...};
  }
};

// One returned pair or tuple staged as ordered values. Nothing is published
// here: the pack is complete native storage the return writer validates before
// it exposes anything.
template <class Pack>
[[nodiscard]] std::vector<Value> StageReturnPack(Pack &&Source) {
  std::vector<Value> Staged;
  std::apply(
      [&Staged](auto &&...Elements) {
        (Staged.push_back(
             Value(std::in_place_type<std::remove_cvref_t<decltype(Elements)>>,
                   std::forward<decltype(Elements)>(Elements))),
         ...);
      },
      std::forward<Pack>(Source));
  return Staged;
}

// One invocation's outcome, described by the declared return shape: `void`
// produces zero values, a supported scalar one, and a pair, tuple, or dynamic
// return pack its ordered elements.
template <class Return, class Invoker>
[[nodiscard]] InvocationOutcome CaptureReturn(Invoker &&Invoke) {
  if constexpr (std::same_as<Return, void>) {
    Invoke();
    return InvocationOutcome::Void();
  } else if constexpr (std::same_as<Return, ConstructedInstance>) {
    // A construction candidate produces one native object plus its ownership
    // statement. Nothing is published here: the object is staged until the
    // whole publication succeeds.
    return InvocationOutcome::WithInstance(Invoke());
  } else if constexpr (IsDynamicReturnPack<Return>) {
    const ReturnPack Produced = Invoke();
    const std::span<const Value> Elements = Produced.Values();
    return InvocationOutcome::WithValues(
        std::vector<Value>(Elements.begin(), Elements.end()));
  } else if constexpr (IsFixedReturnPack<Return>::value) {
    return InvocationOutcome::WithValues(StageReturnPack(Invoke()));
  } else {
    return InvocationOutcome::WithValue(
        Value(std::in_place_type<Return>, Invoke()));
  }
}

// The callable declares at least one optional, defaulted, or variadic
// parameter, so its shape is described by parameter descriptors rather than by
// one value kind per position.
template <class... Parameters>
inline constexpr bool HasRelaxedParameterShape =
    (false || ... || IsRelaxedParameter<Parameters>);

// Number of parameters that consume one call position each. A variadic tail
// consumes the rest and owns no fixed position.
template <class... Parameters>
inline constexpr std::size_t FixedParameterCountOf =
    (std::size_t{0} + ... +
     (IsVariadicParameterType<Parameters> ? std::size_t{0} : std::size_t{1}));

// The default declared for the fixed parameter at `Position`, if any. Defaults
// are declared for the trailing fixed parameters, in declared order.
[[nodiscard]] inline const Value *
DeclaredDefaultAt(std::span<const Value> Defaults, std::size_t Position,
                  std::size_t FixedCount) {
  if (Defaults.empty() || Position >= FixedCount)
    return nullptr;
  if (Defaults.size() > FixedCount)
    return nullptr;
  const std::size_t FirstDefaulted = FixedCount - Defaults.size();
  if (Position < FirstDefaulted)
    return nullptr;
  return &Defaults[Position - FirstDefaulted];
}

// One parameter's immutable descriptor. A declared default turns an otherwise
// required or optional parameter into a defaulted one, and an optional
// parameter keeps accepting an explicit nil either way.
template <class Parameter>
[[nodiscard]] ParameterDescriptor
MakeParameterDescriptor(const Value *Default) {
  if constexpr (IsVariadicParameterType<Parameter>) {
    static_cast<void>(Default);
    constexpr bool Retains =
        std::same_as<std::remove_cvref_t<Parameter>, ArgumentPack>;
    return ParameterDescriptor::ForVariadic(Retains);
  } else if constexpr (IsOptionalValueParameter<Parameter>::value) {
    using Inner = typename OptionalParameterInner<Parameter>::Type;
    constexpr ValueKind Kind = ValueKindFor<Inner>();
    if (Default)
      return ParameterDescriptor::ForDefaulted(Kind, *Default, true);
    return ParameterDescriptor::ForOptional(Kind);
  } else {
    constexpr ValueKind Kind = ValueKindFor<Parameter>();
    if (Default)
      return ParameterDescriptor::ForDefaulted(Kind, *Default, false);
    return ParameterDescriptor::ForRequired(Kind);
  }
}

template <class... Parameters>
[[nodiscard]] std::vector<ParameterDescriptor>
MakeParameterDescriptors(std::span<const Value> Defaults) {
  constexpr std::size_t FixedCount = FixedParameterCountOf<Parameters...>;
  std::vector<ParameterDescriptor> Descriptors;
  Descriptors.reserve(sizeof...(Parameters));
  std::size_t Position = 0;
  (Descriptors.push_back(MakeParameterDescriptor<Parameters>(
       DeclaredDefaultAt(Defaults, Position++, FixedCount))),
   ...);
  return Descriptors;
}

// The slot of one fixed parameter is usable when it carries exactly the value
// the parameter converts, and an omitted slot is usable only for a parameter
// that accepts omission.
template <class Parameter>
[[nodiscard]] bool ParameterSlotIsUsable(const InvocationArguments &Arguments,
                                         std::size_t Position) {
  if constexpr (IsVariadicParameterType<Parameter>) {
    static_cast<void>(Arguments);
    static_cast<void>(Position);
    return true;
  } else {
    const ArgumentSlot *Slot = Arguments.At(Position);
    if (!Slot)
      return false;
    const Value *Present = Slot->Get();
    if constexpr (IsOptionalValueParameter<Parameter>::value) {
      using Inner = typename OptionalParameterInner<Parameter>::Type;
      return Present == nullptr || std::holds_alternative<Inner>(*Present);
    } else {
      return Present != nullptr && std::holds_alternative<Parameter>(*Present);
    }
  }
}

// The native argument one parameter receives.
template <class Parameter>
[[nodiscard]] decltype(auto)
ParameterArgumentFor(const InvocationArguments &Arguments,
                     std::size_t Position) {
  if constexpr (std::same_as<std::remove_cvref_t<Parameter>, ArgumentView>) {
    static_cast<void>(Position);
    return Arguments.Variadic();
  } else if constexpr (std::same_as<std::remove_cvref_t<Parameter>,
                                    ArgumentPack>) {
    static_cast<void>(Position);
    return Arguments.Retained();
  } else if constexpr (IsOptionalValueParameter<Parameter>::value) {
    using Inner = typename OptionalParameterInner<Parameter>::Type;
    const ArgumentSlot *Slot = Arguments.At(Position);
    const Value *Present = Slot ? Slot->Get() : nullptr;
    if (!Present)
      return std::optional<Inner>();
    return std::optional<Inner>(std::get<Inner>(*Present));
  } else {
    const ArgumentSlot *Slot = Arguments.At(Position);
    const Value *Present = Slot ? Slot->Get() : nullptr;
    return std::get<Parameter>(*Present);
  }
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
    if constexpr (HasRelaxedParameterShape<Parameters...>) {
      static_cast<void>(Arguments);
      return InvocationOutcome::InternalFailure(
          "Callable argument metadata is inconsistent.");
    } else {
      if (Arguments.size() != sizeof...(Parameters))
        return InvocationOutcome::InternalFailure(
            "Callable argument metadata is inconsistent.");
      return InvokeWithIndices(Arguments,
                               std::index_sequence_for<Parameters...>{});
    }
  }

  // The richer call shape. Omitted optional slots, materialized defaults, and
  // the variadic tail all arrive here already validated; anything else is an
  // internal inconsistency rather than a caller mistake.
  [[nodiscard]] InvocationOutcome
  InvokeDeclared(const InvocationArguments &Arguments) {
    if (!HasTarget())
      return InvocationOutcome::InternalFailure("Callable target is null.");

    constexpr std::size_t FixedCount = FixedParameterCountOf<Parameters...>;
    constexpr bool IsVariadic = FixedCount != sizeof...(Parameters);
    if (Arguments.Size() != FixedCount || Arguments.HasVariadic() != IsVariadic)
      return InvocationOutcome::InternalFailure(
          "Callable argument metadata is inconsistent.");
    if (!SlotsAreUsable(Arguments, std::index_sequence_for<Parameters...>{}))
      return InvocationOutcome::InternalFailure(
          "Callable argument metadata is inconsistent.");
    return InvokeWithArguments(Arguments,
                               std::index_sequence_for<Parameters...>{});
  }

private:
  template <std::size_t... Indices>
  [[nodiscard]] InvocationOutcome
  InvokeWithIndices(std::span<const Value> Arguments,
                    std::index_sequence<Indices...>) {
    return CaptureReturn<Return>([&] {
      return std::invoke(TargetValue,
                         std::get<Parameters>(Arguments[Indices])...);
    });
  }

  template <std::size_t... Indices>
  [[nodiscard]] static bool SlotsAreUsable(const InvocationArguments &Arguments,
                                           std::index_sequence<Indices...>) {
    return (true && ... &&
            ParameterSlotIsUsable<Parameters>(Arguments, Indices));
  }

  template <std::size_t... Indices>
  [[nodiscard]] InvocationOutcome
  InvokeWithArguments(const InvocationArguments &Arguments,
                      std::index_sequence<Indices...>) {
    return CaptureReturn<Return>([&] {
      return std::invoke(
          TargetValue, ParameterArgumentFor<Parameters>(Arguments, Indices)...);
    });
  }

  StoredCallable TargetValue;
};

template <class Signature> struct DescriptorMetadata;

template <class Return, class... Parameters>
struct DescriptorMetadata<Return(Parameters...)> {
  [[nodiscard]] static ReturnMetadata ReturnShape() {
    if constexpr (std::same_as<Return, void>)
      return ReturnMetadata::ForVoid();
    else if constexpr (IsDynamicReturnPack<Return>)
      return ReturnMetadata::ForDynamicPack();
    else if constexpr (IsFixedReturnPack<Return>::value)
      return ReturnMetadata::ForPack(FixedReturnPackKinds<Return>::Kinds());
    else
      return ReturnMetadata::ForValue(ValueKindFor<Return>());
  }

  [[nodiscard]] static CallableMetadata Create() {
    return CreateWithDefaults(std::span<const Value>());
  }

  // One construction candidate: its parameters are described exactly as any
  // other callable's - required, optional, defaulted, or variadic - and its
  // result is one value of the registered class the declaration names.
  [[nodiscard]] static CallableMetadata
  CreateForInstance(const StableTypeKey &Class) {
    ReturnMetadata Produced = ReturnMetadata::ForInstance(Class);
    if constexpr (HasRelaxedParameterShape<Parameters...>) {
      std::vector<ParameterDescriptor> Declared =
          MakeParameterDescriptors<Parameters...>(std::span<const Value>());
      return CallableMetadata::ForDeclaredParameters(std::move(Declared),
                                                     std::move(Produced));
    } else {
      std::vector<ValueKind> ParameterTypes{ValueKindFor<Parameters>()...};
      return CallableMetadata(std::move(ParameterTypes), std::move(Produced));
    }
  }

  // One canonical metadata builder for both shapes: the foundation's fixed
  // arity keeps its exact value-kind list, and every richer shape is described
  // by immutable parameter descriptors.
  [[nodiscard]] static CallableMetadata
  CreateWithDefaults(std::span<const Value> Defaults) {
    if constexpr (HasRelaxedParameterShape<Parameters...>) {
      return CallableMetadata::ForDeclaredParameters(
          MakeParameterDescriptors<Parameters...>(Defaults), ReturnShape());
    } else {
      if (!Defaults.empty())
        return CallableMetadata::ForDeclaredParameters(
            MakeParameterDescriptors<Parameters...>(Defaults), ReturnShape());
      std::vector<ValueKind> ParameterTypes{ValueKindFor<Parameters>()...};
      return CallableMetadata(std::move(ParameterTypes), ReturnShape());
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

  if constexpr (IsDefaultedCallable<NormalizedCallable>::value) {
    // The declared defaults are read before the target is moved into its
    // adapter, so the metadata never observes a moved-from wrapper.
    CallableMetadata Metadata =
        DescriptorMetadata<Signature>::CreateWithDefaults(Target.Defaults());
    return ErasedCallableDescriptor(std::move(Metadata),
                                    Adapter(std::forward<Callable>(Target)));
  } else {
    return ErasedCallableDescriptor(DescriptorMetadata<Signature>::Create(),
                                    Adapter(std::forward<Callable>(Target)));
  }
}

} // namespace Luna::Detail
