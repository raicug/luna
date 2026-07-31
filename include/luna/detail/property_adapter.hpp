#pragma once

// clang-format off
#include <luna/binding/class_construction.hpp>
#include <luna/binding/class_member.hpp>
#include <luna/binding/supported_callable.hpp>
#include <luna/binding/value.hpp>
#include <luna/detail/callable_adapter.hpp>
#include <luna/detail/canonical_type.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
// clang-format on

namespace Luna::Detail {

struct MemberRequest final {
  SymbolKind Kind = SymbolKind::Property;
  MemberAccess Access = MemberAccess::ReadOnly;
  PropertyEvaluation Evaluation = PropertyEvaluation::Immediate;
  MemberOwnership Ownership = MemberOwnership::Copied;

  TypeDescriptor ValueType;
  TypeDescriptor ReceiverType;

  bool ReadRequiresMutableReceiver = false;

  MemberReadOperation Read;
  MemberWriteOperation Write;
  MemberChangeOperation Change;

  MemberConvertedReadOperation ConvertedRead;
  MemberConvertedWriteOperation ConvertedWrite;

  MemberInstanceReadOperation InstanceRead;
  MemberInstanceWriteOperation InstanceWrite;

  bool InstanceWriteRequiresMutation = false;

  std::string Refusal;

  [[nodiscard]] bool HasReader() const noexcept {
    return Read != nullptr || ConvertedRead != nullptr ||
           InstanceRead != nullptr;
  }
  [[nodiscard]] bool HasWriter() const noexcept {
    return Write != nullptr || ConvertedWrite != nullptr ||
           InstanceWrite != nullptr;
  }
  [[nodiscard]] bool HasChangeHandler() const noexcept {
    return Change != nullptr;
  }
};

template <class Class, class Target, class = void> struct MemberReadShape {
  static constexpr bool IsSupported = false;
  static constexpr bool RequiresMutableReceiver = false;
  using Declared = void;
};

template <class Class, class Produced>
struct MemberReadShape<Class, Produced (Class::*)() const> {
  static constexpr bool IsSupported = true;
  static constexpr bool RequiresMutableReceiver = false;
  using Declared = std::remove_cvref_t<Produced>;
  using Accessor = Produced (Class::*)() const;

  [[nodiscard]] static Declared Read(const void *Object,
                                     const Accessor &Getter) {
    const auto *Typed = static_cast<const Class *>(Object);
    return (Typed->*Getter)();
  }
};

template <class Class, class Produced>
struct MemberReadShape<Class, Produced (Class::*)()> {
  static constexpr bool IsSupported = true;
  static constexpr bool RequiresMutableReceiver = true;
  using Declared = std::remove_cvref_t<Produced>;
  using Accessor = Produced (Class::*)();

  [[nodiscard]] static Declared Read(const void *Object,
                                     const Accessor &Getter) {
    auto *Typed = static_cast<Class *>(const_cast<void *>(Object));
    return (Typed->*Getter)();
  }
};

template <class Class, class Held>
struct MemberReadShape<Class, Held Class::*,
                       std::enable_if_t<!std::is_function_v<Held>>> {
  static constexpr bool IsSupported = true;
  static constexpr bool RequiresMutableReceiver = false;
  using Declared = std::remove_cv_t<Held>;
  using Accessor = Held Class::*;

  [[nodiscard]] static Declared Read(const void *Object,
                                     const Accessor &Member) {
    const auto *Typed = static_cast<const Class *>(Object);
    return Typed->*Member;
  }
};

template <class Class, class Signature> struct MemberReadCallableShape {
  static constexpr bool IsSupported = false;
  static constexpr bool RequiresMutableReceiver = false;
  using Declared = void;
};

template <class Class, class Produced, class Receiver>
struct MemberReadCallableShape<Class, Produced(Receiver)> {
  static constexpr bool IsSupported =
      std::is_same_v<std::remove_cvref_t<Receiver>, Class>;
  static constexpr bool RequiresMutableReceiver =
      std::is_lvalue_reference_v<Receiver> &&
      !std::is_const_v<std::remove_reference_t<Receiver>>;
  using Declared = std::remove_cvref_t<Produced>;

