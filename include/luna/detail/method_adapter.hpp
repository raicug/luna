#pragma once

// clang-format off
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/instance_receiver.hpp>
#include <luna/binding/supported_callable.hpp>
#include <luna/detail/callable_adapter.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/stable_type_key.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
// clang-format on

namespace Luna::Detail {

struct MethodRequest final {
  std::optional<ErasedCallableDescriptor> Callable;
  SymbolKind Kind = SymbolKind::Method;
  bool DeclaresReceiver = true;
  bool ReceiverIsConst = false;
  std::string Refusal;

  MethodRequest() = default;
  MethodRequest(const MethodRequest &) = delete;
  MethodRequest &operator=(const MethodRequest &) = delete;
  MethodRequest(MethodRequest &&) noexcept = default;
  MethodRequest &operator=(MethodRequest &&) noexcept = default;
  ~MethodRequest() = default;

  [[nodiscard]] bool HasTarget() const noexcept {
    return Callable.has_value() && Callable->HasTarget();
  }
};

template <class Type, class Parameter> struct ReceiverParameterTrait {
  static constexpr bool IsSupported = false;
  static constexpr bool ReadsOnly = false;
};

template <class Type> struct ReceiverParameterTrait<Type, Type &> {
  static constexpr bool IsSupported = true;
  static constexpr bool ReadsOnly = false;

  [[nodiscard]] static Type &Supplied(Type *Object) noexcept { return *Object; }
};

template <class Type> struct ReceiverParameterTrait<Type, const Type &> {
  static constexpr bool IsSupported = true;
  static constexpr bool ReadsOnly = true;

  [[nodiscard]] static const Type &Supplied(Type *Object) noexcept {
    return *Object;
  }
};

template <class Type> struct ReceiverParameterTrait<Type, Type *> {
  static constexpr bool IsSupported = true;
  static constexpr bool ReadsOnly = false;

  [[nodiscard]] static Type *Supplied(Type *Object) noexcept { return Object; }
};

template <class Type> struct ReceiverParameterTrait<Type, const Type *> {
  static constexpr bool IsSupported = true;
  static constexpr bool ReadsOnly = true;

  [[nodiscard]] static const Type *Supplied(Type *Object) noexcept {
    return Object;
  }
};

using UnsupportedMemberShape = void;

template <class Type, class Signature> struct WrapperMethodShape {
  static constexpr bool IsSupported = false;
  static constexpr bool ReceiverIsConst = false;
  using Declared = UnsupportedMemberShape;
};

template <class Type, class Return, class First, class... Rest>
struct WrapperMethodShape<Type, Return(First, Rest...)> {
  using Receiver = ReceiverParameterTrait<Type, First>;

  static constexpr bool IsSupported = Receiver::IsSupported;
  static constexpr bool ReceiverIsConst = Receiver::ReadsOnly;
  using Declared = Return(Rest...);

  template <class Target>
  static Return Call(Target &Selected, Type *Object, Rest... Supplied) {
    return std::invoke(Selected, Receiver::Supplied(Object),
                       std::forward<Rest>(Supplied)...);
  }
};

template <class Type, class Target, class = void> struct MethodTargetShape {
  static constexpr bool IsSupported = false;
  static constexpr bool ReceiverIsConst = false;
  using Declared = UnsupportedMemberShape;
};

template <class Type, class Target>
struct MethodTargetShape<
    Type, Target,
    std::void_t<typename CallableSignature<std::decay_t<Target>>::Type>>
    : WrapperMethodShape<
          Type, typename CallableSignature<std::decay_t<Target>>::Type> {};

template <class Type, class Owner, class Return, class... Parameters>
struct MethodTargetShape<Type, Return (Owner::*)(Parameters...), void> {
  static_assert(std::is_base_of_v<Owner, Type>,
                "A Luna method declares a member function pointer of the "
                "registered class or of one of its bases.");

  static constexpr bool IsSupported = true;
  static constexpr bool ReceiverIsConst = false;
  using Declared = Return(Parameters...);
  using Pointer = Return (Owner::*)(Parameters...);

  static Return Call(Pointer &Selected, Type *Object, Parameters... Supplied) {
    return (Object->*Selected)(std::forward<Parameters>(Supplied)...);
  }
};

template <class Type, class Owner, class Return, class... Parameters>
struct MethodTargetShape<Type, Return (Owner::*)(Parameters...) const, void> {
  static_assert(std::is_base_of_v<Owner, Type>,
                "A Luna method declares a member function pointer of the "
                "registered class or of one of its bases.");

