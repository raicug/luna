#pragma once

// One transaction-attached class builder.
//
// `BindingRegistry::RegisterClass` and `NamespaceBuilder::RegisterClass` accept
// one validated identifier segment plus the class's validated stable type key
// and return a builder that stages the whole class in the pending plan of its
// chain. Registration stages exactly three things for one class: its one
// canonical class type, its one class symbol, and the cached metatable identity
// that type owns in this logical State. Every operation stages metadata only:
// no class type, class symbol, metatable identity, documentation string, or
// attribute becomes visible before `Commit` submits the plan as one outermost
// registration transaction, and destroying an uncommitted builder has no
// virtual-machine effect at all.
//
// The class is described, never guessed. A user-defined leaf type is accepted
// only with an explicit validated `StableTypeKey`, and the declared C++ shape a
// consumer's translation unit still knows - storage size, alignment, and
// whether the type is destructible, abstract, polymorphic, copyable, and
// movable - is captured here, where the type is complete. The registration
// backend never sees the consumer's type, so this captured shape is what later
// lets Luna allocate, construct, and release userdata storage without ever
// guessing a layout.
//
// `Allocator` states the semantic storage protocol Luna obtains, and gives
// back, the storage of every value of this class it creates itself. It belongs
// to the class rather than to one declaration, so it applies to every
// constructor and by-value factory of the class whether it was stated before or
// after them.
//
// `Method` and `StaticMethod` stage the member candidates of the class. An
// instance method operates on one value of the class, and that value is rank
// position zero of every call it takes: it is validated - present, of this
// State, of this class, live, of the requested dynamic type, and mutable when
// the method mutates - before one ordinary argument is inspected. That is what
// makes `object:Method(args)`, `object.Method(object, args)`, and
// `Class.Method(object, args)` one call rather than three spellings that happen
// to agree, and what makes a dot call without a receiver fail as a receiver
// refusal instead of as a shifted argument. A static method declares no
// receiver at all and is therefore an ordinary callable of the class scope.
//
// `Constructor`, `Factory`, and `Singleton` stage construction candidates of
// the class. Each one is an ordinary callable candidate: it uses the same
// canonical overload grouping, the same optional, defaulted, and variadic
// parameter descriptors, the same conversion registry, the same transaction,
// and the same deterministic diagnostics as any other declaration. What they
// add is the result: exactly one value of this class, published only after
// native construction, ownership establishment, identity-cache insertion,
// metatable association, and protected return publication have all succeeded. A
// singleton accessor defaults to borrowed ownership; supplying an
// `OwnershipPolicy` that contradicts the accessor's declared result is refused
// transactionally.

// clang-format off
#include <luna/binding/class_construction.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/binding/class_member.hpp>
#include <luna/binding/class_operator.hpp>
#include <luna/binding/class_relationship.hpp>
#include <luna/detail/construction_adapter.hpp>
#include <luna/detail/method_adapter.hpp>
#include <luna/detail/property_adapter.hpp>
#include <luna/type/stable_type_key.hpp>

#include <cstddef>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
// clang-format on

namespace Luna {

class State;

namespace Detail {

class NamespaceBuilderState;

// The declared C++ shape of one class, captured once where the class type is
// still complete. Luna stores it with the registered class so allocation,
// construction, and release decisions never depend on a type the backend cannot
// see.
struct ClassPolicy final {
  std::size_t ByteCount = 0;
  std::size_t Alignment = 0;
  bool IsDestructible = false;
  bool IsAbstract = false;
  bool IsPolymorphic = false;
  bool IsCopyConstructible = false;
  bool IsMoveConstructible = false;
};

template <class Type>
[[nodiscard]] constexpr ClassPolicy ClassPolicyFor() noexcept {
  static_assert(sizeof(Type) > 0,
                "Luna class registration requires a complete class type.");

  ClassPolicy Policy;
  Policy.ByteCount = sizeof(Type);
  Policy.Alignment = alignof(Type);
  Policy.IsDestructible = std::is_destructible_v<Type>;
  Policy.IsAbstract = std::is_abstract_v<Type>;
  Policy.IsPolymorphic = std::is_polymorphic_v<Type>;
  Policy.IsCopyConstructible = std::is_copy_constructible_v<Type>;
  Policy.IsMoveConstructible = std::is_move_constructible_v<Type>;
  return Policy;
}

// The erased half of one class builder. It owns nothing but a reference to the
// shared pending plan and the staged class inside it, so the public builder
// template stays free of Luna's private registration model.
class ClassStaging final {
public:
  ClassStaging() noexcept;
  ClassStaging(std::shared_ptr<NamespaceBuilderState> Plan,
               std::size_t Node) noexcept;

