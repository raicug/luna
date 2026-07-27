#pragma once

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

[[nodiscard]] bool IsFatalHandleStatus(BuilderHandleStatus Status) noexcept;

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
  static constexpr std::size_t RootScopeNode = 0;

  [[nodiscard]] static std::shared_ptr<NamespaceBuilderState>
  Create(State &Owner);

  [[nodiscard]] static NamespaceBuilder
  MakeBuilder(std::shared_ptr<NamespaceBuilderState> Plan,
              std::size_t ScopeNode);

  [[nodiscard]] std::size_t Stage(std::size_t ScopeNode,
                                  std::string_view Segment);

  void StageFunction(std::size_t ScopeNode, std::string_view Name,
                     ErasedCallableDescriptor Descriptor);

  void StageConstant(std::size_t ScopeNode, std::string_view Name,
                     ConstantRequest Request);

  [[nodiscard]] std::size_t StageEnumeration(std::size_t ScopeNode,
                                             std::string_view Name,
                                             const StableTypeKey &Key,
                                             const EnumerationPolicy &Policy);

  [[nodiscard]] std::size_t StageClass(std::size_t ScopeNode,
                                       std::string_view Name,
                                       const StableTypeKey &Key,
                                       const ClassPolicy &Policy);

  void StageModule(std::size_t ScopeNode, ModuleManifest Manifest,
                   ModuleRegistration Registration);

  void StageScopeDocumentation(std::size_t ScopeNode, std::string_view Member,
                               std::string_view Text);
  void StageScopeAttribute(std::size_t ScopeNode, std::string_view Member,
                           std::string_view Name,
                           std::string_view AttributeValue);
  void StageScopeExample(std::size_t ScopeNode, std::string_view Member,
                         std::string_view Text);

  [[nodiscard]] std::size_t StagePath(std::string_view QualifiedName);

  [[nodiscard]] const BuilderPlan &Pending() const noexcept { return Staged; }

  [[nodiscard]] const std::optional<ErrorDiagnostic> &
  StagedFailure() const noexcept {
    return Failure;
  }

  void MarkSubmitted() noexcept { Committed = true; }

  void StageEnumerator(std::size_t EnumerationNode, std::string_view Name,
                       std::int64_t Numeric);
  void StageAlias(std::size_t EnumerationNode, std::string_view AliasName,
                  std::string_view CanonicalName);
  void StageBitflags(std::size_t EnumerationNode, bool HasDeclaredMask,
                     std::int64_t SupportedBits);
  void StageUnscopedOptIn(std::size_t EnumerationNode);

  void StageEnumerationDocumentation(std::size_t EnumerationNode,
                                     std::string_view Member,
                                     std::string_view Text);
  void StageEnumerationAttribute(std::size_t EnumerationNode,
                                 std::string_view Member, std::string_view Name,
                                 std::string_view AttributeValue);
  void StageEnumerationExample(std::size_t EnumerationNode,
                               std::string_view Member, std::string_view Text);

  void StageClassDocumentation(std::size_t ClassNode, std::string_view Member,
                               std::string_view Text);
  void StageClassAttribute(std::size_t ClassNode, std::string_view Member,
                           std::string_view Name,
                           std::string_view AttributeValue);
  void StageClassExample(std::size_t ClassNode, std::string_view Member,
                         std::string_view Text);

  void StageClassOperatorDocumentation(std::size_t ClassNode,
                                       ClassOperator Selected,
                                       std::string_view Text);
  void StageClassOperatorAttribute(std::size_t ClassNode,
                                   ClassOperator Selected,
                                   std::string_view Name,
                                   std::string_view AttributeValue);
  void StageClassOperatorExample(std::size_t ClassNode, ClassOperator Selected,
                                 std::string_view Text);

  void StageClassConstruction(std::size_t ClassNode, std::string_view Name,
                              ConstructionRequest Request);

  void StageClassMember(std::size_t ClassNode, std::string_view Name,
                        MethodRequest Request);

  void StageClassAccessor(std::size_t ClassNode, std::string_view Name,
                          MemberRequest Request);

  void StageClassAllocator(std::size_t ClassNode,
                           const ClassAllocator &Storage);

  void StageClassBase(std::size_t ClassNode, BaseRequest Request);
  void StageClassCast(std::size_t ClassNode, CastRequest Request);

  void StageClassOperator(std::size_t ClassNode, ClassOperator Selected,
                          MethodRequest Request);

  [[nodiscard]] std::string_view
  QualifiedNameOf(std::size_t ScopeNode) const noexcept;

  [[nodiscard]] std::string_view
  QualifiedNameOfEnumeration(std::size_t EnumerationNode) const noexcept;

  [[nodiscard]] std::string_view
  QualifiedNameOfClass(std::size_t ClassNode) const noexcept;

  [[nodiscard]] RegistrationResult Commit();

  [[nodiscard]] bool IsCommitted() const noexcept { return Committed; }

  [[nodiscard]] BuilderHandleStatus Classify() const noexcept;

private:
  explicit NamespaceBuilderState(State &Owner) noexcept;

  [[nodiscard]] State *LiveOwner() const noexcept;

  void RecordFailure(ErrorDiagnostic Diagnostic);

  [[nodiscard]] ErrorDiagnostic
  StaleDiagnostic(BuilderHandleStatus Status,
                  std::string_view QualifiedName) const;

  [[nodiscard]] bool CanStage(std::size_t ScopeNode);

  [[nodiscard]] StagedEnumeration *
  EnumerationAt(std::size_t EnumerationNode) noexcept;

  [[nodiscard]] StagedClass *ClassAt(std::size_t ClassNode) noexcept;

  [[nodiscard]] StagedConstruction *
  ConstructionAt(StagedClass &Declaration, std::string_view Member) noexcept;

  [[nodiscard]] StagedMethod *MethodAt(StagedClass &Declaration,
                                       std::string_view Member) noexcept;

  [[nodiscard]] StagedMember *AccessorAt(StagedClass &Declaration,
                                         std::string_view Member) noexcept;

  [[nodiscard]] StagedOperator *OperatorAt(StagedClass &Declaration,
                                           std::string_view Member) noexcept;

  [[nodiscard]] StagedAnnotationTarget
  ScopeAnnotationTarget(std::size_t ScopeNode, std::string_view Member);

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
