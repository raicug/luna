#pragma once

// Properties and fields as generated getter and setter descriptors.
//
// A property is not a second access path and a field is never raw memory. Each
// declared target - a const or non-const accessor, a mutator, or a plain data
// member - is turned here into exactly two erased descriptors over an already
// validated native object: one that produces a Luna-owned value and one that
// consumes one. The receiver, the origin State, the lifetime, the dynamic type,
// and const permission are all decided by the access gate before either
// descriptor runs, so a descriptor never validates anything a gate already
// decided and never observes a virtual-machine value.
//
// What the declaration states is what Luna enforces. A getter declared on a
// const object reads through a const view; a getter declared on a mutable
// object, a setter, and a writable field all require a mutable view and are
// refused against a const one before the target runs. A member whose declared
// value type Luna cannot copy across the boundary is refused transactionally
// rather than exposed with an ownership Luna would have to keep alive.

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

// One staged member of one class: which category it is, which directions and
// evaluation it declares, the canonical declared value type and receiver type,
// the two generated descriptors, and the first deterministic refusal the
// declaration itself recorded.
struct MemberRequest final {
  SymbolKind Kind = SymbolKind::Property;
  MemberAccess Access = MemberAccess::ReadOnly;
  PropertyEvaluation Evaluation = PropertyEvaluation::Immediate;
  MemberOwnership Ownership = MemberOwnership::Copied;

  TypeDescriptor ValueType;
  TypeDescriptor ReceiverType;

  // The declared getter reaches the object through a mutable receiver, so a
  // const view refuses even the read.
  bool ReadRequiresMutableReceiver = false;

  MemberReadOperation Read;
  MemberWriteOperation Write;

  std::string Refusal;

  [[nodiscard]] bool HasReader() const noexcept { return Read != nullptr; }
  [[nodiscard]] bool HasWriter() const noexcept { return Write != nullptr; }
};

// The read shape of one declared target: a const accessor, a non-const
// accessor, a plain data member, or any callable of shape `Value(Receiver)`.
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

// One callable of shape `Value(Receiver)`. The receiver decides whether the
// read needs a mutable view: a non-const reference does, a const reference or a
// by-value copy does not.
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

// The write shape of one declared target: a mutator, a plain data member, or
// any callable of shape `void(Class &, Value)`.
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

// The canonical declared value type of one member, or the unsupported
// descriptor when Luna cannot copy that type across the member boundary.
template <class Declared>
[[nodiscard]] inline TypeDescriptor MemberValueDescriptor() {
  if constexpr (SupportedValue<Declared>)
    return CanonicalDescriptorFor<Declared>();
  else
    return TypeDescriptor::Unsupported();
}

// One generated getter descriptor over an already validated native object.
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

// One generated setter descriptor over an already validated mutable object.
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

// The refusal one property policy earns when it contradicts the accessors the
// declaration supplied, or nothing when the two agree.
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

// The refusal one field policy earns when its ownership statement is one Luna
// could not honor across the member boundary.
[[nodiscard]] inline std::string
ClassifyFieldPolicy(const FieldPolicy &Policy) {
  if (!Policy.IsCoherent())
    return "a Luna field copies its declared value across the member "
           "boundary, so the ownership statement '" +
           std::string(MemberOwnershipText(Policy.Ownership())) +
           "' cannot be honored.";
  return std::string();
}

// One read-only or computed property over a single declared getter.
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

// One write-only property over a single declared setter.
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

// One read-write property over a declared getter and a declared setter. The two
// must agree on the canonical value type they carry, or the declaration is
// refused rather than reinterpreted.
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

// One field over a declared data member. A const-qualified data member is
// read-only whatever the policy states, because Luna would otherwise have to
// write through a declaration the class itself forbids.
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

  // A const data member is read-only whatever else is stated, so the default
  // policy is narrowed silently. Asking for writes explicitly is a description
  // mistake and is refused instead.
  if (Request.Refusal.empty() && Policy.DeclaresDirection() &&
      Policy.PermitsWrite() && !IsWritable)
    Request.Refusal = "this field is declared const, so it can never be "
                      "written through.";
  return Request;
}

} // namespace Luna::Detail