  ClassStaging(const ClassStaging &) = delete;
  ClassStaging &operator=(const ClassStaging &) = delete;
  ClassStaging(ClassStaging &&Other) noexcept;
  ClassStaging &operator=(ClassStaging &&Other) noexcept;
  ~ClassStaging();

  // An empty member documents or annotates the class itself; otherwise the
  // named member, which must already be declared: a construction candidate, a
  // method, a property or field, or one Luna-owned operator segment.
  void StageDocumentation(std::string_view Member, std::string_view Text);
  void StageAttribute(std::string_view Member, std::string_view Name,
                      std::string_view AttributeValue);
  void StageExample(std::string_view Member, std::string_view Text);

  // The same three operations for one already declared operator of this class,
  // named by the operator it answers rather than by the Luna-owned segment it
  // is published under.
  void StageOperatorDocumentation(ClassOperator Selected,
                                  std::string_view Text);
  void StageOperatorAttribute(ClassOperator Selected, std::string_view Name,
                              std::string_view AttributeValue);
  void StageOperatorExample(ClassOperator Selected, std::string_view Text);

  // Stages one construction candidate of this class under `Name`. The candidate
  // joins the plan of this chain; nothing is installed, converted, or published
  // here.
  void StageConstruction(std::string_view Name, ConstructionRequest Request);

  // Stages one member candidate of this class under `Name`: one instance method
  // that operates on a value of the class, or one static method that does not.
  // Both join the plan of this chain as ordinary callable candidates; nothing
  // is installed, converted, or published here.
  void StageMember(std::string_view Name, MethodRequest Request);

  // Stages one typed accessor of this class under `Name`: one property with its
  // generated getter and setter descriptors, or one field with the generated
  // descriptors of the data member behind it. Neither one is a callable
  // candidate and neither one installs a virtual-machine value: a member is
  // reached through its class, so it becomes reachable only when the plan of
  // this chain is published.
  void StageAccessor(std::string_view Name, MemberRequest Request);

  // Stages the semantic storage protocol this class selects for every value
  // Luna creates of it. It is validated against the declared storage shape of
  // the class where the declaration is still known, and it decides the
  // reflected allocator policy identity of every creating candidate, whichever
  // order the declarations were made in.
  void StageAllocator(const ClassAllocator &Storage);

  // Stages one base edge, or one safe downcast policy, of this class. Neither
  // one is decided here: a relationship is accepted only as part of the whole
  // candidate graph of the attempt, so declaration order never changes the
  // outcome.
  void StageBase(BaseRequest Request);
  void StageCast(CastRequest Request);

  // Stages one operator of this class. It is an ordinary member candidate
  // published under the Luna-owned segment that operator names, so the operator
  // Luna answers is never a second dispatch path.
  void StageOperator(ClassOperator Selected, MethodRequest Request);

