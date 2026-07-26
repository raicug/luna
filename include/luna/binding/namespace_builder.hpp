#pragma once

// One transaction-attached namespace builder.
// `BindingRegistry::RegisterNamespace` and
// `NamespaceBuilder::RegisterNamespace` accept exactly one validated identifier
// segment and return a builder that owns a pending registration plan. Nested
// builders share that plan and stage their declarations under their own scope
// node, so `Commit` submits the whole plan as one outermost registration
// transaction: either every namespace of the plan becomes visible or none does.
// Destroying an uncommitted builder has no virtual-machine effect at all.
//
// A builder carries the logical State identity, the owner-object epoch, the
// scope identity, and the lifecycle generation it was created with. Use after
// the implementation moves to another owner object, after the owner is
// destroyed, after freeze, after its scope is removed, or after an incompatible
// generation replacement fails with one deterministic stale-builder diagnostic
// instead of touching the virtual machine.

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

  // Destroying an uncommitted builder discards its pending plan without
  // touching the virtual machine, reflection, or dispatch.
  ~NamespaceBuilder();

  // Stages one validated identifier segment inside this namespace and returns
  // the builder of the nested namespace. The returned builder shares this
  // builder's pending plan.
  [[nodiscard]] NamespaceBuilder RegisterNamespace(std::string_view Name);

  // Stages one function inside this namespace. It accepts exactly the callable
  // forms root-scope registration accepts - free functions, function pointers,
  // concrete lambdas, functors, static methods, explicit member wrappers, and
  // `Overload<Signature>` selections - and describes them with exactly the same
  // canonical descriptor, so a scoped function and a root-scope one differ only
  // in their qualified name.
  template <class Callable>
    requires SupportedCallable<Callable>
  NamespaceBuilder &RegisterFunction(std::string_view Name, Callable &&Target) {
    ErasedCallableDescriptor Descriptor =
        Detail::MakeErasedCallableDescriptor(std::forward<Callable>(Target));
    StageFunction(Name, std::move(Descriptor));
    return *this;
  }

  // Stages one immutable constant inside this namespace. The value is
  // normalized to its canonical type in the caller's translation unit and
  // converted through that type's registered writer at commit time.
  template <class ValueType>
  NamespaceBuilder &RegisterConstant(std::string_view Name,
                                     ValueType &&Constant) {
    StageConstant(
        Name, Detail::MakeConstantRequest(std::forward<ValueType>(Constant)));
    return *this;
  }

  // The same, for a constant whose canonical type is a user-defined leaf such
  // as a registered enumeration: the leaf consumes its explicit validated
  // stable key, so an enumeration constant keeps the enumeration's canonical
  // type identity instead of degrading into an untyped integer.
  template <class ValueType>
  NamespaceBuilder &RegisterConstant(std::string_view Name,
                                     ValueType &&Constant, StableTypeKey Key) {
    StageConstant(Name, Detail::MakeConstantRequest(
                            std::forward<ValueType>(Constant), std::move(Key)));
    return *this;
  }

  // Stages one enumeration inside this namespace and returns its builder. The
  // returned builder shares this builder's pending plan.
  template <class Enum>
  [[nodiscard]] EnumBuilder<Enum> RegisterEnum(std::string_view Name,
                                               StableTypeKey Key) {
    return EnumBuilder<Enum>(Detail::StageEnumeration(
        Plan, Scope, Name, Key, Detail::EnumerationPolicyFor<Enum>()));
  }

  // Stages one class inside this namespace and returns its builder. The
  // returned builder shares this builder's pending plan, so the class, its
  // canonical type, and its metatable identity commit with everything else the
  // plan staged.
  template <class Type>
  [[nodiscard]] ClassBuilder<Type> RegisterClass(std::string_view Name,
                                                 StableTypeKey Key) {
    Detail::ClassStaging Staged = Detail::StageClassDeclaration(
        Plan, Scope, Name, Key, Detail::ClassPolicyFor<Type>());
    return ClassBuilder<Type>(std::move(Staged), std::move(Key));
  }

  // Stages one module load inside this namespace. When this plan commits, the
  // module graph rooted at `Manifest` resolves against the definitions this
  // State was given, and every not-yet-loaded dependency callback plus this
  // module's callback runs inside exactly this plan's outermost transaction.
  // Each callback receives a transaction-attached builder scoped to this
  // namespace.
  template <class Configure>
    requires ModuleConfiguration<Configure>
  NamespaceBuilder &RegisterModule(ModuleManifest Manifest,
                                   Configure &&Registration) {
    StageModule(std::move(Manifest),
                Detail::ModuleRegistration::Create(
                    std::forward<Configure>(Registration)));
    return *this;
  }

  // Documents this namespace itself. The text becomes the documentation of the
  // namespace's own reflection record, which is what documentation and
  // declaration generation read.
  NamespaceBuilder &Documentation(std::string_view Text) {
    StageDocumentation(std::string_view(), Text);
    return *this;
  }

  // Documents one declaration already staged in this namespace: a function, a
  // constant, an enumeration, a class, or a nested namespace. The member must
  // already be declared, so a typo fails the commit deterministically instead
  // of documenting nothing.
  NamespaceBuilder &Documentation(std::string_view Member,
                                  std::string_view Text) {
    StageDocumentation(Member, Text);
    return *this;
  }

  // Annotates this namespace itself.
  NamespaceBuilder &Attribute(std::string_view Name,
                              std::string_view AttributeValue) {
    StageAttribute(std::string_view(), Name, AttributeValue);
    return *this;
  }

  // Annotates one declaration already staged in this namespace.
  NamespaceBuilder &Attribute(std::string_view Member, std::string_view Name,
                              std::string_view AttributeValue) {
    StageAttribute(Member, Name, AttributeValue);
    return *this;
  }

  // Adds one usage example to this namespace itself. Examples are reflected in
  // declaration order, so generated material repeats them exactly as declared.
  NamespaceBuilder &Example(std::string_view Text) {
    StageExample(std::string_view(), Text);
    return *this;
  }

  // Adds one usage example to a declaration already staged in this namespace.
  NamespaceBuilder &Example(std::string_view Member, std::string_view Text) {
    StageExample(Member, Text);
    return *this;
  }

  // The canonical `.`-separated qualified name of this namespace.
  [[nodiscard]] std::string_view QualifiedName() const noexcept;

  // Submits the whole pending plan as one outermost registration transaction.
  [[nodiscard]] RegistrationResult Commit();

