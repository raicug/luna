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
#include <luna/type/stable_type_key.hpp>

#include <string_view>
#include <utility>
// clang-format on

namespace Luna {

class BindingRegistry {
public:
  // Registers one root-scope function. Free functions, function pointers,
  // concrete lambdas, functors, static methods, explicit member wrappers, and
  // `Overload<Signature>` selections are all accepted here, and each one is
  // described by exactly the same canonical descriptor.
  template <class Callable>
    requires SupportedCallable<Callable>
  [[nodiscard]] RegistrationResult RegisterFunction(std::string_view Name,
                                                    Callable &&Target) {
    ErasedCallableDescriptor Descriptor =
        Detail::MakeErasedCallableDescriptor(std::forward<Callable>(Target));
    return SubmitFunction(Name, std::move(Descriptor));
  }

  // The established root-scope spelling. It is a source-compatible alias of
  // `RegisterFunction`, not a second behavior path: both build the same
  // descriptor, enter the same transaction, install the same callable target,
  // and report the same diagnostics in the same precedence.
  template <class Callable>
    requires SupportedCallable<Callable>
  [[nodiscard]] RegistrationResult Register(std::string_view GlobalName,
                                            Callable &&Target) {
    return RegisterFunction(GlobalName, std::forward<Callable>(Target));
  }

  // Stages one validated identifier segment as a root-scope namespace and
  // returns its builder. The builder owns a pending plan: nothing reaches the
  // virtual machine until `Commit`, and destroying it uncommitted has no
  // effect.
  [[nodiscard]] NamespaceBuilder RegisterNamespace(std::string_view Name);

  // Registers one immutable root-scope constant. A root single-symbol operation
  // commits immediately, as one outermost registration transaction.
  template <class ValueType>
  [[nodiscard]] RegistrationResult RegisterConstant(std::string_view Name,
                                                    ValueType &&Constant) {
    return CommitConstant(
        Name, Detail::MakeConstantRequest(std::forward<ValueType>(Constant)));
  }

  // The same, for a constant whose canonical type is a user-defined leaf such
  // as a registered enumeration.
  template <class ValueType>
  [[nodiscard]] RegistrationResult RegisterConstant(std::string_view Name,
                                                    ValueType &&Constant,
                                                    StableTypeKey Key) {
    return CommitConstant(
        Name, Detail::MakeConstantRequest(std::forward<ValueType>(Constant),
                                          std::move(Key)));
  }

  // Stages one root-scope enumeration and returns its builder. Nothing reaches
  // the virtual machine until `Commit`, and destroying the builder uncommitted
  // has no effect.
  template <class Enum>
  [[nodiscard]] EnumBuilder<Enum> RegisterEnum(std::string_view Name,
                                               StableTypeKey Key) {
    return EnumBuilder<Enum>(Detail::StageRootEnumeration(
        *Owner, Name, Key, Detail::EnumerationPolicyFor<Enum>()));
  }

  // Stages one root-scope class and returns its builder. The class stages one
  // canonical class type, one class symbol, and the cached metatable identity
  // that type owns in this logical State. Nothing reaches the virtual machine
  // until `Commit`, and destroying the builder uncommitted has no effect.
  template <class Type>
  [[nodiscard]] ClassBuilder<Type> RegisterClass(std::string_view Name,
                                                 StableTypeKey Key) {
    Detail::ClassStaging Staged = Detail::StageRootClassDeclaration(
        *Owner, Name, Key, Detail::ClassPolicyFor<Type>());
    return ClassBuilder<Type>(std::move(Staged), std::move(Key));
  }

  // Makes one module definition available to dependency resolution without
  // loading it. Providing a definition runs no callback, installs nothing, and
  // publishes nothing: it only tells this State which manifest and which scoped
  // registration belong to one module identity and version, so a later load can
  // execute the whole resolved graph inside one transaction.
  template <class Configure>
    requires ModuleConfiguration<Configure>
  [[nodiscard]] RegistrationResult ProvideModule(ModuleManifest Manifest,
                                                 Configure &&Registration) {
    return CommitProvidedModule(std::move(Manifest),
                                Detail::ModuleRegistration::Create(
                                    std::forward<Configure>(Registration)));
  }

  // Loads the module graph rooted at `Manifest` as one outermost registration
  // transaction. Resolution selects the highest available version satisfying
  // every accumulated constraint, every not-yet-loaded dependency callback and
  // this module's callback run dependency-first in canonical order, and the
  // selected graph, its exports, virtual-machine values, types, reflection
  // records, and dispatch targets are published atomically or not at all.
  template <class Configure>
    requires ModuleConfiguration<Configure>
  [[nodiscard]] RegistrationResult RegisterModule(ModuleManifest Manifest,
                                                  Configure &&Registration) {
    return CommitModule(std::move(Manifest),
                        Detail::ModuleRegistration::Create(
                            std::forward<Configure>(Registration)));
  }

  // Validates the complete committed model, prepares every deterministic
  // runtime lookup cache in unpublished immutable storage, and publishes the
  // caches together with the frozen lifecycle transition only on success.
  [[nodiscard]] RegistrationResult Freeze() { return Owner->Freeze(); }

  // Captures one committed reflection generation. The returned snapshot owns
  // its immutable storage, so it stays readable after later registrations, a
  // State move, and destruction of this State.
  [[nodiscard]] ReflectionSnapshot Reflection() const {
    return Owner->CaptureReflection();
  }

private:
  friend class State;

  explicit BindingRegistry(State &Owner) noexcept : Owner(&Owner) {}

  // The one adapter every root-scope callable API routes through, so `Register`
  // and `RegisterFunction` share one canonical descriptor builder, one
  // transaction entry, one callable target, and one diagnostic path.
  [[nodiscard]] RegistrationResult
  SubmitFunction(std::string_view Name, ErasedCallableDescriptor Descriptor) {
    return Owner->RegisterErased(Name, std::move(Descriptor));
  }

  // Submits one normalized root-scope constant, including a refused one, as its
  // own outermost registration transaction.
  [[nodiscard]] RegistrationResult
  CommitConstant(std::string_view Name, Detail::ConstantRequest Request);

  // Records one available module definition, and loads one module graph.
  [[nodiscard]] RegistrationResult
  CommitProvidedModule(ModuleManifest Manifest,
                       Detail::ModuleRegistration Registration);

  [[nodiscard]] RegistrationResult
  CommitModule(ModuleManifest Manifest,
               Detail::ModuleRegistration Registration);

  State *Owner;
};

} // namespace Luna
