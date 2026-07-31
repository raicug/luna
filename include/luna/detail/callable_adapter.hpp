#pragma once

// clang-format off
#include <luna/binding/argument_pack.hpp>
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/class_construction.hpp>
#include <luna/binding/parameter_descriptor.hpp>
#include <luna/binding/return_pack.hpp>
#include <luna/binding/supported_callable.hpp>
#include <luna/type/stable_type_key.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
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

template <class Return>
[[nodiscard]] ConstructedInstance
AdoptInstanceReturn(Return &&Produced, const OwnershipPolicy &Policy) {
  using Trait = InstanceReturnTrait<std::remove_cvref_t<Return>>;
  using Native = typename Trait::Native;

  if constexpr (std::is_pointer_v<std::remove_cvref_t<Return>>) {
    static_cast<void>(Policy);
    return BorrowedClassInstance(static_cast<void *>(Produced),
                                 Policy.Lifetime());
  } else if constexpr (std::is_same_v<std::remove_cvref_t<Return>,
                                      std::shared_ptr<Native>>) {
    static_cast<void>(Policy);
    std::shared_ptr<Native> Held = std::forward<Return>(Produced);
    Native *const Object = Held.get();
    return SharedClassInstance(Object,
                               std::static_pointer_cast<void>(std::move(Held)));
  } else {
    static_cast<void>(Policy);
    ClassAllocator Protocol =
        ClassAllocator::ForOwnedObject<Native>(ConstructedStoragePolicyName);
    auto Held = std::make_shared<Native>(std::forward<Return>(Produced));
    ClassAllocator::ConstructOperation Build = [Held](void *Storage) {
      static_cast<void>(new (Storage) Native(std::move(*Held)));
      return AllocatorStepResult::Done();
    };
    return CreatedClassInstance(std::move(Protocol), std::move(Build));
  }
}

[[nodiscard]] inline OwnershipPolicy UndeclaredOwnershipPolicy() {
  return OwnershipPolicy::LuaOwned();
}

template <class Return>
[[nodiscard]] std::string
ClassifyInstanceReturnPolicy(const OwnershipPolicy &Declared) {
  if constexpr (IsInstanceReturnType<Return>) {
    if constexpr (InstanceReturnTrait<Return>::RequiresLifetime) {
      if (Declared.Ownership() != ConstructionOwnership::Borrowed ||
          !Declared.IsCoherent())
        return "publishing a class instance by pointer is a borrowed result, "
               "so "
               "the declaration states Luna::OwnershipPolicy::Borrowed with "
               "one declared lifetime.";
      return std::string();
    } else if (Declared.Lifetime().IsDeclared()) {
      return "a lifetime is declared for a result that is not borrowed; only a "
             "pointer result borrows.";
    }
  } else {
    static_cast<void>(Declared);
  }
  return std::string();
}

template <class Return, class Invoker>
[[nodiscard]] InvocationOutcome
CaptureReturn(Invoker &&Invoke,
              const OwnershipPolicy &Policy = UndeclaredOwnershipPolicy()) {
  if constexpr (AsyncReturnTrait<Return>::value) {
    static_cast<void>(Policy);
    return InvocationOutcome::Suspended(MakePendingAsyncWork(Invoke()));
  } else if constexpr (std::same_as<Return, void>) {
    Invoke();
    return InvocationOutcome::Void();
  } else if constexpr (std::same_as<Return, ConstructedInstance>) {
    return InvocationOutcome::WithInstance(Invoke());
  } else if constexpr (IsInstanceReturnType<Return>) {
    return InvocationOutcome::WithInstance(
        AdoptInstanceReturn<Return>(Invoke(), Policy));
  } else if constexpr (IsDynamicReturnPack<Return>) {
    const ReturnPack Produced = Invoke();
    if (Produced.CarriesOwnedValues())
      return InvocationOutcome::WithOwnedValues(Produced.ToOwnedValues());
    const std::span<const Value> Elements = Produced.Values();
    return InvocationOutcome::WithValues(
        std::vector<Value>(Elements.begin(), Elements.end()));
  } else if constexpr (IsOwnedValueReturn<Return>) {
    ValuePack Produced;
    Produced.Append(Invoke());
    return InvocationOutcome::WithOwnedValues(std::move(Produced));
  } else if constexpr (IsOwnedPackReturn<Return>) {
    return InvocationOutcome::WithOwnedValues(Invoke());
  } else if constexpr (IsChunkReturn<Return>) {
    const Luna::Chunk Produced = Invoke();
    if (!Produced.IsLoaded()) {
      const ErrorDiagnostic *Refused = Produced.Diagnostic();
      return InvocationOutcome::InternalFailure(
          Refused != nullptr ? Refused->Message()
                             : std::string("the callable produced no loadable "
                                           "chunk."));
    }
    return InvocationOutcome::WithChunk(std::string(Produced.Bytecode()),
                                        std::string(Produced.Name()));
  } else if constexpr (IsFixedReturnPack<Return>::value) {
    return InvocationOutcome::WithValues(StageReturnPack(Invoke()));
  } else {
    return InvocationOutcome::WithValue(
        Value(std::in_place_type<Return>, Invoke()));
  }
}

