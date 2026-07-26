#pragma once

// The pending plan behind one namespace builder chain.
//
// Every builder of a chain shares one plan. A builder operation stages one
// validated identifier segment under its own scope node and records the first
// deterministic failure of the chain; nothing reaches the virtual machine until
// `Commit` submits the whole plan as one outermost registration transaction.
// Destroying an uncommitted plan therefore has no virtual-machine, reflection,
// or dispatch effect at all.
//
// The plan also carries the identity a builder was created with: the logical
// State identity, the owner-object epoch, the scope identity, and the lifecycle
// generation. Every operation classifies that identity against the live State
// first, so use after an owner move, after destruction of the owner, after
// freeze, after scope removal, or after an incompatible generation replacement
// fails with one deterministic stale-builder diagnostic.

// clang-format off
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_construction.hpp>
#include <luna/binding/constant_value.hpp>
#include <luna/binding/enum_builder.hpp>
#include <luna/detail/construction_adapter.hpp>
#include <luna/detail/method_adapter.hpp>
#include <luna/binding/module_registration.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/binding/state_handle.hpp"
#include "state/module/load.hpp"
#include "state/registration/class_plan.hpp"
#include "state/registration/scope_plan.hpp"
#include "state/registration/value_plan.hpp"
#include "state/transaction/lifecycle.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

// Why one builder handle can no longer be used. `Usable` is the only value that
// permits staging or committing.
enum class BuilderHandleStatus {
  Usable,
  OwnerDestroyed,
  OwnerMoved,
  DifferentState,
  ReplacedGeneration,
  ForeignThread,
  Frozen,
  NotReady
};

[[nodiscard]] std::string_view
BuilderHandleStatusText(BuilderHandleStatus Status) noexcept;

// A status that makes the handle itself unusable: its State is gone, moved,
// replaced, or its scope belongs to a replaced lifecycle generation. Freeze and
// unavailability are ordinary lifecycle rejections of the transaction instead,
// so their wording and precedence stay shared with every other registration.
[[nodiscard]] bool IsFatalHandleStatus(BuilderHandleStatus Status) noexcept;

// The declared documentation surface of one staged declaration, as the three
// mutable fields every reflected declaration publishes. It is a view into one
// staged declaration of a plan, so it is only ever used while that plan is
// still being staged.
struct StagedAnnotationTarget final {
  std::string *Documentation = nullptr;
  std::vector<ReflectionAttributeFields> *Attributes = nullptr;
  std::vector<std::string> *Examples = nullptr;

  [[nodiscard]] bool IsValid() const noexcept {
    return Documentation != nullptr && Attributes != nullptr &&
           Examples != nullptr;
  }
};

class NamespaceBuilderState final {
public:
  // The scope node of the root builder. Every staged namespace is identified by
  // its index in the plan plus one, so zero always means the root scope.
  static constexpr std::size_t RootScopeNode = 0;

  // One shared plan per builder chain. Every builder of the chain keeps the
  // plan alive, so it dies with the last builder that referenced it.
  [[nodiscard]] static std::shared_ptr<NamespaceBuilderState>
  Create(State &Owner);

  // One public builder over `Plan`, scoped to `ScopeNode`. The module loader
  // uses it to hand a module callback a transaction-attached builder.
  [[nodiscard]] static NamespaceBuilder
  MakeBuilder(std::shared_ptr<NamespaceBuilderState> Plan,
              std::size_t ScopeNode);

  // Stages one validated identifier segment inside `ScopeNode` and returns the
  // scope node of the nested namespace. A failed staging keeps the first
  // deterministic diagnostic of the chain and returns the parent node, so a
  // returned builder is always usable as a handle and always fails at commit.
  [[nodiscard]] std::size_t Stage(std::size_t ScopeNode,
                                  std::string_view Segment);

  // Stages one function inside `ScopeNode`. The callable is only recorded here;
  // it joins the plan's one outermost transaction when the plan is submitted.
  void StageFunction(std::size_t ScopeNode, std::string_view Name,
                     ErasedCallableDescriptor Descriptor);

  // Stages one constant inside `ScopeNode`. A refused normalization and an
  // invalid name are recorded as the chain's first deterministic failure, so an
  // ignored intermediate result still fails the commit.
  void StageConstant(std::size_t ScopeNode, std::string_view Name,
                     ConstantRequest Request);

  // Stages one enumeration inside `ScopeNode` and returns its enumeration node:
  // the index in the staged enumerations plus one, or zero when the enumeration
  // could not be staged at all.
  [[nodiscard]] std::size_t StageEnumeration(std::size_t ScopeNode,
                                             std::string_view Name,
                                             const StableTypeKey &Key,
                                             const EnumerationPolicy &Policy);

  // Stages one class inside `ScopeNode` and returns its class node: the index
  // in the staged classes plus one, or zero when the class could not be staged
  // at all.
  [[nodiscard]] std::size_t StageClass(std::size_t ScopeNode,
                                       std::string_view Name,
                                       const StableTypeKey &Key,
                                       const ClassPolicy &Policy);

