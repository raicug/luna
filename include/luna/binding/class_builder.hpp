#pragma once

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

  void StageDocumentation(std::string_view Member, std::string_view Text);
  void StageAttribute(std::string_view Member, std::string_view Name,
                      std::string_view AttributeValue);
  void StageExample(std::string_view Member, std::string_view Text);

  void StageOperatorDocumentation(ClassOperator Selected,
                                  std::string_view Text);
  void StageOperatorAttribute(ClassOperator Selected, std::string_view Name,
                              std::string_view AttributeValue);
  void StageOperatorExample(ClassOperator Selected, std::string_view Text);

  void StageConstruction(std::string_view Name, ConstructionRequest Request);

  void StageMember(std::string_view Name, MethodRequest Request);

  void StageAccessor(std::string_view Name, MemberRequest Request);

  void StageAllocator(const ClassAllocator &Storage);

  void StageBase(BaseRequest Request);
  void StageCast(CastRequest Request);

  void StageOperator(ClassOperator Selected, MethodRequest Request);

  [[nodiscard]] RegistrationResult Commit();
  [[nodiscard]] std::string_view QualifiedName() const noexcept;

private:
  std::shared_ptr<NamespaceBuilderState> Plan;
  std::size_t Node = 0;
};

[[nodiscard]] ClassStaging
StageClassDeclaration(std::shared_ptr<NamespaceBuilderState> Plan,
                      std::size_t ScopeNode, std::string_view Name,
                      const StableTypeKey &Key, const ClassPolicy &Policy);

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

  ~ClassBuilder() = default;

  template <class... Arguments> ClassBuilder &Constructor() {
    return Constructor<Arguments...>(Detail::DefaultConstructorName);
  }

  template <class... Arguments>
  ClassBuilder &Constructor(std::string_view Name) {
    Detail::ConstructionRequest Request =
        Detail::MakeConstructorRequest<Type, Arguments...>(ClassKey, Storage);
    Staging.StageConstruction(Name, std::move(Request));
    return *this;
  }

  template <class Target>
  ClassBuilder &Factory(std::string_view Name, Target Producer) {
    Detail::ConstructionRequest Request = Detail::MakeFactoryRequest<Type>(
        ClassKey, std::move(Producer), Storage);
    Staging.StageConstruction(Name, std::move(Request));
    return *this;
  }

  ClassBuilder &Allocator(ClassAllocator Selected) {
    Staging.StageAllocator(Selected);
    if (Storage)
      *Storage = std::move(Selected);
    return *this;
  }

  template <class Target>
  ClassBuilder &Singleton(std::string_view Name, Target Accessor) {
    OwnershipPolicy Default;
    return Singleton(Name, std::move(Accessor), std::move(Default));
  }

  template <class Target>
  ClassBuilder &Singleton(std::string_view Name, Target Accessor,
                          OwnershipPolicy Policy) {
    Detail::ConstructionRequest Request = Detail::MakeSingletonRequest<Type>(
        ClassKey, std::move(Accessor), std::move(Policy));
    Staging.StageConstruction(Name, std::move(Request));
    return *this;
  }

  template <class Target>
  ClassBuilder &Method(std::string_view Name, Target Selected) {
    Detail::MethodRequest Request =
        Detail::MakeMethodRequest<Type>(ClassKey, std::move(Selected));
    Staging.StageMember(Name, std::move(Request));
    return *this;
  }

  template <class Target>
  ClassBuilder &StaticMethod(std::string_view Name, Target Selected) {
    Detail::MethodRequest Request =
        Detail::MakeStaticMethodRequest(std::move(Selected));
    Staging.StageMember(Name, std::move(Request));
    return *this;
  }

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

  template <class Getter, class Setter>
  ClassBuilder &Property(std::string_view Name, PropertyPolicy Policy,
                         Getter Accessor, Setter Mutator) {
    Detail::MemberRequest Request =
        Detail::MakePropertyRequest<Type, Getter, Setter>(
            ClassKey, Policy, std::move(Accessor), std::move(Mutator));
    Staging.StageAccessor(Name, std::move(Request));
    return *this;
  }

  template <class Held>
  ClassBuilder &Field(std::string_view Name, Held Type::*Member) {
    const FieldPolicy Policy;
    Detail::MemberRequest Request =
        Detail::MakeFieldRequest<Type, Held>(ClassKey, Policy, Member);
    Staging.StageAccessor(Name, std::move(Request));
    return *this;
  }

  template <class Held>
  ClassBuilder &Field(std::string_view Name, Held Type::*Member,
                      FieldPolicy Policy) {
    Detail::MemberRequest Request =
        Detail::MakeFieldRequest<Type, Held>(ClassKey, Policy, Member);
    Staging.StageAccessor(Name, std::move(Request));
    return *this;
  }

  template <class BaseType> ClassBuilder &Base(const StableTypeKey &BaseKey) {
    Staging.StageBase(Detail::MakeBaseRequest<Type, BaseType>(BaseKey));
    return *this;
  }

  template <class SourceType>
  ClassBuilder &Cast(const StableTypeKey &SourceKey) {
    Staging.StageCast(
        Detail::MakeRuntimeTypeCastRequest<Type, SourceType>(SourceKey));
    return *this;
  }

  template <class SourceType, class Check>
  ClassBuilder &Cast(const StableTypeKey &SourceKey,
                     std::string_view PolicyIdentity, Check Compatible) {
    static_cast<void>(Compatible);
    Staging.StageCast(Detail::MakeCheckedCastRequest<Type, SourceType, Check>(
        SourceKey, PolicyIdentity));
    return *this;
  }

  template <class Target>
  ClassBuilder &Operator(ClassOperator Selected, Target Chosen) {
    Detail::MethodRequest Request =
        Detail::MakeMethodRequest<Type>(ClassKey, std::move(Chosen));
    Staging.StageOperator(Selected, std::move(Request));
    return *this;
  }

  ClassBuilder &Documentation(std::string_view Text) {
    Staging.StageDocumentation(std::string_view(), Text);
    return *this;
  }

  ClassBuilder &Documentation(std::string_view Member, std::string_view Text) {
    Staging.StageDocumentation(Member, Text);
    return *this;
  }

  ClassBuilder &Attribute(std::string_view Name,
                          std::string_view AttributeValue) {
    Staging.StageAttribute(std::string_view(), Name, AttributeValue);
    return *this;
  }

  ClassBuilder &Attribute(std::string_view Member, std::string_view Name,
                          std::string_view AttributeValue) {
    Staging.StageAttribute(Member, Name, AttributeValue);
    return *this;
  }

  ClassBuilder &Example(std::string_view Text) {
    Staging.StageExample(std::string_view(), Text);
    return *this;
  }

  ClassBuilder &Example(std::string_view Member, std::string_view Text) {
    Staging.StageExample(Member, Text);
    return *this;
  }

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

  [[nodiscard]] std::string_view QualifiedName() const noexcept {
    return Staging.QualifiedName();
  }

  [[nodiscard]] RegistrationResult Commit() { return Staging.Commit(); }

private:
  friend class BindingRegistry;
  friend class NamespaceBuilder;

  ClassBuilder(Detail::ClassStaging Staged, StableTypeKey Key)
      : Staging(std::move(Staged)), ClassKey(std::move(Key)),
        Storage(Detail::MakeStorageSelection()) {}

  Detail::ClassStaging Staging;

  StableTypeKey ClassKey;

  Detail::StorageSelection Storage;
};

} // namespace Luna