template <class... Parameters>
inline constexpr bool HasRelaxedParameterShape =
    (false || ... || IsRelaxedParameter<Parameters>);

template <class... Parameters>
inline constexpr std::size_t FixedParameterCountOf =
    (std::size_t{0} + ... +
     (IsVariadicParameterType<Parameters> ? std::size_t{0} : std::size_t{1}));

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

[[nodiscard]] inline std::size_t NextConvertedParameterOrdinal() noexcept {
  static std::atomic<std::size_t> Ordinal{0};
  return Ordinal.fetch_add(1, std::memory_order_relaxed);
}

template <class Native>
[[nodiscard]] const StableTypeKey &ConvertedParameterKeyFor() {
  static const StableTypeKey Key(
      "ConvertedParameter" + std::to_string(NextConvertedParameterOrdinal()));
  return Key;
}

template <class Parameter>
[[nodiscard]] ConvertedParameterShape ConvertedParameterShapeOf() {
  using Native = std::remove_cvref_t<Parameter>;
  ConvertedParameterShape Shape;
  Shape.Probe = +[](ValueView Source,
                    const ConversionContext &Context) -> ConversionProbe {
    return ProbeValue<Native>(Source, Context);
  };
  return Shape;
}

template <class Parameter>
[[nodiscard]] ParameterDescriptor
MakeParameterDescriptor(const Value *Default) {
  if constexpr (IsVariadicParameterType<Parameter>) {
    static_cast<void>(Default);
    constexpr bool Retains =
        std::same_as<std::remove_cvref_t<Parameter>, ArgumentPack>;
    return ParameterDescriptor::ForVariadic(Retains);
  } else if constexpr (IsDelegateParameterType<Parameter>) {
    static_cast<void>(Default);
    using Signature = DelegateParameterSignatureOf<Parameter>;
    return ParameterDescriptor::ForDelegate(
        DelegateSignatureShape<Signature>::Shape());
  } else if constexpr (IsConvertedParameterType<Parameter>) {
    static_cast<void>(Default);
    using Native = std::remove_cvref_t<Parameter>;
    return ParameterDescriptor::ForConverted(
        ConvertedParameterKeyFor<Native>(),
        ConvertedParameterShapeOf<Parameter>());
  } else if constexpr (IsInstanceParameterType<Parameter>) {
    static_cast<void>(Default);
    using Declared = InstanceParameterTrait<Parameter>;
    Luna::InstanceParameterShape Shape;
    Shape.Resolve = ClassKeyResolverFor<typename Declared::Native>();
    Shape.RequiresMutation = Declared::RequiresMutation;
    return ParameterDescriptor::ForInstance(Shape);
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

template <class Parameter>
[[nodiscard]] bool ParameterSlotIsUsable(const InvocationArguments &Arguments,
                                         std::size_t Position) {
  if constexpr (IsVariadicParameterType<Parameter>) {
    static_cast<void>(Arguments);
    static_cast<void>(Position);
    return true;
  } else if constexpr (IsDelegateParameterType<Parameter>) {
    const ArgumentSlot *Slot = Arguments.At(Position);
    return Slot != nullptr && Slot->HasHandler();
  } else if constexpr (IsConvertedParameterType<Parameter>) {
    const ArgumentSlot *Slot = Arguments.At(Position);
    return Slot != nullptr && Slot->HasConvertedValue();
  } else if constexpr (IsInstanceParameterType<Parameter>) {
    const ArgumentSlot *Slot = Arguments.At(Position);
    if (!Slot || !Slot->HasInstance())
      return false;
    if constexpr (InstanceParameterTrait<Parameter>::RequiresMutation)
      return Slot->Instance().PermitsMutation();
    else
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
  } else if constexpr (IsDelegateParameterType<Parameter>) {
    using Declared = std::remove_cvref_t<Parameter>;
    using Signature = DelegateParameterSignatureOf<Parameter>;
    const ArgumentSlot *Slot = Arguments.At(Position);
    Delegate<Signature> Subscribed(Slot ? Slot->Handler() : nullptr);
    return Declared(std::move(Subscribed));
  } else if constexpr (IsConvertedParameterType<Parameter>) {
    using Native = std::remove_cvref_t<Parameter>;
    const ArgumentSlot *Slot = Arguments.At(Position);
    const OwnedValue *Source = Slot ? Slot->ConvertedValue() : nullptr;
    ConversionResult<Native> Read = ReadConvertedArgument<Native>(
        Source ? *Source : OwnedValue(), std::string_view(), Position);
    return Read.ConvertedValue ? std::move(*Read.ConvertedValue) : Native();
  } else if constexpr (IsInstanceParameterType<Parameter>) {
    using Declared = InstanceParameterTrait<Parameter>;
    using Native = typename Declared::Native;
    const ArgumentSlot *Slot = Arguments.At(Position);
    auto *const Object =
        static_cast<Native *>(Slot ? Slot->Instance().Storage() : nullptr);
    if constexpr (Declared::IsPointer)
      return Object;
    else if constexpr (Declared::IsCopied)
      return Native(*Object);
    else
      return static_cast<Parameter>(*Object);
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

template <class Signature> struct CallableReturnOf;

template <class Return, class... Parameters>
struct CallableReturnOf<Return(Parameters...)> {
  using Type = Return;
};

template <class Return, class... Parameters>
struct CallableReturnOf<Return(Parameters..., ...)> {
  using Type = Return;
};

template <class Signature, class StoredCallable> class CallableAdapter;

template <class Return, class... Parameters, class StoredCallable>
class CallableAdapter<Return(Parameters...), StoredCallable> {
public:
  template <class Callable>
  explicit CallableAdapter(
      Callable &&Target, OwnershipPolicy Declared = UndeclaredOwnershipPolicy())
      : TargetValue(std::forward<Callable>(Target)),
        PolicyValue(std::move(Declared)) {}

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
    return CaptureReturn<Return>(
        [&] {
          return std::invoke(TargetValue,
                             std::get<Parameters>(Arguments[Indices])...);
        },
        PolicyValue);
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
    return CaptureReturn<Return>(
        [&] {
          return std::invoke(TargetValue, ParameterArgumentFor<Parameters>(
                                              Arguments, Indices)...);
        },
        PolicyValue);
  }

  StoredCallable TargetValue;
  OwnershipPolicy PolicyValue = UndeclaredOwnershipPolicy();
};

template <class Signature> struct DescriptorMetadata;

template <class Return, class... Parameters>
struct DescriptorMetadata<Return(Parameters...)> {
  [[nodiscard]] static ReturnMetadata AwaitedShape() {
    using Awaited = typename AsyncReturnTrait<Return>::ResultType;
    if constexpr (std::same_as<Awaited, void>)
      return ReturnMetadata::ForVoid();
    else if constexpr (IsDynamicReturnPack<Awaited>)
      return ReturnMetadata::ForDynamicPack();
    else
      return ReturnMetadata::ForValue(ValueKindFor<Awaited>());
  }

  [[nodiscard]] static ReturnMetadata ReturnShape() {
    if constexpr (AsyncReturnTrait<Return>::value)
      return ReturnMetadata::ForAsync(AwaitedShape());
    else if constexpr (std::same_as<Return, void>)
      return ReturnMetadata::ForVoid();
    else if constexpr (IsDynamicReturnPack<Return>)
      return ReturnMetadata::ForDynamicPack();
    else if constexpr (IsOwnedValueReturn<Return>)
      return ReturnMetadata::ForOwnedValue();
    else if constexpr (IsOwnedPackReturn<Return>)
      return ReturnMetadata::ForOwnedPack();
    else if constexpr (IsChunkReturn<Return>)
      return ReturnMetadata::ForChunk();
    else if constexpr (IsInstanceReturnType<Return>)
      return ReturnMetadata::ForInstance(
          RecordedClassKey<typename InstanceReturnTrait<Return>::Native>());
    else if constexpr (IsFixedReturnPack<Return>::value)
      return ReturnMetadata::ForPack(FixedReturnPackKinds<Return>::Kinds());
    else
      return ReturnMetadata::ForValue(ValueKindFor<Return>());
  }

  [[nodiscard]] static CallableMetadata Create() {
    return CreateWithDefaults(std::span<const Value>());
  }

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

struct PublishedValueRequest final {
  StableTypeKey Class;
  std::optional<ConstructedInstance> Produced;
  std::string Refusal;

  [[nodiscard]] bool HasObject() const noexcept { return Produced.has_value(); }
};

template <class Producer>
[[nodiscard]] PublishedValueRequest MakePublishedValueRequest(
    Producer &&Produce,
    OwnershipPolicy Declared = UndeclaredOwnershipPolicy()) {
  using Normalized = std::remove_cvref_t<Producer>;
  static_assert(std::is_invocable_v<Normalized &>,
                "A Luna published value declares a producer taking no "
                "arguments: a function, a function pointer, a lambda, or a "
                "functor.");
  using Return = std::invoke_result_t<Normalized &>;

  PublishedValueRequest Request;
  Normalized Target(std::forward<Producer>(Produce));

  if constexpr (AsyncReturnTrait<Return>::value) {
    static_cast<void>(Target);
    Request.Refusal =
        "a published value is produced and published on the owner thread, so "
        "an asynchronous producer is refused; a namespace or root function "
        "delivers an awaited value instead.";
  } else if constexpr (!IsInstanceReturnType<Return>) {
    static_cast<void>(Target);
    Request.Refusal =
        "a published value names one registered class instance, so the "
        "producer returns T, std::shared_ptr<T>, or a borrowed T * of a class "
        "that opted in with Luna::RegisteredClassTrait.";
  } else {
    using Native = typename InstanceReturnTrait<Return>::Native;
    Request.Class = RecordedClassKey<Native>();
    Request.Refusal = ClassifyInstanceReturnPolicy<Return>(Declared);
    if (Request.Refusal.empty()) {
      Return Object = std::invoke(Target);
      if constexpr (std::is_pointer_v<Return> ||
                    !std::is_same_v<Return, Native>) {
        if (!Object) {
          Request.Refusal = "the producer returned no object.";
          return Request;
        }
      }
      Request.Produced =
          AdoptInstanceReturn<Return>(std::move(Object), Declared);
    }
  }
  return Request;
}

template <SupportedCallable Callable>
[[nodiscard]] ErasedCallableDescriptor MakeErasedCallableDescriptor(
    Callable &&Target, OwnershipPolicy Declared = UndeclaredOwnershipPolicy()) {
  using NormalizedCallable = std::remove_cvref_t<Callable>;
  using Signature = typename CallableSignature<NormalizedCallable>::Type;
  using StoredCallable = std::decay_t<Callable>;
  using Adapter = CallableAdapter<Signature, StoredCallable>;

  if constexpr (IsDefaultedCallable<NormalizedCallable>::value) {
    CallableMetadata Metadata =
        DescriptorMetadata<Signature>::CreateWithDefaults(Target.Defaults());
    return ErasedCallableDescriptor(
        std::move(Metadata),
        Adapter(std::forward<Callable>(Target), std::move(Declared)));
  } else {
    return ErasedCallableDescriptor(
        DescriptorMetadata<Signature>::Create(),
        Adapter(std::forward<Callable>(Target), std::move(Declared)));
  }
}

} // namespace Luna::Detail