  // Stages one module load inside `ScopeNode`. The manifest and the erased
  // callback are recorded here; the callbacks of the resolved graph run when
  // the plan is submitted, inside the plan's one outermost transaction.
  void StageModule(std::size_t ScopeNode, ModuleManifest Manifest,
                   ModuleRegistration Registration);

  // Scope annotation staging. An empty member annotates the namespace of
  // `ScopeNode` itself; otherwise the declaration of that name already staged
  // inside it - a function, a constant, an enumeration, a class, or a nested
  // namespace. A member that was never declared, and the root scope itself,
  // which reflects no declaration of its own, are the chain's first
  // deterministic failure rather than metadata that belongs to nothing.
  void StageScopeDocumentation(std::size_t ScopeNode, std::string_view Member,
                               std::string_view Text);
  void StageScopeAttribute(std::size_t ScopeNode, std::string_view Member,
                           std::string_view Name,
                           std::string_view AttributeValue);
  void StageScopeExample(std::size_t ScopeNode, std::string_view Member,
                         std::string_view Text);

  // Stages every segment of one canonical qualified name in order and returns
  // the scope node of its last segment. An already committed Luna-owned
  // namespace is reopened when the plan is submitted, so seeding the scope of a
  // module callback never creates a second declaration.
  [[nodiscard]] std::size_t StagePath(std::string_view QualifiedName);

  // The staged declarations of this plan and the first deterministic failure of
  // its chain. The module loader reads both when it submits one module's
  // callback plan into the active transaction.
  [[nodiscard]] const BuilderPlan &Pending() const noexcept { return Staged; }

  [[nodiscard]] const std::optional<ErrorDiagnostic> &
  StagedFailure() const noexcept {
    return Failure;
  }

  // The plan was submitted by the loader instead of by `Commit`, so a later
  // `Commit` of the same chain cannot submit it a second time.
  void MarkSubmitted() noexcept { Committed = true; }

  // Enumeration builder operations. Each one stages metadata on one staged
  // enumeration; none of them touches the State or the virtual machine.
  void StageEnumerator(std::size_t EnumerationNode, std::string_view Name,
                       std::int64_t Numeric);
  void StageAlias(std::size_t EnumerationNode, std::string_view AliasName,
                  std::string_view CanonicalName);
  void StageBitflags(std::size_t EnumerationNode, bool HasDeclaredMask,
                     std::int64_t SupportedBits);
  void StageUnscopedOptIn(std::size_t EnumerationNode);

  // An empty member documents or annotates the enumeration itself; otherwise
  // the named enumerator or alias, which must already be declared.
  void StageEnumerationDocumentation(std::size_t EnumerationNode,
                                     std::string_view Member,
                                     std::string_view Text);
  void StageEnumerationAttribute(std::size_t EnumerationNode,
                                 std::string_view Member, std::string_view Name,
                                 std::string_view AttributeValue);
  void StageEnumerationExample(std::size_t EnumerationNode,
                               std::string_view Member, std::string_view Text);

  // Class builder operations. Each one stages metadata on one staged class;
  // none of them touches the State or the virtual machine.
  //
  // An empty member documents or annotates the class itself; otherwise the
  // named member, which must already be declared: a construction candidate, a
  // method, a property or field, or one Luna-owned operator segment.
  void StageClassDocumentation(std::size_t ClassNode, std::string_view Member,
                               std::string_view Text);
  void StageClassAttribute(std::size_t ClassNode, std::string_view Member,
                           std::string_view Name,
                           std::string_view AttributeValue);
  void StageClassExample(std::size_t ClassNode, std::string_view Member,
                         std::string_view Text);

  // The same three operations for one operator of a staged class, named by the
  // operator it answers rather than by the Luna-owned segment it is published
  // under. The operator must already be declared.
  void StageClassOperatorDocumentation(std::size_t ClassNode,
                                       ClassOperator Selected,
                                       std::string_view Text);
  void StageClassOperatorAttribute(std::size_t ClassNode,
                                   ClassOperator Selected,
                                   std::string_view Name,
                                   std::string_view AttributeValue);
  void StageClassOperatorExample(std::size_t ClassNode, ClassOperator Selected,
                                 std::string_view Text);

  // Stages one construction candidate inside one staged class.
  void StageClassConstruction(std::size_t ClassNode, std::string_view Name,
                              ConstructionRequest Request);

  // Stages one member candidate inside one staged class: one instance method or
  // one static method. Both are validated where the consumer's declaration is
  // still known, so a receiver that contradicts the class it was declared in
  // and one name declared both with and without a receiver are the chain's
  // first deterministic failure.
  void StageClassMember(std::size_t ClassNode, std::string_view Name,
                        MethodRequest Request);

  // Stages one typed accessor inside one staged class: one property or one
  // field. The declaration is checked as a whole here, where the consumer's
  // accessors are still known, so a policy that contradicts them, a value type
  // Luna cannot carry across the member boundary, and a name Luna reserves for
  // itself are all the chain's first deterministic failure.
  void StageClassAccessor(std::size_t ClassNode, std::string_view Name,
                          MemberRequest Request);

