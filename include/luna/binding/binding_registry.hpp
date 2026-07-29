#pragma once

// clang-format off
#include <luna/binding/class_builder.hpp>
#include <luna/binding/constant_value.hpp>
#include <luna/binding/enum_builder.hpp>
#include <luna/binding/module_registration.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/binding/supported_callable.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/detail/callable_adapter.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/tooling/profiling_hook.hpp>
#include <luna/type/stable_type_key.hpp>

#include <string_view>
#include <utility>
// clang-format on

namespace Luna {

class BindingRegistry {
public:
  template <class Callable>
    requires SupportedCallable<Callable>
  [[nodiscard]] RegistrationResult RegisterFunction(std::string_view Name,
                                                    Callable &&Target) {
    ErasedCallableDescriptor Descriptor =
        Detail::MakeErasedCallableDescriptor(std::forward<Callable>(Target));
    return SubmitFunction(Name, std::move(Descriptor));
  }

  template <class Callable>
    requires SupportedCallable<Callable>
  [[nodiscard]] RegistrationResult Register(std::string_view GlobalName,
                                            Callable &&Target) {
    return RegisterFunction(GlobalName, std::forward<Callable>(Target));
  }

  [[nodiscard]] NamespaceBuilder RegisterNamespace(std::string_view Name);

  template <class ValueType>
  [[nodiscard]] RegistrationResult RegisterConstant(std::string_view Name,
                                                    ValueType &&Constant) {
    return CommitConstant(
        Name, Detail::MakeConstantRequest(std::forward<ValueType>(Constant)));
  }

  template <class ValueType>
  [[nodiscard]] RegistrationResult RegisterConstant(std::string_view Name,
                                                    ValueType &&Constant,
                                                    StableTypeKey Key) {
    return CommitConstant(
        Name, Detail::MakeConstantRequest(std::forward<ValueType>(Constant),
                                          std::move(Key)));
  }

  template <class Enum>
  [[nodiscard]] EnumBuilder<Enum> RegisterEnum(std::string_view Name,
                                               StableTypeKey Key) {
    return EnumBuilder<Enum>(Detail::StageRootEnumeration(
        *Owner, Name, Key, Detail::EnumerationPolicyFor<Enum>()));
  }

  template <class Type>
  [[nodiscard]] ClassBuilder<Type> RegisterClass(std::string_view Name,
                                                 StableTypeKey Key) {
    Detail::ClassStaging Staged = Detail::StageRootClassDeclaration(
        *Owner, Name, Key, Detail::ClassPolicyFor<Type>());
    return ClassBuilder<Type>(std::move(Staged), std::move(Key));
  }

  template <class Configure>
    requires ModuleConfiguration<Configure>
  [[nodiscard]] RegistrationResult ProvideModule(ModuleManifest Manifest,
                                                 Configure &&Registration) {
    return CommitProvidedModule(std::move(Manifest),
                                Detail::ModuleRegistration::Create(
                                    std::forward<Configure>(Registration)));
  }

  template <class Configure>
    requires ModuleConfiguration<Configure>
  [[nodiscard]] RegistrationResult RegisterModule(ModuleManifest Manifest,
                                                  Configure &&Registration) {
    return CommitModule(std::move(Manifest),
                        Detail::ModuleRegistration::Create(
                            std::forward<Configure>(Registration)));
  }

  [[nodiscard]] RegistrationResult Freeze() { return Owner->Freeze(); }

  [[nodiscard]] ReflectionSnapshot Reflection() const {
    return Owner->CaptureReflection();
  }

  // Installs a profiling or debug-UI hook. It runs on the owner thread only,
  // strictly after Luna has already produced the reported outcome, and
  // never changes invocation semantics. Installing a new hook replaces any
  // previous one.
  [[nodiscard]] RegistrationResult InstallProfilingHook(ProfilingHook Hook) {
    return Owner->InstallProfilingHook(std::move(Hook));
  }

  [[nodiscard]] RegistrationResult ClearProfilingHook() {
    return Owner->ClearProfilingHook();
  }

private:
  friend class State;

  explicit BindingRegistry(State &Owner) noexcept : Owner(&Owner) {}

  [[nodiscard]] RegistrationResult
  SubmitFunction(std::string_view Name, ErasedCallableDescriptor Descriptor) {
    return Owner->RegisterErased(Name, std::move(Descriptor));
  }

  [[nodiscard]] RegistrationResult
  CommitConstant(std::string_view Name, Detail::ConstantRequest Request);

  [[nodiscard]] RegistrationResult
  CommitProvidedModule(ModuleManifest Manifest,
                       Detail::ModuleRegistration Registration);

  [[nodiscard]] RegistrationResult
  CommitModule(ModuleManifest Manifest,
               Detail::ModuleRegistration Registration);

  State *Owner;
};

} // namespace Luna