private:
  friend class BindingRegistry;
  friend class Detail::NamespaceBuilderState;

  // `Scope` is the staged scope node this builder registers into: zero is the
  // root scope and every other value names one staged namespace of the plan.
  NamespaceBuilder(std::shared_ptr<Detail::NamespaceBuilderState> Plan,
                   std::size_t Scope) noexcept;

  // Records one function declaration in the shared pending plan. Nothing is
  // installed here: the staged callable joins the plan's one outermost
  // transaction when the plan commits.
  void StageFunction(std::string_view Name,
                     ErasedCallableDescriptor Descriptor);

  // Records one normalized constant declaration in the shared pending plan,
  // including a refused one, so an unsupported value fails the commit with one
  // deterministic diagnostic instead of being silently dropped.
  void StageConstant(std::string_view Name, Detail::ConstantRequest Request);

  // Records one module load in the shared pending plan.
  void StageModule(ModuleManifest Manifest,
                   Detail::ModuleRegistration Registration);

  // Records the declared documentation surface of this namespace or of one
  // declaration staged inside it. An empty member names the namespace itself.
  void StageDocumentation(std::string_view Member, std::string_view Text);
  void StageAttribute(std::string_view Member, std::string_view Name,
                      std::string_view AttributeValue);
  void StageExample(std::string_view Member, std::string_view Text);

  std::shared_ptr<Detail::NamespaceBuilderState> Plan;
  std::size_t Scope = 0;
};

} // namespace Luna