  // Stages the storage protocol one staged class selects for the values Luna
  // creates of it. It is validated against the declared storage shape of that
  // class immediately, so an incompatible protocol is the chain's first
  // deterministic failure whether it was stated before or after the candidates
  // that use it.
  void StageClassAllocator(std::size_t ClassNode,
                           const ClassAllocator &Storage);

  // Stages one base edge, or one safe downcast policy, of one staged class. The
  // whole candidate graph decides both, so nothing is validated here beyond the
  // class node itself existing.
  void StageClassBase(std::size_t ClassNode, BaseRequest Request);
  void StageClassCast(std::size_t ClassNode, CastRequest Request);

  // Stages one operator of one staged class. The declaration is checked as a
  // whole here, where the consumer's target is still known, so an operand count
  // no call of that operator could supply and one operator declared twice are
  // the chain's first deterministic failure.
  void StageClassOperator(std::size_t ClassNode, ClassOperator Selected,
                          MethodRequest Request);

  [[nodiscard]] std::string_view
  QualifiedNameOf(std::size_t ScopeNode) const noexcept;

  [[nodiscard]] std::string_view
  QualifiedNameOfEnumeration(std::size_t EnumerationNode) const noexcept;

  [[nodiscard]] std::string_view
  QualifiedNameOfClass(std::size_t ClassNode) const noexcept;

  // Submits the whole plan as one outermost registration transaction.
  [[nodiscard]] RegistrationResult Commit();

  [[nodiscard]] bool IsCommitted() const noexcept { return Committed; }

  // Classification of the captured handle against the live State right now.
  [[nodiscard]] BuilderHandleStatus Classify() const noexcept;

private:
  explicit NamespaceBuilderState(State &Owner) noexcept;

  // The live implementation of the captured owner, or null when the handle can
  // no longer be used. Nothing dereferences the owner before this succeeds.
  [[nodiscard]] State *LiveOwner() const noexcept;

  void RecordFailure(ErrorDiagnostic Diagnostic);

  [[nodiscard]] ErrorDiagnostic
  StaleDiagnostic(BuilderHandleStatus Status,
                  std::string_view QualifiedName) const;

  // True when the chain may still stage a declaration into `ScopeNode`: the
  // handle is usable and the node exists. A fatal handle status records the
  // stale-builder diagnostic here, so no staging path dereferences its State.
  [[nodiscard]] bool CanStage(std::size_t ScopeNode);

  // The staged enumeration of one enumeration node, or null.
  [[nodiscard]] StagedEnumeration *
  EnumerationAt(std::size_t EnumerationNode) noexcept;

  // The staged class of one class node, or null.
  [[nodiscard]] StagedClass *ClassAt(std::size_t ClassNode) noexcept;

  // The staged construction candidate of one already declared member name, or
  // null. A member that was never declared is a deterministic failure of the
  // chain, never a silently created declaration.
  [[nodiscard]] StagedConstruction *
  ConstructionAt(StagedClass &Declaration, std::string_view Member) noexcept;

  // The staged method candidate of one already declared member name, or null.
  [[nodiscard]] StagedMethod *MethodAt(StagedClass &Declaration,
                                       std::string_view Member) noexcept;

  // The staged property or field of one already declared member name, or null.
  [[nodiscard]] StagedMember *AccessorAt(StagedClass &Declaration,
                                         std::string_view Member) noexcept;

  // The staged operator of one already declared Luna-owned segment, or null.
  [[nodiscard]] StagedOperator *OperatorAt(StagedClass &Declaration,
                                           std::string_view Member) noexcept;

  // The annotation target of one scope-level name. An empty member names the
  // namespace of `ScopeNode` itself, and every other member names one
  // declaration already staged inside it. An unresolvable request records the
  // chain's first deterministic failure and returns an invalid target.
  [[nodiscard]] StagedAnnotationTarget
  ScopeAnnotationTarget(std::size_t ScopeNode, std::string_view Member);

  // The annotation target of one class member name, and of one enumeration
  // member name. An empty member names the class or enumeration itself.
  [[nodiscard]] StagedAnnotationTarget
  ClassAnnotationTarget(std::size_t ClassNode, std::string_view Member);
  [[nodiscard]] StagedAnnotationTarget
  ClassOperatorAnnotationTarget(std::size_t ClassNode, ClassOperator Selected);
  [[nodiscard]] StagedAnnotationTarget
  EnumerationAnnotationTarget(std::size_t EnumerationNode,
                              std::string_view Member);

  State *Owner = nullptr;
  std::weak_ptr<StateHandleToken> Handle;
  StateIdentity Identity;
  std::uint64_t OwnerEpoch = 0;
  std::uint64_t LifecycleGeneration = 0;

  BuilderPlan Staged;
  std::optional<ErrorDiagnostic> Failure;
  bool Committed = false;
};

} // namespace Luna::Detail
