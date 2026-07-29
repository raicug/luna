#pragma once

// clang-format off
#include <luna/binding/class_member.hpp>
#include <luna/binding/supported_callable.hpp>
#include <luna/binding/value.hpp>
#include <luna/detail/canonical_type.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

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

  // Set instead of Read/Write when the declared value type is not one of
  // the four foundation scalars: a getter or setter of a consumer type with
  // its own `Luna::TypeConverter<T>` specialization.
  MemberConvertedReadOperation ConvertedRead;
  MemberConvertedWriteOperation ConvertedWrite;

  std::string Refusal;

  [[nodiscard]] bool HasReader() const noexcept {
    return Read != nullptr || ConvertedRead != nullptr;
  }
  [[nodiscard]] bool HasWriter() const noexcept {
    return Write != nullptr || ConvertedWrite != nullptr;
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

template <class Declared>
inline constexpr bool IsConvertedMemberValue =
    !SupportedValue<Declared> && std::is_class_v<Declared> &&
    ConversionCapable<Declared>;

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
[[nodiscard]] MemberRequest
MakeReadablePropertyRequest(const StableTypeKey &Key,
                            const PropertyPolicy &Policy, Getter Accessor) {
  using Shape = MemberReadShape<Class, Getter>;

  MemberRequest Request;
  Request.Kind = SymbolKind::Property;
  Request.Access = Policy.Access();
  Request.Evaluation = Policy.Evaluation();
  Request.ReceiverType = TypeDescriptor::ForClass(Key);
  Request.ValueType = MemberValueDescriptor<typename Shape::Declared>();
  Request.ReadRequiresMutableReceiver = Shape::RequiresMutableReceiver;
  Request.Read = MakeMemberReader<Class, Getter>(std::move(Accessor));
  Request.Refusal = ClassifyPropertyPolicy(Policy, true, false);
  return Request;
}

// A property or field whose declared value type is not one of the four
// foundation scalars converts through the consumer's own
// `Luna::TypeConverter<T>` specialization instead. These converted-value
// builders take an explicit `StableTypeKey` for that value type, the same
// way `Base<T>`/`Cast<T>` take one for a related class.
template <class Class, class Getter>
[[nodiscard]] MemberRequest MakeReadableConvertedPropertyRequest(
    const StableTypeKey &Key, const StableTypeKey &ValueKey,
    const PropertyPolicy &Policy, Getter Accessor) {
  using Shape = MemberReadShape<Class, Getter>;

  MemberRequest Request;
  Request.Kind = SymbolKind::Property;
  Request.Access = Policy.Access();
  Request.Evaluation = Policy.Evaluation();
  Request.ReceiverType = TypeDescriptor::ForClass(Key);
  Request.ValueType = MemberValueDescriptor<typename Shape::Declared>(ValueKey);
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

template <class Class, class Setter>
[[nodiscard]] MemberRequest MakeWritableConvertedPropertyRequest(
    const StableTypeKey &Key, const StableTypeKey &ValueKey,
    const PropertyPolicy &Policy, Setter Mutator) {
  using Shape = MemberWriteShape<Class, Setter>;

  MemberRequest Request;
  Request.Kind = SymbolKind::Property;
  Request.Access = Policy.Access();
  Request.Evaluation = Policy.Evaluation();
  Request.ReceiverType = TypeDescriptor::ForClass(Key);
  Request.ValueType = MemberValueDescriptor<typename Shape::Declared>(ValueKey);
  Request.ConvertedWrite =
      MakeMemberConvertedWriter<Class, Setter>(std::move(Mutator));
  Request.Refusal = ClassifyPropertyPolicy(Policy, false, true);
  return Request;
}

template <class Class, class Getter, class Setter>
[[nodiscard]] MemberRequest
MakePropertyRequest(const StableTypeKey &Key, const PropertyPolicy &Policy,
                    Getter Accessor, Setter Mutator) {
  using ReadShape = MemberReadShape<Class, Getter>;
  using WriteShape = MemberWriteShape<Class, Setter>;
  static_assert(std::is_same_v<typename ReadShape::Declared,
                               typename WriteShape::Declared>,
                "A Luna read-write property declares one value type for both "
                "its getter and its setter.");

  MemberRequest Request;
  Request.Kind = SymbolKind::Property;
  Request.Access = Policy.Access();
  Request.Evaluation = Policy.Evaluation();
  Request.ReceiverType = TypeDescriptor::ForClass(Key);
  Request.ValueType = MemberValueDescriptor<typename ReadShape::Declared>();
  Request.ReadRequiresMutableReceiver = ReadShape::RequiresMutableReceiver;
  Request.Read = MakeMemberReader<Class, Getter>(std::move(Accessor));
  Request.Write = MakeMemberWriter<Class, Setter>(std::move(Mutator));
  Request.Refusal = ClassifyPropertyPolicy(Policy, true, true);
  return Request;
}

template <class Class, class Getter, class Setter>
[[nodiscard]] MemberRequest MakeConvertedPropertyRequest(
    const StableTypeKey &Key, const StableTypeKey &ValueKey,
    const PropertyPolicy &Policy, Getter Accessor, Setter Mutator) {
  using ReadShape = MemberReadShape<Class, Getter>;
  using WriteShape = MemberWriteShape<Class, Setter>;
  static_assert(std::is_same_v<typename ReadShape::Declared,
                               typename WriteShape::Declared>,
                "A Luna read-write property declares one value type for both "
                "its getter and its setter.");

  MemberRequest Request;
  Request.Kind = SymbolKind::Property;
  Request.Access = Policy.Access();
  Request.Evaluation = Policy.Evaluation();
  Request.ReceiverType = TypeDescriptor::ForClass(Key);
  Request.ValueType =
      MemberValueDescriptor<typename ReadShape::Declared>(ValueKey);
  Request.ReadRequiresMutableReceiver = ReadShape::RequiresMutableReceiver;
  Request.ConvertedRead =
      MakeMemberConvertedReader<Class, Getter>(std::move(Accessor));
  Request.ConvertedWrite =
      MakeMemberConvertedWriter<Class, Setter>(std::move(Mutator));
  Request.Refusal = ClassifyPropertyPolicy(Policy, true, true);
  return Request;
}

template <class Class, class Getter, class Setter, class OnChange>
[[nodiscard]] MemberRequest
MakePropertyRequest(const StableTypeKey &Key, const PropertyPolicy &Policy,
                    Getter Accessor, Setter Mutator, OnChange Handler) {
  using WriteShape = MemberWriteShape<Class, Setter>;

  MemberRequest Request = MakePropertyRequest<Class, Getter, Setter>(
      Key, Policy, std::move(Accessor), std::move(Mutator));
  Request.Change =
      MakeMemberChangeHandler<Class, typename WriteShape::Declared>(
          std::move(Handler));
  return Request;
}

template <class Class, class Held>
[[nodiscard]] MemberRequest MakeFieldRequest(const StableTypeKey &Key,
                                             const FieldPolicy &Policy,
                                             Held Class::*Member) {
  using Pointer = Held Class::*;
  using ReadShape = MemberReadShape<Class, Pointer>;
  constexpr bool IsWritable =
      !std::is_const_v<Held> && MemberWriteShape<Class, Pointer>::IsSupported;

  MemberRequest Request;
  Request.Kind = SymbolKind::Field;
  Request.Evaluation = PropertyEvaluation::Immediate;
  Request.Ownership = Policy.Ownership();
  Request.ReceiverType = TypeDescriptor::ForClass(Key);
  Request.ValueType = MemberValueDescriptor<typename ReadShape::Declared>();
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

template <class Class, class Held, class OnChange>
[[nodiscard]] MemberRequest
MakeFieldRequest(const StableTypeKey &Key, const FieldPolicy &Policy,
                 Held Class::*Member, OnChange Handler) {
  MemberRequest Request = MakeFieldRequest<Class, Held>(Key, Policy, Member);
  if (Request.HasWriter())
    Request.Change = MakeMemberChangeHandler<Class, std::remove_cv_t<Held>>(
        std::move(Handler));
  return Request;
}

template <class Class, class Held>
[[nodiscard]] MemberRequest
MakeConvertedFieldRequest(const StableTypeKey &Key,
                          const StableTypeKey &ValueKey,
                          const FieldPolicy &Policy, Held Class::*Member) {
  using Pointer = Held Class::*;
  constexpr bool IsWritable = !std::is_const_v<Held>;

  MemberRequest Request;
  Request.Kind = SymbolKind::Field;
  Request.Evaluation = PropertyEvaluation::Immediate;
  Request.Ownership = Policy.Ownership();
  Request.ReceiverType = TypeDescriptor::ForClass(Key);
  Request.ValueType = MemberValueDescriptor<std::remove_cv_t<Held>>(ValueKey);
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
