#pragma once

// clang-format off
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_construction.hpp>
#include <luna/binding/instance_receiver.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/reflection/storage.hpp"
#include "state/registration/member_plan.hpp"
#include "state/registration/operator_plan.hpp"
#include "state/registration/plan.hpp"
#include "state/registration/relationship_plan.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

inline constexpr std::string_view ClassMetatableSegment = "__LunaMetatable";

struct StagedConstruction final {
  std::string Segment;
  std::string QualifiedName;
  SymbolKind Kind = SymbolKind::Constructor;
  ConstructionOwnership Ownership = ConstructionOwnership::LuaOwned;
  std::string AllocatorPolicy;

  std::shared_ptr<ErasedCallableDescriptor> Callable;

  std::string Refusal;

  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;

  [[nodiscard]] bool HasTarget() const noexcept {
    return Callable != nullptr && Callable->HasTarget();
  }
};

struct StagedMethod final {
  std::string Segment;
  std::string QualifiedName;
  SymbolKind Kind = SymbolKind::Method;

  bool DeclaresReceiver = true;
  bool ReceiverIsConst = false;

  std::shared_ptr<ErasedCallableDescriptor> Callable;

  std::string Refusal;

  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;

  [[nodiscard]] bool HasTarget() const noexcept {
    return Callable != nullptr && Callable->HasTarget();
  }
};

struct StagedClass final {
  std::string Segment;
  std::string QualifiedName;
  StableTypeKey Key;
  ClassPolicy Policy;

  bool SelectsStorage = false;
  ClassAllocator Storage;

  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;

  std::vector<StagedConstruction> Constructions;

  std::vector<StagedMethod> Methods;

  std::vector<StagedMember> Members;

  std::vector<StagedOperator> Operators;

  PlannedClassRelationships Relationships;
};

[[nodiscard]] TypeDescriptor ClassTypeOf(const StagedClass &Declaration);

[[nodiscard]] DescriptorPlanEntry
MakeClassPlanEntry(const StagedClass &Declaration, SymbolId Parent);

[[nodiscard]] DescriptorPlanEntry
MakeClassMetatablePlanEntry(const StagedClass &Declaration,
                            const SymbolId &ClassSymbol);

[[nodiscard]] DescriptorPlanEntry
MakeConstructionPlanEntry(const StagedClass &Class,
                          const StagedConstruction &Declaration,
                          const SymbolId &ClassSymbol);

[[nodiscard]] DescriptorPlanEntry
MakeMethodPlanEntry(const StagedClass &Class, const StagedMethod &Declaration,
                    const SymbolId &ClassSymbol);

[[nodiscard]] StagedConstruction *
FindStagedConstruction(StagedClass &Declaration, std::string_view Segment);

[[nodiscard]] StagedMethod *FindStagedMethod(StagedClass &Declaration,
                                             std::string_view Segment);

[[nodiscard]] DescriptorPlanEntry
MakeOperatorPlanEntry(const StagedClass &Class,
                      const StagedOperator &Declaration,
                      const SymbolId &ClassSymbol);

[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateStagedClass(const StagedClass &Declaration);

[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateSelectedClassStorage(const StagedClass &Declaration,
                             const ClassAllocator &Storage);

[[nodiscard]] std::string
ReflectedAllocatorPolicy(const StagedClass &Class,
                         const StagedConstruction &Declaration);

[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateStagedConstruction(const StagedClass &Class,
                           const StagedConstruction &Declaration);

[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateStagedMethod(const StagedClass &Class, const StagedMethod &Declaration);

} // namespace Luna::Detail
