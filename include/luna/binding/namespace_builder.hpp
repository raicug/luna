#pragma once

// clang-format off
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/constant_value.hpp>
#include <luna/binding/enum_builder.hpp>
#include <luna/binding/module_registration.hpp>
#include <luna/binding/supported_callable.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/detail/callable_adapter.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/type/stable_type_key.hpp>

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna {

namespace Detail {
class NamespaceBuilderState;
}

class NamespaceBuilder final {
public:
  NamespaceBuilder(const NamespaceBuilder &) = delete;
  NamespaceBuilder &operator=(const NamespaceBuilder &) = delete;
  NamespaceBuilder(NamespaceBuilder &&Other) noexcept;
  NamespaceBuilder &operator=(NamespaceBuilder &&Other) noexcept;

  ~NamespaceBuilder();

  [[nodiscard]] NamespaceBuilder RegisterNamespace(std::string_view Name);

  template <class Callable>
    requires SupportedCallable<Callable>
  NamespaceBuilder &RegisterFunction(std::string_view Name, Callable &&Target) {
    ErasedCallableDescriptor Descriptor =
        Detail::MakeErasedCallableDescriptor(std::forward<Callable>(Target));
    StageFunction(Name, std::move(Descriptor));
    return *this;
  }

  template <class ValueType>
  NamespaceBuilder &RegisterConstant(std::string_view Name,
                                     ValueType &&Constant) {
    StageConstant(
        Name, Detail::MakeConstantRequest(std::forward<ValueType>(Constant)));
    return *this;
  }

  template <class ValueType>
  NamespaceBuilder &RegisterConstant(std::string_view Name,
                                     ValueType &&Constant, StableTypeKey Key) {
    StageConstant(Name, Detail::MakeConstantRequest(
                            std::forward<ValueType>(Constant), std::move(Key)));
    return *this;
  }

  template <class Enum>
  [[nodiscard]] EnumBuilder<Enum> RegisterEnum(std::string_view Name,
                                               StableTypeKey Key) {
    return EnumBuilder<Enum>(Detail::StageEnumeration(
        Plan, Scope, Name, Key, Detail::EnumerationPolicyFor<Enum>()));
  }

  template <class Type>
  [[nodiscard]] ClassBuilder<Type> RegisterClass(std::string_view Name,
                                                 StableTypeKey Key) {
    // Recording the key against the C++ type is what lets a later
    // declaration name an instance of this class as an operand or a result
    // without restating the key.
    Detail::RecordClassKey<Type>(Key);
    Detail::ClassStaging Staged = Detail::StageClassDeclaration(
        Plan, Scope, Name, Key, Detail::ClassPolicyFor<Type>());
    return ClassBuilder<Type>(std::move(Staged), std::move(Key));
  }

  template <class Configure>
    requires ModuleConfiguration<Configure>
  NamespaceBuilder &RegisterModule(ModuleManifest Manifest,
                                   Configure &&Registration) {
    StageModule(std::move(Manifest),
                Detail::ModuleRegistration::Create(
                    std::forward<Configure>(Registration)));
    return *this;
  }

  NamespaceBuilder &Documentation(std::string_view Text) {
    StageDocumentation(std::string_view(), Text);
    return *this;
  }

  NamespaceBuilder &Documentation(std::string_view Member,
                                  std::string_view Text) {
    StageDocumentation(Member, Text);
    return *this;
  }

  NamespaceBuilder &Attribute(std::string_view Name,
                              std::string_view AttributeValue) {
    StageAttribute(std::string_view(), Name, AttributeValue);
    return *this;
  }

  NamespaceBuilder &Attribute(std::string_view Member, std::string_view Name,
                              std::string_view AttributeValue) {
    StageAttribute(Member, Name, AttributeValue);
    return *this;
  }

  NamespaceBuilder &Example(std::string_view Text) {
    StageExample(std::string_view(), Text);
    return *this;
  }

  NamespaceBuilder &Example(std::string_view Member, std::string_view Text) {
    StageExample(Member, Text);
    return *this;
  }

  [[nodiscard]] std::string_view QualifiedName() const noexcept;

  [[nodiscard]] RegistrationResult Commit();

private:
  friend class BindingRegistry;
  friend class Detail::NamespaceBuilderState;

  NamespaceBuilder(std::shared_ptr<Detail::NamespaceBuilderState> Plan,
                   std::size_t Scope) noexcept;

  void StageFunction(std::string_view Name,
                     ErasedCallableDescriptor Descriptor);

  void StageConstant(std::string_view Name, Detail::ConstantRequest Request);

  void StageModule(ModuleManifest Manifest,
                   Detail::ModuleRegistration Registration);

  void StageDocumentation(std::string_view Member, std::string_view Text);
  void StageAttribute(std::string_view Member, std::string_view Name,
                      std::string_view AttributeValue);
  void StageExample(std::string_view Member, std::string_view Text);

  std::shared_ptr<Detail::NamespaceBuilderState> Plan;
  std::size_t Scope = 0;
};

} // namespace Luna
