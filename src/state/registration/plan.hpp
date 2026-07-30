#pragma once

// clang-format off
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_member.hpp>
#include <luna/binding/class_operator.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/symbol_descriptor.hpp"
#include "state/reflection/storage.hpp"
#include "state/registration/relationship_plan.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/type_record.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

enum class PlanEntryKind {
  Function,
  Scope,
  Value,
  Type,
  ReflectionRecord,
  Module,
  DispatchTarget,
  Metatable,
  ClassSymbol,
  ClassMember
};

[[nodiscard]] std::string_view
PlanEntryKindText(PlanEntryKind Category) noexcept;

struct PlannedValue final {
  TypeDescriptor Type;
  StructuredValue Staged;
};

struct PlannedValueField final {
  std::string Name;
  StructuredValue Staged;
};

struct PlannedValueTable final {
  TypeDescriptor Type;
  std::vector<PlannedValueField> Fields;
};

struct PlannedClassMember final {
  SymbolKind Kind = SymbolKind::Property;
  MemberAccess Access = MemberAccess::ReadOnly;
  PropertyEvaluation Evaluation = PropertyEvaluation::Immediate;
  MemberOwnership Ownership = MemberOwnership::Copied;
  TypeId ValueType;

  TypeDescriptor ValueDescriptor;

  bool ReadRequiresMutableReceiver = false;
  MemberReadOperation Read;
  MemberWriteOperation Write;
  MemberChangeOperation Change;

  MemberConvertedReadOperation ConvertedRead;
  MemberConvertedWriteOperation ConvertedWrite;
};

struct PlannedClassOperator final {
  ClassOperator Selected = ClassOperator::Call;
  std::string Segment;
};

struct DescriptorPlanEntry final {
  PlanEntryKind Category = PlanEntryKind::Function;
  SymbolDescriptor Symbol;
  SymbolId Identity;
  std::string VmPath;
  std::optional<ReflectionRecordFields> Record;

  std::optional<ReflectionRecordFields> OverloadSetRecord;

  std::optional<ReflectionTypeFields> TypeFields;
  std::optional<ReflectionModuleFields> ModuleFields;

  std::string ModuleIdentity;

  std::optional<ClassPolicy> ClassStorage;

  std::optional<TypeRecord> TypeConversion;

  std::vector<TypeRecord> ParameterTypeConversions;

  std::optional<ErasedCallableDescriptor> Callable;
  std::size_t DispatchSlot = 0;

  std::optional<PlannedValue> InstalledValue;

  std::optional<PlannedValueTable> InstalledTable;

  std::optional<PlannedClassMember> ClassMember;

  std::optional<PlannedClassRelationships> Relationships;

  std::optional<PlannedClassOperator> OperatorFields;

  DescriptorPlanEntry() = default;

  DescriptorPlanEntry(const DescriptorPlanEntry &) = delete;
  DescriptorPlanEntry &operator=(const DescriptorPlanEntry &) = delete;
  DescriptorPlanEntry(DescriptorPlanEntry &&) noexcept = default;
  DescriptorPlanEntry &operator=(DescriptorPlanEntry &&) noexcept = default;
  ~DescriptorPlanEntry() = default;

  [[nodiscard]] bool IsValid() const;
};

[[nodiscard]] CallableSignatureDescriptor
CanonicalFoundationSignature(const CallableMetadata &Metadata);

[[nodiscard]] DescriptorPlanEntry
MakeFunctionPlanEntry(std::string QualifiedName,
                      ErasedCallableDescriptor Callable,
                      SymbolId Parent = SymbolId());

void ApplyDeclaredAnnotations(DescriptorPlanEntry &Entry,
                              std::string_view Documentation,
                              std::vector<ReflectionAttributeFields> Attributes,
                              std::vector<std::string> Examples);

void JoinPlannedOverloadSet(DescriptorPlanEntry &Entry) noexcept;

[[nodiscard]] DescriptorPlanEntry MakeTypePlanEntry(std::string QualifiedName,
                                                    TypeRecord Type);

class DescriptorPlan final {
public:
  DescriptorPlan() = default;

  DescriptorPlan(const DescriptorPlan &) = delete;
  DescriptorPlan &operator=(const DescriptorPlan &) = delete;
  DescriptorPlan(DescriptorPlan &&) noexcept = default;
  DescriptorPlan &operator=(DescriptorPlan &&) noexcept = default;
  ~DescriptorPlan() = default;

  std::size_t Append(DescriptorPlanEntry Entry);

  [[nodiscard]] std::size_t Size() const noexcept { return Entries.size(); }
  [[nodiscard]] bool IsEmpty() const noexcept { return Entries.empty(); }

  [[nodiscard]] DescriptorPlanEntry *At(std::size_t Index) noexcept;
  [[nodiscard]] const DescriptorPlanEntry *At(std::size_t Index) const noexcept;

  [[nodiscard]] std::span<const DescriptorPlanEntry>
  PlannedEntries() const noexcept {
    return Entries;
  }

  [[nodiscard]] const DescriptorPlanEntry *
  Find(std::string_view QualifiedName) const noexcept;
  [[nodiscard]] const DescriptorPlanEntry *
  Find(const SymbolId &Identity) const noexcept;
  [[nodiscard]] bool Contains(std::string_view QualifiedName) const noexcept;

  [[nodiscard]] std::size_t CountOf(PlanEntryKind Category) const noexcept;

  [[nodiscard]] std::vector<std::size_t> CanonicalOrder() const;

  void Clear() noexcept { Entries.clear(); }

private:
  std::vector<DescriptorPlanEntry> Entries;
};

[[nodiscard]] bool
PlanContributesReflection(const DescriptorPlan &Plan) noexcept;

[[nodiscard]] std::size_t
PlannedReflectionRecordCount(const DescriptorPlan &Plan) noexcept;

[[nodiscard]] bool PlanEntryPrecedes(const SymbolDescriptor &Left,
                                     const SymbolId &LeftIdentity,
                                     const SymbolDescriptor &Right,
                                     const SymbolId &RightIdentity);

} // namespace Luna::Detail