  [[nodiscard]] RegistrationResult Commit();
  [[nodiscard]] std::string_view QualifiedName() const noexcept;

private:
  std::shared_ptr<NamespaceBuilderState> Plan;
  std::size_t Node = 0;
};

// Stages one class inside a scope of an existing pending plan.
[[nodiscard]] ClassStaging
StageClassDeclaration(std::shared_ptr<NamespaceBuilderState> Plan,
                      std::size_t ScopeNode, std::string_view Name,
                      const StableTypeKey &Key, const ClassPolicy &Policy);

// Stages one root-scope class in a new pending plan of its own.
[[nodiscard]] ClassStaging StageRootClassDeclaration(State &Owner,
                                                     std::string_view Name,
                                                     const StableTypeKey &Key,
                                                     const ClassPolicy &Policy);

} // namespace Detail

template <class Type> class ClassBuilder final {
  static_assert(std::is_class_v<Type>,
                "Luna class registration requires a class or struct type.");
  static_assert(!std::is_const_v<Type> && !std::is_volatile_v<Type>,
                "Luna class registration requires an unqualified class type; "
                "constness is a property of one exposed value, not of the "
                "registered class.");

public:
  using Class = Type;

  ClassBuilder(const ClassBuilder &) = delete;
  ClassBuilder &operator=(const ClassBuilder &) = delete;
  ClassBuilder(ClassBuilder &&Other) noexcept = default;
  ClassBuilder &operator=(ClassBuilder &&Other) noexcept = default;

  // Destroying an uncommitted builder discards its staged class without
  // touching the virtual machine, reflection, or dispatch.
  ~ClassBuilder() = default;

  // Stages one constructor of this class under Luna's default constructor name.
  // Several constructors of one class share that name and form one canonical
  // overload set; two whose declared parameter shapes no call could tell apart
  // are refused transactionally.
  template <class... Arguments> ClassBuilder &Constructor() {
    return Constructor<Arguments...>(Detail::DefaultConstructorName);
  }

  // Stages one constructor of this class under an explicit name.
  template <class... Arguments>
  ClassBuilder &Constructor(std::string_view Name) {
    Detail::ConstructionRequest Request =
        Detail::MakeConstructorRequest<Type, Arguments...>(ClassKey, Storage);
    Staging.StageConstruction(Name, std::move(Request));
    return *this;
  }

  // Stages one factory of this class. Returning the class by value states Lua
  // ownership of the produced object; returning `std::shared_ptr<Type>` states
  // shared ownership of exactly one reference.
  template <class Target>
  ClassBuilder &Factory(std::string_view Name, Target Producer) {
    Detail::ConstructionRequest Request = Detail::MakeFactoryRequest<Type>(
        ClassKey, std::move(Producer), Storage);
    Staging.StageConstruction(Name, std::move(Request));
    return *this;
  }

  // Selects the semantic storage protocol Luna obtains, and gives back, the
  // storage of every value of this class it creates itself: the objects of its
  // constructors and of its by-value factories.
  //
  // The selection belongs to the class rather than to one declaration, so it
  // may be stated before or after those candidates are declared and still
  // applies to all of them; stating it twice keeps the last protocol named. A
  // candidate whose object Luna never allocated - a shared factory, a borrowed
  // singleton accessor - is unaffected, because Luna owns no storage of it to
  // obtain.
  //
  // The protocol must be one Luna could actually create and release a value
  // through: it declares allocation, destruction, and deallocation, names a
  // policy identity to reflect, and covers the declared storage shape of this
  // class. Anything else is refused transactionally rather than reinterpreted.
  ClassBuilder &Allocator(ClassAllocator Selected) {
    Staging.StageAllocator(Selected);
    if (Storage)
      *Storage = std::move(Selected);
    return *this;
  }

  // Stages one singleton accessor of this class under the default policy:
  // borrowed ownership with one Luna-owned lifetime that stays live for as long
  // as this registration.
  template <class Target>
  ClassBuilder &Singleton(std::string_view Name, Target Accessor) {
    OwnershipPolicy Default;
    return Singleton(Name, std::move(Accessor), std::move(Default));
  }

  // Stages one singleton accessor under an explicit ownership policy. The
  // policy must be compatible with the accessor's declared result.
  template <class Target>
  ClassBuilder &Singleton(std::string_view Name, Target Accessor,
                          OwnershipPolicy Policy) {
    Detail::ConstructionRequest Request = Detail::MakeSingletonRequest<Type>(
        ClassKey, std::move(Accessor), std::move(Policy));
    Staging.StageConstruction(Name, std::move(Request));
    return *this;
  }

  // Stages one instance method of this class under `Name`.
  //
  // The target states the object the method operates on rather than having it
  // guessed: a member function pointer of this class - or of one of its bases -
  // states it through its own class and const qualification, and an explicit
  // wrapper states it through a first parameter of `Type &`, `const Type &`,
  // `Type *`, or `const Type *`. A const method may be called through a const
  // value of the class; a non-const one may not.
  //
  // The receiver is rank position zero of the call, so `object:Method(args)`,
  // `object.Method(object, args)`, and `Class.Method(object, args)` are one
  // call: the receiver's presence, origin State, lifetime, dynamic type,
  // metatable identity, and const access are all validated before any ordinary
  // argument is even inspected. A virtual method dispatches through the object
  // the call site supplied.
  //
  // Several methods declared under one name form one canonical overload set,
  // resolved by exactly the rules every other overload set follows; two whose
  // declared shapes no call could tell apart are refused transactionally.
  template <class Target>
  ClassBuilder &Method(std::string_view Name, Target Selected) {
    Detail::MethodRequest Request =
        Detail::MakeMethodRequest<Type>(ClassKey, std::move(Selected));
    Staging.StageMember(Name, std::move(Request));
    return *this;
  }

  // Stages one static method of this class under `Name`. It declares no
  // receiver, so it is an ordinary callable candidate of the class scope and
  // uses the ordinary overload, conversion, and diagnostic pipeline; a dot call
  // reaches it without any instance at all.
  template <class Target>
  ClassBuilder &StaticMethod(std::string_view Name, Target Selected) {
    Detail::MethodRequest Request =
        Detail::MakeStaticMethodRequest(std::move(Selected));
    Staging.StageMember(Name, std::move(Request));
    return *this;
  }

  // Stages one read-only property of this class under `Name`, over a single
  // declared getter.
  //
  // The getter states the object it reads rather than having it guessed: a
  // const member function of the class reads through a const view, a non-const
  // member function requires a mutable one, a data member of the class is read
  // as the value it holds, and an explicit callable states it through a first
  // parameter of `const Type &`, `Type &`, or `Type`. Luna generates the getter
  // descriptor from that declaration and reaches the object only through the
  // validated access gate, so the receiver's presence, origin State, lifetime,
  // dynamic type, metatable identity, and const permission are all decided
  // before the declared getter ever runs.
  template <class Getter>
    requires(!std::is_same_v<std::decay_t<Getter>, PropertyPolicy>)
  ClassBuilder &Property(std::string_view Name, Getter Accessor) {
    const PropertyPolicy Policy = PropertyPolicy::ReadOnly();
    Detail::MemberRequest Request =
        Detail::MakeReadablePropertyRequest<Type, Getter>(ClassKey, Policy,
                                                          std::move(Accessor));
    Staging.StageAccessor(Name, std::move(Request));
    return *this;
  }

  // Stages one read-write property of this class under `Name`, over a declared
  // getter and a declared setter. Both must carry the same declared value type,
  // and the setter always requires a mutable view, so a const value of the
  // class permits the read and refuses the write before native code.
  template <class Getter, class Setter>
    requires(!std::is_same_v<std::decay_t<Getter>, PropertyPolicy>)
  ClassBuilder &Property(std::string_view Name, Getter Accessor,
                         Setter Mutator) {
    const PropertyPolicy Policy = PropertyPolicy::ReadWrite();
    Detail::MemberRequest Request =
        Detail::MakePropertyRequest<Type, Getter, Setter>(
            ClassKey, Policy, std::move(Accessor), std::move(Mutator));
    Staging.StageAccessor(Name, std::move(Request));
    return *this;
  }

  // Stages one property of this class under an explicit policy and a single
  // declared accessor. A policy that permits reads takes a getter and a policy
  // that permits writes takes a setter, so a write-only property, a computed
  // property, and an explicitly lazy property are all stated rather than
  // inferred. A policy that contradicts the accessor it was given is refused
  // transactionally.
  template <class Accessor>
  ClassBuilder &Property(std::string_view Name, PropertyPolicy Policy,
                         Accessor Target) {
    if constexpr (Detail::MemberReadShape<Type, Accessor>::IsSupported) {
      Detail::MemberRequest Request =
          Detail::MakeReadablePropertyRequest<Type, Accessor>(
              ClassKey, Policy, std::move(Target));
      Staging.StageAccessor(Name, std::move(Request));
    } else {
      Detail::MemberRequest Request =
          Detail::MakeWritablePropertyRequest<Type, Accessor>(
              ClassKey, Policy, std::move(Target));
      Staging.StageAccessor(Name, std::move(Request));
    }
    return *this;
  }

  // Stages one property of this class under an explicit policy, a declared
  // getter, and a declared setter. This is how an explicitly lazy read-write
  // property is declared: the getter's value is reused until a successful
  // write, an explicit invalidation, or a dispatch-generation change ends its
  // validity.
  template <class Getter, class Setter>
  ClassBuilder &Property(std::string_view Name, PropertyPolicy Policy,
                         Getter Accessor, Setter Mutator) {
    Detail::MemberRequest Request =
        Detail::MakePropertyRequest<Type, Getter, Setter>(
            ClassKey, Policy, std::move(Accessor), std::move(Mutator));
    Staging.StageAccessor(Name, std::move(Request));
    return *this;
  }

  // Stages one field of this class under `Name`, over one declared data member.
  //
  // A field is never raw virtual-machine memory access: Luna generates the same
  // getter and setter descriptors a property uses, so a field obeys its
  // declared type, its constness, and its ownership restriction exactly like
  // every other member. The declared value is copied across the boundary in
  // both directions, so no reference into an object Luna does not own can
  // escape through it, and a const-qualified data member is read-only whatever
  // else is stated.
  template <class Held>
  ClassBuilder &Field(std::string_view Name, Held Type::*Member) {
    const FieldPolicy Policy;
    Detail::MemberRequest Request =
        Detail::MakeFieldRequest<Type, Held>(ClassKey, Policy, Member);
    Staging.StageAccessor(Name, std::move(Request));
    return *this;
  }

  // Stages one field of this class under an explicit policy: a read-only field,
  // or an explicit ownership statement. Only a copied field is honored, because
  // that is the only ownership Luna can promise across the member boundary.
  template <class Held>
  ClassBuilder &Field(std::string_view Name, Held Type::*Member,
                      FieldPolicy Policy) {
    Detail::MemberRequest Request =
        Detail::MakeFieldRequest<Type, Held>(ClassKey, Policy, Member);
    Staging.StageAccessor(Name, std::move(Request));
    return *this;
  }

  // Stages one explicit base edge of this class, from this class to the already
  // registered class `BaseKey` names.
  //
  // The relationship is described rather than discovered: whether `BaseType`
  // really is a base of this class, whether it is reachable through one
  // unambiguous public path, and how a pointer to this class is adjusted to it
  // are all captured here, where both types are still complete. The edge is
  // then accepted only as part of the whole class relationship graph of the
  // attempt, so a base that is not registered, an inaccessible base, a
  // duplicate edge, an edge that would close a cycle, and any pair of classes
  // reachable through more than one path are all refused transactionally.
  //
  // One registered accessible path is what permits a value of this class to be
  // received as a receiver or an argument of that base; nothing else does.
  template <class BaseType> ClassBuilder &Base(const StableTypeKey &BaseKey) {
    Staging.StageBase(Detail::MakeBaseRequest<Type, BaseType>(BaseKey));
    return *this;
  }

  // Stages the safe downcast policy that permits a value exposed as the class
  // `SourceKey` names to be received as this class, deciding compatibility with
  // internal runtime type assistance.
  //
  // A downcast is never implicit: without this declaration a base value is
  // simply not a value of this class. The compatibility check the policy
  // performs is non-mutating and runs before any conversion is committed and
  // before any native target is invoked, so an incompatible object is refused
  // rather than reinterpreted. The assistance stays internal: no persistent
  // identity of this class derives from a runtime type name or address.
  template <class SourceType>
  ClassBuilder &Cast(const StableTypeKey &SourceKey) {
    Staging.StageCast(
        Detail::MakeRuntimeTypeCastRequest<Type, SourceType>(SourceKey));
    return *this;
  }

  // Stages the safe downcast policy under an explicit policy identity and an
  // explicit declared compatibility check. The check receives the object as a
  // `const SourceType &`, reports whether it really is a value of this class,
  // and must be stateless and free of side effects; a check that refuses is an
  // ordinary incompatible object.
  template <class SourceType, class Check>
  ClassBuilder &Cast(const StableTypeKey &SourceKey,
                     std::string_view PolicyIdentity, Check Compatible) {
    static_cast<void>(Compatible);
    Staging.StageCast(Detail::MakeCheckedCastRequest<Type, SourceType, Check>(
        SourceKey, PolicyIdentity));
    return *this;
  }

  // Stages one operator of this class.
  //
  // The target states the object the operator operates on exactly the way an
  // instance method does - a member function pointer of the class, or an
  // explicit wrapper whose first parameter is a reference or pointer to it -
  // and every operand after that receiver is an ordinary parameter. The
  // receiver is therefore rank position zero of every operator call: it is
  // validated before one operand is inspected, and the candidate resolves
  // through the same canonical overload rules every other member follows.
  //
  // The operator is described rather than guessed: the operand count of the
  // selected operator is fixed, and a declaration that takes a different number
  // of them - or takes one optionally - is refused transactionally. A call
  // operator is the exception, because it forwards whatever the call site
  // supplied.
  //
  // Indexing and assignment keep Luna's own metamethods: the declaration is the
  // behaviour Luna consults for a name the class declares nothing for, never a
  // replacement of Luna's reserved dispatch.
  template <class Target>
  ClassBuilder &Operator(ClassOperator Selected, Target Chosen) {
    Detail::MethodRequest Request =
        Detail::MakeMethodRequest<Type>(ClassKey, std::move(Chosen));
    Staging.StageOperator(Selected, std::move(Request));
    return *this;
  }

  // Documents the class itself.
  ClassBuilder &Documentation(std::string_view Text) {
    Staging.StageDocumentation(std::string_view(), Text);
    return *this;
  }

  // Documents one already declared member of this class: a construction
  // candidate, a method, a property or field, or one operator segment.
  ClassBuilder &Documentation(std::string_view Member, std::string_view Text) {
    Staging.StageDocumentation(Member, Text);
    return *this;
  }

  // Annotates the class itself.
  ClassBuilder &Attribute(std::string_view Name,
                          std::string_view AttributeValue) {
    Staging.StageAttribute(std::string_view(), Name, AttributeValue);
    return *this;
  }

  // Annotates one already declared member of this class.
  ClassBuilder &Attribute(std::string_view Member, std::string_view Name,
                          std::string_view AttributeValue) {
    Staging.StageAttribute(Member, Name, AttributeValue);
    return *this;
  }

  // Adds one usage example to the class itself. Examples are reflected in
  // declaration order, so generated material repeats them exactly as declared.
  ClassBuilder &Example(std::string_view Text) {
    Staging.StageExample(std::string_view(), Text);
    return *this;
  }

  // Adds one usage example to one already declared member of this class.
  ClassBuilder &Example(std::string_view Member, std::string_view Text) {
    Staging.StageExample(Member, Text);
    return *this;
  }

  // Documents, annotates, and exemplifies one already declared operator of this
  // class. The operator is named by what it answers, so a consumer never spells
  // the Luna-owned segment it is published under.
  ClassBuilder &Documentation(ClassOperator Selected, std::string_view Text) {
    Staging.StageOperatorDocumentation(Selected, Text);
    return *this;
  }

  ClassBuilder &Attribute(ClassOperator Selected, std::string_view Name,
                          std::string_view AttributeValue) {
    Staging.StageOperatorAttribute(Selected, Name, AttributeValue);
    return *this;
  }

  ClassBuilder &Example(ClassOperator Selected, std::string_view Text) {
    Staging.StageOperatorExample(Selected, Text);
    return *this;
  }

  // The canonical `.`-separated qualified name of this class.
  [[nodiscard]] std::string_view QualifiedName() const noexcept {
    return Staging.QualifiedName();
  }

  // Submits the whole pending plan of this builder chain as one outermost
  // registration transaction.
  [[nodiscard]] RegistrationResult Commit() { return Staging.Commit(); }

private:
  friend class BindingRegistry;
  friend class NamespaceBuilder;

  ClassBuilder(Detail::ClassStaging Staged, StableTypeKey Key)
      : Staging(std::move(Staged)), ClassKey(std::move(Key)),
        Storage(Detail::MakeStorageSelection()) {}

  Detail::ClassStaging Staging;

  // The validated stable key of this class. A construction candidate names it
  // as its return shape, so publication resolves the class through the
  // canonical type registry rather than through anything the target returned.
  StableTypeKey ClassKey;

  // The one storage protocol every value of this class Luna creates is
  // allocated from. Every creating candidate shares it and reads it where it
  // creates its object, which is what makes `Allocator` independent of
  // declaration order.
  Detail::StorageSelection Storage;
};

} // namespace Luna