  static constexpr bool IsSupported = true;
  static constexpr bool ReceiverIsConst = true;
  using Declared = Return(Parameters...);
  using Pointer = Return (Owner::*)(Parameters...) const;

  static Return Call(Pointer &Selected, Type *Object, Parameters... Supplied) {
    return (Object->*Selected)(std::forward<Parameters>(Supplied)...);
  }
};

template <class Type, class Owner, class Return, class... Parameters>
struct MethodTargetShape<Type, Return (Owner::*)(Parameters...) noexcept,
                         void> {
  static_assert(std::is_base_of_v<Owner, Type>,
                "A Luna method declares a member function pointer of the "
                "registered class or of one of its bases.");

  static constexpr bool IsSupported = true;
  static constexpr bool ReceiverIsConst = false;
  using Declared = Return(Parameters...);
  using Pointer = Return (Owner::*)(Parameters...) noexcept;

  static Return Call(Pointer &Selected, Type *Object, Parameters... Supplied) {
    return (Object->*Selected)(std::forward<Parameters>(Supplied)...);
  }
};

template <class Type, class Owner, class Return, class... Parameters>
struct MethodTargetShape<Type, Return (Owner::*)(Parameters...) const noexcept,
                         void> {
  static_assert(std::is_base_of_v<Owner, Type>,
                "A Luna method declares a member function pointer of the "
                "registered class or of one of its bases.");

  static constexpr bool IsSupported = true;
  static constexpr bool ReceiverIsConst = true;
  using Declared = Return(Parameters...);
  using Pointer = Return (Owner::*)(Parameters...) const noexcept;

  static Return Call(Pointer &Selected, Type *Object, Parameters... Supplied) {
    return (Object->*Selected)(std::forward<Parameters>(Supplied)...);
  }
};

template <class Type, class Shape, class Signature, class Stored>
class MethodAdapter;

template <class Type, class Shape, class Return, class... Parameters,
          class Stored>
class MethodAdapter<Type, Shape, Return(Parameters...), Stored> {
public:
  template <class Target>
  explicit MethodAdapter(Target &&Selected)
      : TargetValue(std::forward<Target>(Selected)) {}

  [[nodiscard]] bool HasTarget() const noexcept {
    if constexpr (std::is_pointer_v<Stored> || std::is_member_pointer_v<Stored>)
      return TargetValue != nullptr;
    else
      return true;
  }

  [[nodiscard]] InvocationOutcome Invoke(std::span<const Value> Arguments) {
    static_cast<void>(Arguments);
    return InvocationOutcome::InternalFailure(
        "Instance member invoked without its receiver.");
  }

  [[nodiscard]] InvocationOutcome
  InvokeWithReceiver(const InstanceReceiver &Receiver,
                     std::span<const Value> Arguments) {
    Type *const Object = ValidatedObject(Receiver);
    if (!Object)
      return InvocationOutcome::InternalFailure(UnboundReceiverText());
    if constexpr (HasRelaxedParameterShape<Parameters...>) {
      static_cast<void>(Arguments);
      return InvocationOutcome::InternalFailure(
          "Callable argument metadata is inconsistent.");
    } else {
      if (Arguments.size() != sizeof...(Parameters))
        return InvocationOutcome::InternalFailure(
            "Callable argument metadata is inconsistent.");
      return InvokeWithIndices(Object, Arguments,
                               std::index_sequence_for<Parameters...>{});
    }
  }

  [[nodiscard]] InvocationOutcome
  InvokeDeclaredWithReceiver(const InstanceReceiver &Receiver,
                             const InvocationArguments &Arguments) {
    Type *const Object = ValidatedObject(Receiver);
    if (!Object)
      return InvocationOutcome::InternalFailure(UnboundReceiverText());

    constexpr std::size_t FixedCount = FixedParameterCountOf<Parameters...>;
    constexpr bool IsVariadic = FixedCount != sizeof...(Parameters);
    if (Arguments.Size() != FixedCount || Arguments.HasVariadic() != IsVariadic)
      return InvocationOutcome::InternalFailure(
          "Callable argument metadata is inconsistent.");
    if (!SlotsAreUsable(Arguments, std::index_sequence_for<Parameters...>{}))
      return InvocationOutcome::InternalFailure(
          "Callable argument metadata is inconsistent.");
    return InvokeWithArguments(Object, Arguments,
                               std::index_sequence_for<Parameters...>{});
  }

private:
  [[nodiscard]] static std::string UnboundReceiverText() {
    return "Instance member received no validated receiver.";
  }

