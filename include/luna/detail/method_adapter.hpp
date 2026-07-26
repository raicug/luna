#pragma once

// Instance and static members as ordinary callable candidates.
//
// A method is not a second invocation pipeline. Its ordinary parameters are
// described by exactly the canonical parameter descriptors every other
// declaration uses - required, optional, defaulted, or variadic - it joins the
// same canonical overload set, it converts through the same registry, and it
// reports the same deterministic diagnostics. The one thing it adds is the
// object it operates on: the receiver, which is rank position zero of the call
// rather than one of its arguments.
//
// Three target forms declare one instance method, and each one states its
// receiver rather than having it guessed:
//
//   * a member function pointer of the class, or of a base of it, whose const
//     qualification states whether the method only reads its object;
//   * an explicit wrapper - a free function, a lambda, a functor, or an
//     `Overload<Signature, Class>` selection - whose first parameter is
//     `Class &`, `const Class &`, `Class *`, or `const Class *`;
//   * either of those declared under a name several methods share, which makes
//     them one canonical overload set.
//
// The receiver is never converted here. It arrives already validated - present,
// of this State, of this class, live, of the requested dynamic type, and
// mutable when the method mutates - because validated access is the only thing
// that produces one. Calling a member function pointer through the supplied
// object is an ordinary virtual call, so a virtual method dispatches on the
// object the call site supplied rather than on the declared class.
//
// A static method declares no receiver at all and is therefore built by the
// ordinary callable adapter, which is exactly what routes it through the
// ordinary function pipeline.

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

// One staged member candidate of one class: the erased candidate, the reflected
// symbol kind it declares, whether its receiver is const, and the first
// deterministic refusal the declaration recorded, if any.
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

// How one wrapper's first parameter names the object it operates on. A
// reference or pointer to the class is a receiver; anything else is not, so the
// declaration is refused where it is written instead of being reinterpreted as
// an ordinary argument.
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

// The declared shape a target that is not a member target at all reports. It
// exists only so a refused declaration reads as its own static assertion rather
// than as an unrelated cascade.
using UnsupportedMemberShape = void;

// The declared shape of one wrapper target: its first parameter is the
// receiver, and every later parameter is an ordinary parameter of the method.
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

// The declared shape of one method target. A member function pointer states its
// receiver through its own class and const qualification; every other callable
// states it through its first parameter.
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

// One erased instance member. Everything about its ordinary parameters is the
// canonical callable machinery; the receiver is the one thing it adds, and it
// is never converted here.
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

  // An instance member is never invoked without the receiver it operates on.
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

  // A member that mutates its object is never invoked through a const view, and
  // a receiver that never passed validation is never dereferenced. Both are
  // already decided before this point; repeating them here is what keeps a
  // native pointer unreachable by ignoring a refusal.
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

// One instance member candidate, built over the ordinary parameter list its
// target declares.
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

// One instance method of `Type`, declared by any of its accepted target forms.
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

// One static method of `Type`. It declares no receiver, so it is exactly an
// ordinary callable candidate reflected under the class scope.
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