  template <class Accessor>
  [[nodiscard]] static Declared Read(const void *Object, Accessor &Getter) {
    auto *Typed = static_cast<Class *>(const_cast<void *>(Object));
    return Getter(*Typed);
  }
};

template <class Class, class Target>
struct MemberReadShape<Class, Target,
                       std::void_t<typename CallableSignature<Target>::Type>>
    : MemberReadCallableShape<Class, typename CallableSignature<Target>::Type> {
};

template <class Class, class Target, class = void> struct MemberWriteShape {
  static constexpr bool IsSupported = false;
  using Declared = void;
};

template <class Class, class Result, class Accepted>
struct MemberWriteShape<Class, Result (Class::*)(Accepted)> {
  static_assert(std::is_void_v<Result>,
                "A Luna property or field setter returns nothing.");

  static constexpr bool IsSupported = true;
  using Declared = std::remove_cvref_t<Accepted>;
  using Mutator = Result (Class::*)(Accepted);

  static void Write(void *Object, const Mutator &Setter, Declared Incoming) {
    auto *Typed = static_cast<Class *>(Object);
    (Typed->*Setter)(std::move(Incoming));
  }
};

template <class Class, class Held>
struct MemberWriteShape<
    Class, Held Class::*,
    std::enable_if_t<!std::is_function_v<Held> && !std::is_const_v<Held>>> {
  static constexpr bool IsSupported = true;
  using Declared = std::remove_cv_t<Held>;
  using Mutator = Held Class::*;

  static void Write(void *Object, const Mutator &Member, Declared Incoming) {
    auto *Typed = static_cast<Class *>(Object);
    Typed->*Member = std::move(Incoming);
  }
};

template <class Class, class Signature> struct MemberWriteCallableShape {
  static constexpr bool IsSupported = false;
  using Declared = void;
};

template <class Class, class Result, class Receiver, class Accepted>
struct MemberWriteCallableShape<Class, Result(Receiver, Accepted)> {
  static constexpr bool IsSupported =
      std::is_void_v<Result> &&
      std::is_same_v<std::remove_cvref_t<Receiver>, Class>;
  using Declared = std::remove_cvref_t<Accepted>;

  template <class Mutator>
  static void Write(void *Object, Mutator &Setter, Declared Incoming) {
    auto *Typed = static_cast<Class *>(Object);
    Setter(*Typed, std::move(Incoming));
  }
};

template <class Class, class Target>
struct MemberWriteShape<Class, Target,
                        std::void_t<typename CallableSignature<Target>::Type>>
    : MemberWriteCallableShape<Class,
                               typename CallableSignature<Target>::Type> {};

template <class Class, class Target, class = void>
struct MemberInstanceWriteShape {
  static constexpr bool IsSupported = false;
  static constexpr bool RequiresMutation = false;
  using Native = void;
};

template <class Class, class Result, class Accepted>
struct MemberInstanceWriteShape<Class, Result (Class::*)(Accepted)> {
  static constexpr bool IsSupported =
      std::is_void_v<Result> && IsInstanceParameterType<Accepted>;
  static constexpr bool RequiresMutation =
      InstanceParameterTrait<Accepted>::RequiresMutation;
  using Native = InstanceParameterNative<Accepted>;
  using Mutator = Result (Class::*)(Accepted);

  static void Write(void *Object, const Mutator &Setter, void *Incoming) {
    auto *Typed = static_cast<Class *>(Object);
    auto *Held = static_cast<Native *>(Incoming);
    if constexpr (InstanceParameterTrait<Accepted>::IsPointer)
      (Typed->*Setter)(Held);
    else if constexpr (InstanceParameterTrait<Accepted>::IsCopied)
      (Typed->*Setter)(Native(*Held));
    else
      (Typed->*Setter)(static_cast<Accepted>(*Held));
  }
};

template <class Class, class Signature>
struct MemberInstanceWriteCallableShape {
  static constexpr bool IsSupported = false;
  static constexpr bool RequiresMutation = false;
  using Native = void;
};

template <class Class, class Result, class Receiver, class Accepted>
struct MemberInstanceWriteCallableShape<Class, Result(Receiver, Accepted)> {
  static constexpr bool IsSupported =
      std::is_void_v<Result> &&
      std::is_same_v<std::remove_cvref_t<Receiver>, Class> &&
      IsInstanceParameterType<Accepted>;
  static constexpr bool RequiresMutation =
      InstanceParameterTrait<Accepted>::RequiresMutation;
  using Native = InstanceParameterNative<Accepted>;