  [[nodiscard]] static Type *
  ValidatedObject(const InstanceReceiver &Receiver) noexcept {
    if (!Receiver.IsBound())
      return nullptr;
    if constexpr (!Shape::ReceiverIsConst) {
      if (!Receiver.PermitsMutation())
        return nullptr;
    }
    return static_cast<Type *>(Receiver.Storage());
  }

  template <std::size_t... Indices>
  [[nodiscard]] InvocationOutcome
  InvokeWithIndices(Type *Object, std::span<const Value> Arguments,
                    std::index_sequence<Indices...>) {
    if (!HasTarget())
      return InvocationOutcome::InternalFailure("Callable target is null.");
    return CaptureReturn<Return>([&] {
      return Shape::Call(TargetValue, Object,
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
  InvokeWithArguments(Type *Object, const InvocationArguments &Arguments,
                      std::index_sequence<Indices...>) {
    if (!HasTarget())
      return InvocationOutcome::InternalFailure("Callable target is null.");
    return CaptureReturn<Return>([&] {
      return Shape::Call(
          TargetValue, Object,
          ParameterArgumentFor<Parameters>(Arguments, Indices)...);
    });
  }

  Stored TargetValue;
};

template <class Type, class Shape, class Signature>
struct MethodCandidateBuilder;

template <class Type, class Shape, class Return, class... Parameters>
struct MethodCandidateBuilder<Type, Shape, Return(Parameters...)> final {
  template <class Target>
  [[nodiscard]] static MethodRequest Build(const StableTypeKey &Class,
                                           Target &&Selected) {
    static_assert(SupportedReturn<Return>,
                  "A Luna method declares only a supported return type.");
    static_assert((SupportedParameter<Parameters> && ...),
                  "A Luna method declares only supported parameter types.");
    static_assert(VariadicParameterShape<Parameters...>::IsValid,
                  "A Luna method declares at most one variadic parameter, and "
                  "only as its final one.");

    using Signature = Return(Parameters...);
    using Stored = std::decay_t<Target>;
    using Adapter = MethodAdapter<Type, Shape, Signature, Stored>;

    ReceiverMetadata Receiver =
        ReceiverMetadata::ForInstance(Class, Shape::ReceiverIsConst);
    CallableMetadata Ordinary = DescriptorMetadata<Signature>::Create();
    CallableMetadata Metadata = CallableMetadata::ForInstanceMember(
        std::move(Receiver), std::move(Ordinary));

    MethodRequest Request;
    Request.Kind = SymbolKind::Method;
    Request.DeclaresReceiver = true;
    Request.ReceiverIsConst = Shape::ReceiverIsConst;
    Adapter Erased(std::forward<Target>(Selected));
    Request.Callable.emplace(std::move(Metadata), std::move(Erased));
    return Request;
  }
};

template <class Type, class Target>
[[nodiscard]] MethodRequest MakeMethodRequest(const StableTypeKey &Class,
                                              Target &&Selected) {
  using Shape = MethodTargetShape<Type, std::decay_t<Target>>;
  static_assert(Shape::IsSupported,
                "A Luna method declares a member function pointer of the "
                "registered class, or an explicit wrapper whose first "
                "parameter is a reference or pointer to it.");

  using Builder = MethodCandidateBuilder<Type, Shape, typename Shape::Declared>;
  return Builder::Build(Class, std::forward<Target>(Selected));
}

template <class Target>
[[nodiscard]] MethodRequest MakeStaticMethodRequest(Target &&Selected) {
  static_assert(SupportedCallable<Target>,
                "A Luna static method declares an ordinary supported callable: "
                "a free function, a function pointer, a lambda, a functor, or "
                "an explicit overload selection.");

  MethodRequest Request;
  Request.Kind = SymbolKind::StaticMethod;
  Request.DeclaresReceiver = false;
  Request.ReceiverIsConst = false;
  ErasedCallableDescriptor Erased =
      MakeErasedCallableDescriptor(std::forward<Target>(Selected));
  Request.Callable.emplace(std::move(Erased));
  return Request;
}

} // namespace Luna::Detail