  template <class Mutator>
  static void Write(void *Object, Mutator &Setter, void *Incoming) {
    auto *Typed = static_cast<Class *>(Object);
    auto *Held = static_cast<Native *>(Incoming);
    if constexpr (InstanceParameterTrait<Accepted>::IsPointer)
      Setter(*Typed, Held);
    else if constexpr (InstanceParameterTrait<Accepted>::IsCopied)
      Setter(*Typed, Native(*Held));
    else
      Setter(*Typed, static_cast<Accepted>(*Held));
  }
};

template <class Class, class Target>
struct MemberInstanceWriteShape<
    Class, Target, std::void_t<typename CallableSignature<Target>::Type>>
    : MemberInstanceWriteCallableShape<
          Class, typename CallableSignature<Target>::Type> {};

template <class Declared>
inline constexpr bool IsConvertedMemberValue =
    !SupportedValue<Declared> && std::is_class_v<Declared> &&
    ConversionCapable<Declared>;

template <class Declared>
inline constexpr bool IsInstanceMemberValue = IsInstanceReturnType<Declared>;

inline constexpr std::string_view WritableInstanceMemberRefusal =
    "a field whose value is a registered class instance publishes one object "
    "per read, so it is read-only; declare a property with a getter and a "
    "setter taking that class as an instance operand instead.";

inline constexpr std::string_view InstanceMemberChangeRefusal =
    "an on-change handler receives the newly written value as one canonical "
    "Luna value, which a registered class instance is not, so an "
    "instance-valued property declares no on-change handler.";

[[nodiscard]] inline StableTypeKey
SyntheticConvertedValueKey(const StableTypeKey &ClassKey,
                           std::string_view MemberName) {
  return StableTypeKey(std::string(ClassKey.Text()) + ".converted." +
                       std::string(MemberName));
}

template <class Declared>
[[nodiscard]] inline TypeDescriptor
MemberValueDescriptor(const StableTypeKey &ConvertedKey = StableTypeKey()) {
  if constexpr (SupportedValue<Declared>)
    return CanonicalDescriptorFor<Declared>();
  else if constexpr (IsConvertedMemberValue<Declared>)
    return TypeDescriptor::ForConverted(ConvertedKey);
  else
    return TypeDescriptor::Unsupported();
}

template <class Declared>
[[nodiscard]] inline TypeDescriptor InstanceMemberValueDescriptor() {
  if constexpr (IsInstanceMemberValue<Declared>)
    return TypeDescriptor::ForClass(
        RecordedClassKey<typename InstanceReturnTrait<Declared>::Native>());
  else
    return TypeDescriptor::Unsupported();
}

template <class Class, class Target>
[[nodiscard]] MemberReadOperation MakeMemberReader(Target Accessor) {
  using Shape = MemberReadShape<Class, Target>;
  static_assert(Shape::IsSupported,
                "A Luna property or field getter is a const or non-const "
                "accessor of the class, a data member of it, or a callable "
                "taking the class and returning one supported value type.");
  using Declared = typename Shape::Declared;
  static_assert(SupportedValue<Declared>,
                "A Luna property or field exposes one supported value type.");

  return [Accessor](const void *Object) mutable -> MemberReadOutcome {
    if (Object == nullptr)
      return MemberReadOutcome::Refuse(
          "the generated getter received no native object.");
    return MemberReadOutcome::Accept(Value(Shape::Read(Object, Accessor)));
  };
}

template <class Class, class Target>
[[nodiscard]] MemberInstanceReadOperation
MakeMemberInstanceReader(Target Accessor, OwnershipPolicy Declared) {
  using Shape = MemberReadShape<Class, Target>;
  static_assert(Shape::IsSupported,
                "A Luna property or field getter is a const or non-const "
                "accessor of the class, a data member of it, or a callable "
                "taking the class and returning one value type.");
  using Produced = typename Shape::Declared;
  static_assert(IsInstanceMemberValue<Produced>,
                "An instance property or field publishes one registered class "
                "instance: the class itself, a std::shared_ptr to it, or a "
                "borrowed pointer to it.");

  return [Accessor,
          Declared](const void *Object) mutable -> MemberInstanceOutcome {
    if (Object == nullptr)
      return MemberInstanceOutcome::Refuse(
          "the generated getter received no native object.");
    Produced Value = Shape::Read(Object, Accessor);
    if constexpr (std::is_pointer_v<Produced> ||
                  !std::is_same_v<Produced, typename InstanceReturnTrait<
                                                Produced>::Native>) {
      if (!Value)
        return MemberInstanceOutcome::Refuse(
            "the generated getter produced no object.");
    }
    return MemberInstanceOutcome::Accept(
        AdoptInstanceReturn<Produced>(std::move(Value), Declared));
  };
}

template <class Class, class Target>
[[nodiscard]] MemberInstanceWriteOperation
MakeMemberInstanceWriter(Target Mutator) {
  using Shape = MemberInstanceWriteShape<Class, Target>;
  static_assert(Shape::IsSupported,
                "A Luna instance property setter is a mutator of the class, or "
                "a callable taking the class, whose one parameter names the "
                "same registered class as an instance operand: T, const T &, "
                "T &, T *, or const T *.");

  return [Mutator](void *Object, void *Incoming) mutable -> MemberWriteOutcome {
    if (Object == nullptr)
      return MemberWriteOutcome::Refuse(
          "the generated setter received no native object.");
    if (Incoming == nullptr)
      return MemberWriteOutcome::Refuse(
          "the generated setter received no incoming object.");
    Shape::Write(Object, Mutator, Incoming);
    return MemberWriteOutcome::Accept();
  };
}

template <class Class, class Target>
[[nodiscard]] MemberConvertedReadOperation
MakeMemberConvertedReader(Target Accessor) {
  using Shape = MemberReadShape<Class, Target>;
  static_assert(Shape::IsSupported,
                "A Luna property or field getter is a const or non-const "
                "accessor of the class, a data member of it, or a callable "
                "taking the class and returning one value type.");
  using Declared = typename Shape::Declared;
  static_assert(ConversionCapable<Declared>,
                "A converted property or field getter returns one type with "
                "its own Luna::TypeConverter<T> specialization.");

  return
      [Accessor](
          const void *Object,
          Luna::ConversionContext &Context) mutable -> MemberConvertedOutcome {
        if (Object == nullptr)
          return MemberConvertedOutcome::Refuse(
              "the generated getter received no native object.");
        const Declared Native = Shape::Read(Object, Accessor);
        const Luna::WriteResult Written =
            Luna::WriteValue<Declared>(Native, Context);
        if (!Written.IsSuccess())
          return MemberConvertedOutcome::Refuse(Written.Diagnostic);
        return MemberConvertedOutcome::Accept();
      };
}

template <class Class, class Target>
[[nodiscard]] MemberConvertedWriteOperation
MakeMemberConvertedWriter(Target Mutator) {
  using Shape = MemberWriteShape<Class, Target>;
  static_assert(Shape::IsSupported,
                "A Luna property or field setter is a mutator of the class, a "
                "mutable data member of it, or a callable taking the class and "
                "one value type.");
  using Declared = typename Shape::Declared;
  static_assert(ConversionCapable<Declared>,
                "A converted property or field setter accepts one type with "
                "its own Luna::TypeConverter<T> specialization.");

  return
      [Mutator](
          void *Object, Luna::ValueView Source,
          Luna::ConversionContext &Context) mutable -> MemberConvertedOutcome {
        if (Object == nullptr)
          return MemberConvertedOutcome::Refuse(
              "the generated setter received no native object.");
        const Luna::ConversionResult<Declared> Read =
            Luna::ReadValue<Declared>(Source, Context);
        if (!Read.IsSuccess())
          return MemberConvertedOutcome::Refuse(Read.Diagnostic);
        Shape::Write(Object, Mutator, *Read.ConvertedValue);
        return MemberConvertedOutcome::Accept();
      };
}

template <class Class, class Declared, class Callback>
[[nodiscard]] MemberChangeOperation MakeMemberChangeHandler(Callback Handler) {
  static_assert(std::is_invocable_v<Callback &, Class &, const Declared &>,
                "A Luna property or field on-change handler accepts the "
                "class by reference and the newly written value.");

  return [Handler](void *Object, const Value &Updated) mutable {
    auto *Typed = static_cast<Class *>(Object);
    const Declared *Held = std::get_if<Declared>(&Updated);
    if (Held == nullptr)
      return;
    Handler(*Typed, *Held);
  };
}

template <class Class, class Target>
[[nodiscard]] MemberWriteOperation MakeMemberWriter(Target Mutator) {
  using Shape = MemberWriteShape<Class, Target>;
  static_assert(Shape::IsSupported,
                "A Luna property or field setter is a mutator of the class, a "
                "mutable data member of it, or a callable taking the class and "
                "one supported value type.");
  using Declared = typename Shape::Declared;
  static_assert(SupportedValue<Declared>,
                "A Luna property or field accepts one supported value type.");

  return [Mutator](void *Object,
                   const Value &Incoming) mutable -> MemberWriteOutcome {
    if (Object == nullptr)
      return MemberWriteOutcome::Refuse(
          "the generated setter received no native object.");
    const Declared *Held = std::get_if<Declared>(&Incoming);
    if (Held == nullptr)
      return MemberWriteOutcome::Refuse(
          "the generated setter received a value of another canonical type.");
    Shape::Write(Object, Mutator, *Held);
    return MemberWriteOutcome::Accept();
  };
}

[[nodiscard]] inline std::string
ClassifyPropertyPolicy(const PropertyPolicy &Policy, bool HasReader,
                       bool HasWriter) {
  if (!Policy.IsCoherent())
    return "the property policy is not coherent: a computed or lazy property "
           "must be readable.";
  if (Policy.PermitsRead() && !HasReader)
    return "the property permits reads but declares no getter.";
  if (Policy.PermitsWrite() && !HasWriter)
    return "the property permits writes but declares no setter.";
  if (!Policy.PermitsRead() && HasReader)
    return "the property declares a getter but permits no reads.";
  if (!Policy.PermitsWrite() && HasWriter)
    return "the property declares a setter but permits no writes.";
  return std::string();
}

template <class Produced>
[[nodiscard]] inline std::string
ClassifyInstancePropertyPolicy(const PropertyPolicy &Policy,
                               const OwnershipPolicy &Ownership,
                               bool HasWriter = false) {
  if (std::string Refused = ClassifyPropertyPolicy(Policy, true, HasWriter);
      !Refused.empty())
    return Refused;
  return ClassifyInstanceReturnPolicy<Produced>(Ownership);
}

[[nodiscard]] inline std::string
ClassifyFieldPolicy(const FieldPolicy &Policy) {
  if (!Policy.IsCoherent())
    return "a Luna field copies its declared value across the member "
           "boundary, so the ownership statement '" +
           std::string(MemberOwnershipText(Policy.Ownership())) +
           "' cannot be honored.";
  return std::string();
}

template <class Class, class Getter>
[[nodiscard]] MemberRequest MakeReadablePropertyRequest(
    const StableTypeKey &Key, const PropertyPolicy &Policy, Getter Accessor,
    OwnershipPolicy Ownership = UndeclaredOwnershipPolicy()) {
  using Shape = MemberReadShape<Class, Getter>;
  using Produced = typename Shape::Declared;

  MemberRequest Request;
  Request.Kind = SymbolKind::Property;
  Request.Access = Policy.Access();
  Request.Evaluation = Policy.Evaluation();
  Request.ReceiverType = TypeDescriptor::ForClass(Key);
  Request.ReadRequiresMutableReceiver = Shape::RequiresMutableReceiver;

  if constexpr (IsInstanceMemberValue<Produced>) {
    Request.Access = MemberAccess::ReadOnly;
    Request.ValueType = InstanceMemberValueDescriptor<Produced>();
    Request.InstanceRead =
        MakeMemberInstanceReader<Class, Getter>(std::move(Accessor), Ownership);
    Request.Refusal =
        ClassifyInstancePropertyPolicy<Produced>(Policy, Ownership);
  } else {
    static_cast<void>(Ownership);
    Request.ValueType = MemberValueDescriptor<Produced>();
    Request.Read = MakeMemberReader<Class, Getter>(std::move(Accessor));
    Request.Refusal = ClassifyPropertyPolicy(Policy, true, false);
  }
  return Request;
}

template <class Class, class Value, class Getter>
[[nodiscard]] MemberRequest MakeReadableConvertedPropertyRequest(
    const StableTypeKey &Key, std::string_view Name,
    const PropertyPolicy &Policy, Getter Accessor) {
  using Shape = MemberReadShape<Class, Getter>;
  static_assert(
      std::is_same_v<std::remove_cv_t<typename Shape::Declared>, Value>,
      "A converted property's named value type must match its getter's "
      "declared return type.");

  MemberRequest Request;
  Request.Kind = SymbolKind::Property;
  Request.Access = Policy.Access();
  Request.Evaluation = Policy.Evaluation();
  Request.ReceiverType = TypeDescriptor::ForClass(Key);
  Request.ValueType =
      MemberValueDescriptor<Value>(SyntheticConvertedValueKey(Key, Name));
  Request.ReadRequiresMutableReceiver = Shape::RequiresMutableReceiver;
  Request.ConvertedRead =
      MakeMemberConvertedReader<Class, Getter>(std::move(Accessor));
  Request.Refusal = ClassifyPropertyPolicy(Policy, true, false);
  return Request;
}

template <class Class, class Setter>
[[nodiscard]] MemberRequest
MakeWritablePropertyRequest(const StableTypeKey &Key,
                            const PropertyPolicy &Policy, Setter Mutator) {
  using Shape = MemberWriteShape<Class, Setter>;

  MemberRequest Request;
  Request.Kind = SymbolKind::Property;
  Request.Access = Policy.Access();
  Request.Evaluation = Policy.Evaluation();
  Request.ReceiverType = TypeDescriptor::ForClass(Key);
  Request.ValueType = MemberValueDescriptor<typename Shape::Declared>();
  Request.Write = MakeMemberWriter<Class, Setter>(std::move(Mutator));
  Request.Refusal = ClassifyPropertyPolicy(Policy, false, true);
  return Request;
}

template <class Class, class Value, class Setter>
[[nodiscard]] MemberRequest MakeWritableConvertedPropertyRequest(
    const StableTypeKey &Key, std::string_view Name,
    const PropertyPolicy &Policy, Setter Mutator) {
  using Shape = MemberWriteShape<Class, Setter>;
  static_assert(
      std::is_same_v<std::remove_cv_t<typename Shape::Declared>, Value>,
      "A converted property's named value type must match its setter's "
      "declared parameter type.");

  MemberRequest Request;
  Request.Kind = SymbolKind::Property;
  Request.Access = Policy.Access();
  Request.Evaluation = Policy.Evaluation();
  Request.ReceiverType = TypeDescriptor::ForClass(Key);
  Request.ValueType =
      MemberValueDescriptor<Value>(SyntheticConvertedValueKey(Key, Name));
  Request.ConvertedWrite =
      MakeMemberConvertedWriter<Class, Setter>(std::move(Mutator));
  Request.Refusal = ClassifyPropertyPolicy(Policy, false, true);
  return Request;
}

template <class Class, class Getter, class Setter>
[[nodiscard]] MemberRequest
MakePropertyRequest(const StableTypeKey &Key, const PropertyPolicy &Policy,
                    Getter Accessor, Setter Mutator,
                    OwnershipPolicy Ownership = UndeclaredOwnershipPolicy()) {
  using ReadShape = MemberReadShape<Class, Getter>;
  using Produced = typename ReadShape::Declared;

  MemberRequest Request;
  Request.Kind = SymbolKind::Property;
  Request.Access = Policy.Access();
  Request.Evaluation = Policy.Evaluation();
  Request.ReceiverType = TypeDescriptor::ForClass(Key);
  Request.ReadRequiresMutableReceiver = ReadShape::RequiresMutableReceiver;

  if constexpr (IsInstanceMemberValue<Produced>) {
    using WriteShape = MemberInstanceWriteShape<Class, Setter>;
    static_assert(WriteShape::IsSupported,
                  "A Luna instance property setter is a mutator of the class, "
                  "or a callable taking the class, whose one parameter names "
                  "the same registered class as an instance operand: T, "
                  "const T &, T &, T *, or const T *.");
    static_assert(
        std::is_same_v<typename InstanceReturnTrait<Produced>::Native,
                       typename WriteShape::Native>,
        "A Luna read-write instance property names one registered class for "
        "both its getter and its setter.");

    Request.ValueType = InstanceMemberValueDescriptor<Produced>();
    Request.InstanceRead =
        MakeMemberInstanceReader<Class, Getter>(std::move(Accessor), Ownership);
    Request.InstanceWrite =
        MakeMemberInstanceWriter<Class, Setter>(std::move(Mutator));
    Request.InstanceWriteRequiresMutation = WriteShape::RequiresMutation;
    Request.Refusal =
        ClassifyInstancePropertyPolicy<Produced>(Policy, Ownership, true);
  } else {
    static_cast<void>(Ownership);
    using WriteShape = MemberWriteShape<Class, Setter>;
    static_assert(std::is_same_v<Produced, typename WriteShape::Declared>,
                  "A Luna read-write property declares one value type for both "
                  "its getter and its setter.");

    Request.ValueType = MemberValueDescriptor<Produced>();
    Request.Read = MakeMemberReader<Class, Getter>(std::move(Accessor));
    Request.Write = MakeMemberWriter<Class, Setter>(std::move(Mutator));
    Request.Refusal = ClassifyPropertyPolicy(Policy, true, true);
  }
  return Request;
}

template <class Class, class Value, class Getter, class Setter>
[[nodiscard]] MemberRequest
MakeConvertedPropertyRequest(const StableTypeKey &Key, std::string_view Name,
                             const PropertyPolicy &Policy, Getter Accessor,
                             Setter Mutator) {
  using ReadShape = MemberReadShape<Class, Getter>;
  using WriteShape = MemberWriteShape<Class, Setter>;
  static_assert(std::is_same_v<typename ReadShape::Declared,
                               typename WriteShape::Declared>,
                "A Luna read-write property declares one value type for both "
                "its getter and its setter.");
  static_assert(
      std::is_same_v<std::remove_cv_t<typename ReadShape::Declared>, Value>,
      "A converted property's named value type must match its getter and "
      "setter's declared value type.");

  MemberRequest Request;
  Request.Kind = SymbolKind::Property;
  Request.Access = Policy.Access();
  Request.Evaluation = Policy.Evaluation();
  Request.ReceiverType = TypeDescriptor::ForClass(Key);
  Request.ValueType =
      MemberValueDescriptor<Value>(SyntheticConvertedValueKey(Key, Name));
  Request.ReadRequiresMutableReceiver = ReadShape::RequiresMutableReceiver;
  Request.ConvertedRead =
      MakeMemberConvertedReader<Class, Getter>(std::move(Accessor));
  Request.ConvertedWrite =
      MakeMemberConvertedWriter<Class, Setter>(std::move(Mutator));
  Request.Refusal = ClassifyPropertyPolicy(Policy, true, true);
  return Request;
}

template <class Class, class Getter, class Setter, class OnChange>
  requires(!std::is_same_v<std::decay_t<OnChange>, OwnershipPolicy>)
[[nodiscard]] MemberRequest
MakePropertyRequest(const StableTypeKey &Key, const PropertyPolicy &Policy,
                    Getter Accessor, Setter Mutator, OnChange Handler) {
  MemberRequest Request = MakePropertyRequest<Class, Getter, Setter>(
      Key, Policy, std::move(Accessor), std::move(Mutator));

  if constexpr (IsInstanceMemberValue<
                    typename MemberReadShape<Class, Getter>::Declared>) {
    static_cast<void>(Handler);
    Request.Refusal = std::string(InstanceMemberChangeRefusal);
  } else {
    using WriteShape = MemberWriteShape<Class, Setter>;
    Request.Change =
        MakeMemberChangeHandler<Class, typename WriteShape::Declared>(
            std::move(Handler));
  }
  return Request;
}

template <class Class, class Held>
[[nodiscard]] MemberRequest
MakeFieldRequest(const StableTypeKey &Key, const FieldPolicy &Policy,
                 Held Class::*Member,
                 OwnershipPolicy Ownership = UndeclaredOwnershipPolicy()) {
  using Pointer = Held Class::*;
  using ReadShape = MemberReadShape<Class, Pointer>;
  using Produced = typename ReadShape::Declared;

  MemberRequest Request;
  Request.Kind = SymbolKind::Field;
  Request.Evaluation = PropertyEvaluation::Immediate;
  Request.Ownership = Policy.Ownership();
  Request.ReceiverType = TypeDescriptor::ForClass(Key);

  if constexpr (IsInstanceMemberValue<Produced>) {
    Request.Access = MemberAccess::ReadOnly;
    Request.ValueType = InstanceMemberValueDescriptor<Produced>();
    Request.InstanceRead =
        MakeMemberInstanceReader<Class, Pointer>(Member, Ownership);
    Request.Refusal = ClassifyFieldPolicy(Policy);
    if (Request.Refusal.empty())
      Request.Refusal = ClassifyInstanceReturnPolicy<Produced>(Ownership);
    if (Request.Refusal.empty() && Policy.DeclaresDirection() &&
        Policy.PermitsWrite())
      Request.Refusal = std::string(WritableInstanceMemberRefusal);
    return Request;
  } else {
    static_cast<void>(Ownership);
    constexpr bool IsWritable =
        !std::is_const_v<Held> && MemberWriteShape<Class, Pointer>::IsSupported;

    Request.ValueType = MemberValueDescriptor<Produced>();
    Request.Read = MakeMemberReader<Class, Pointer>(Member);

    const bool PermitsWrite = Policy.PermitsWrite() && IsWritable;
    Request.Access =
        PermitsWrite ? MemberAccess::ReadWrite : MemberAccess::ReadOnly;
    if constexpr (IsWritable) {
      if (PermitsWrite)
        Request.Write = MakeMemberWriter<Class, Pointer>(Member);
    }

    Request.Refusal = ClassifyFieldPolicy(Policy);

    if (Request.Refusal.empty() && Policy.DeclaresDirection() &&
        Policy.PermitsWrite() && !IsWritable)
      Request.Refusal = "this field is declared const, so it can never be "
                        "written through.";
    return Request;
  }
}

template <class Class, class Held, class OnChange>
  requires(!std::is_same_v<std::decay_t<OnChange>, OwnershipPolicy>)
[[nodiscard]] MemberRequest
MakeFieldRequest(const StableTypeKey &Key, const FieldPolicy &Policy,
                 Held Class::*Member, OnChange Handler) {
  MemberRequest Request = MakeFieldRequest<Class, Held>(Key, Policy, Member);

  if constexpr (IsInstanceMemberValue<std::remove_cv_t<Held>>) {
    static_cast<void>(Handler);
    Request.Refusal = std::string(WritableInstanceMemberRefusal);
  } else if (Request.HasWriter()) {
    Request.Change = MakeMemberChangeHandler<Class, std::remove_cv_t<Held>>(
        std::move(Handler));
  }
  return Request;
}

template <class Class, class Value, class Held>
[[nodiscard]] MemberRequest
MakeConvertedFieldRequest(const StableTypeKey &Key, std::string_view Name,
                          const FieldPolicy &Policy, Held Class::*Member) {
  using Pointer = Held Class::*;
  constexpr bool IsWritable = !std::is_const_v<Held>;
  static_assert(std::is_same_v<std::remove_cv_t<Held>, Value>,
                "A converted field's named value type must match the "
                "declared data member's type.");

  MemberRequest Request;
  Request.Kind = SymbolKind::Field;
  Request.Evaluation = PropertyEvaluation::Immediate;
  Request.Ownership = Policy.Ownership();
  Request.ReceiverType = TypeDescriptor::ForClass(Key);
  Request.ValueType =
      MemberValueDescriptor<Value>(SyntheticConvertedValueKey(Key, Name));
  Request.ConvertedRead = MakeMemberConvertedReader<Class, Pointer>(Member);

  const bool PermitsWrite = Policy.PermitsWrite() && IsWritable;
  Request.Access =
      PermitsWrite ? MemberAccess::ReadWrite : MemberAccess::ReadOnly;
  if constexpr (IsWritable) {
    if (PermitsWrite)
      Request.ConvertedWrite =
          MakeMemberConvertedWriter<Class, Pointer>(Member);
  }

  Request.Refusal = ClassifyFieldPolicy(Policy);

  if (Request.Refusal.empty() && Policy.DeclaresDirection() &&
      Policy.PermitsWrite() && !IsWritable)
    Request.Refusal = "this field is declared const, so it can never be "
                      "written through.";
  return Request;
}

} // namespace Luna::Detail
