// clang-format off
#include "state/registration/plan.hpp"

#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/value.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/identity/symbol_descriptor.hpp"
#include "state/registration/parameter_shape.hpp"
#include "state/registration/return_shape.hpp"
#include "state/registration/scope_plan.hpp"
#include "state/type/type_record.hpp"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] std::strong_ordering
CompareText(std::string_view Left, std::string_view Right) noexcept {
  const int Comparison = Left.compare(Right);
  if (Comparison < 0)
    return std::strong_ordering::less;
  if (Comparison > 0)
    return std::strong_ordering::greater;
  return std::strong_ordering::equal;
}

[[nodiscard]] constexpr bool RequiresVmPath(PlanEntryKind Category) noexcept {
  switch (Category) {
  case PlanEntryKind::Function:
  case PlanEntryKind::Scope:
  case PlanEntryKind::Value:
  case PlanEntryKind::Module:
  case PlanEntryKind::Metatable:
  case PlanEntryKind::ClassSymbol:
    return true;
  default:
    return false;
  }
}

} // namespace

std::string_view PlanEntryKindText(PlanEntryKind Category) noexcept {
  switch (Category) {
  case PlanEntryKind::Function:
    return "function";
  case PlanEntryKind::Scope:
    return "scope";
  case PlanEntryKind::Value:
    return "value";
  case PlanEntryKind::Type:
    return "type";
  case PlanEntryKind::ReflectionRecord:
    return "reflection_record";
  case PlanEntryKind::Module:
    return "module";
  case PlanEntryKind::DispatchTarget:
    return "dispatch_target";
  case PlanEntryKind::Metatable:
    return "metatable";
  case PlanEntryKind::ClassSymbol:
    return "class_symbol";
  case PlanEntryKind::ClassMember:
    return "class_member";
  }
  return "unknown";
}

bool DescriptorPlanEntry::IsValid() const {
  if (!Symbol.IsValid() || !Identity.IsValid())
    return false;
  if (RequiresVmPath(Category) && VmPath.empty())
    return false;

  switch (Category) {
  case PlanEntryKind::Function:
    return Callable.has_value() && Callable->HasTarget();
  case PlanEntryKind::Value:
    return Record.has_value() && InstalledValue.has_value();
  case PlanEntryKind::Type:
    return TypeFields.has_value();
  case PlanEntryKind::Module:
    return ModuleFields.has_value();
  case PlanEntryKind::ReflectionRecord:
    return Record.has_value();
  case PlanEntryKind::ClassMember:
    return Record.has_value() && ClassMember.has_value() &&
           (ClassMember->Read != nullptr || ClassMember->Write != nullptr ||
            ClassMember->ConvertedRead != nullptr ||
            ClassMember->ConvertedWrite != nullptr ||
            ClassMember->InstanceRead != nullptr ||
            ClassMember->InstanceWrite != nullptr);
  default:
    return true;
  }
}

CallableSignatureDescriptor
CanonicalFoundationSignature(const CallableMetadata &Metadata) {
  if (Metadata.HasRichParameters())
    return CanonicalDeclaredSignature(Metadata);

  CallableSignatureDescriptor Signature;
  for (const ValueKind Parameter : Metadata.ParameterTypes())
    Signature.ParameterTypes.push_back(CanonicalValueType(Parameter));

  Signature.RequiredParameterCount = Signature.ParameterTypes.size();
  Signature.IsVariadic = false;

  Signature.ReturnType = CanonicalReturnType(Metadata.ReturnType());
  return WithCanonicalReceiver(Metadata, std::move(Signature));
}

DescriptorPlanEntry MakeFunctionPlanEntry(std::string QualifiedName,
                                          ErasedCallableDescriptor Callable,
                                          SymbolId Parent) {
  DescriptorPlanEntry Entry;
  Entry.Category = PlanEntryKind::Function;
  Entry.VmPath = QualifiedName;

  const CallableSignatureDescriptor Signature =
      CanonicalFoundationSignature(Callable.Metadata());
  const std::string LocalName = std::string(FinalSegment(QualifiedName));

  const SymbolDescriptor SetSymbol =
      MakeOverloadSetSymbol(QualifiedName, Parent);
  SymbolId SetIdentity;
  if (const auto Identity = SymbolIdentityRegistry::ComputeIdentity(SetSymbol))
    SetIdentity = *Identity;

  Entry.Symbol = MakeCallableCandidateSymbol(SymbolKind::FunctionCandidate,
                                             QualifiedName, Parent, Signature);
  if (const auto Identity =
          SymbolIdentityRegistry::ComputeIdentity(Entry.Symbol))
    Entry.Identity = *Identity;

  if (SetIdentity.IsValid()) {
    ReflectionRecordFields SetRecord;
    SetRecord.Kind = SymbolKind::OverloadSet;
    SetRecord.Id = SetIdentity;
    SetRecord.Name = LocalName;
    SetRecord.QualifiedName = QualifiedName;
    SetRecord.Scope = Parent.IsValid() ? ScopeId(Parent) : ScopeId::Root();
    Entry.OverloadSetRecord = std::move(SetRecord);
  }

  ReflectionRecordFields Record;
  Record.Kind = SymbolKind::FunctionCandidate;
  Record.Id = Entry.Identity;
  Record.Name = LocalName;
  Record.QualifiedName = QualifiedName;
  Record.Signature = CanonicalSignatureText(Signature);
  Record.Scope = SetIdentity.IsValid()
                     ? ScopeId(SetIdentity)
                     : (Parent.IsValid() ? ScopeId(Parent) : ScopeId::Root());
  Record.OverloadSet = SetIdentity;
  Record.Descriptor = Signature.ReturnType;
  if (const auto Identity =
          TypeIdentityRegistry::ComputeIdentity(Signature.ReturnType))
    Record.Type = *Identity;
  Record.Parameters = MakeReflectedParameters(Callable.Metadata());
  Record.ReturnValues = MakeReflectedReturns(Callable.Metadata());
  Record.Returns = ReflectedReturnShape(Callable.Metadata());
  Record.ReturnsAsynchronously =
      Callable.Metadata().ReturnType().IsAsynchronous();
  Entry.Record = std::move(Record);

  Entry.ParameterTypeConversions =
      MakeParameterTypeConversions(Callable.Metadata());

  Entry.Callable = std::move(Callable);
  return Entry;
}

void ApplyDeclaredAnnotations(DescriptorPlanEntry &Entry,
                              std::string_view Documentation,
                              std::vector<ReflectionAttributeFields> Attributes,
                              std::vector<std::string> Examples) {
  if (!Entry.Record)
    return;
  Entry.Record->Documentation = std::string(Documentation);
  Entry.Record->Attributes = std::move(Attributes);
  Entry.Record->Examples = std::move(Examples);
}

void JoinPlannedOverloadSet(DescriptorPlanEntry &Entry) noexcept {
  Entry.OverloadSetRecord.reset();
}

DescriptorPlanEntry MakeTypePlanEntry(std::string QualifiedName,
                                      TypeRecord Type) {
  DescriptorPlanEntry Entry;
  Entry.Category = PlanEntryKind::Type;

  SymbolDescriptor Symbol;
  Symbol.Kind = SymbolKind::Type;
  Symbol.QualifiedName = QualifiedName;
  Symbol.AssociatedType = Type.Descriptor;
  Entry.Symbol = std::move(Symbol);
  if (const auto Identity =
          SymbolIdentityRegistry::ComputeIdentity(Entry.Symbol))
    Entry.Identity = *Identity;

  ReflectionTypeFields Fields;
  Fields.Id = Type.Identity;
  Fields.Name = std::move(QualifiedName);
  Fields.Descriptor = Type.Descriptor;

  Entry.TypeFields = std::move(Fields);

  Entry.TypeConversion = std::move(Type);
  return Entry;
}

std::size_t DescriptorPlan::Append(DescriptorPlanEntry Entry) {
  Entries.push_back(std::move(Entry));
  return Entries.size() - 1;
}

DescriptorPlanEntry *DescriptorPlan::At(std::size_t Index) noexcept {
  return Index < Entries.size() ? &Entries[Index] : nullptr;
}

const DescriptorPlanEntry *
DescriptorPlan::At(std::size_t Index) const noexcept {
  return Index < Entries.size() ? &Entries[Index] : nullptr;
}

const DescriptorPlanEntry *
DescriptorPlan::Find(std::string_view QualifiedName) const noexcept {
  for (const DescriptorPlanEntry &Entry : Entries) {
    if (Entry.Symbol.QualifiedName == QualifiedName)
      return &Entry;
  }
  return nullptr;
}

const DescriptorPlanEntry *
DescriptorPlan::Find(const SymbolId &Identity) const noexcept {
  for (const DescriptorPlanEntry &Entry : Entries) {
    if (Entry.Identity == Identity)
      return &Entry;
  }
  return nullptr;
}

bool DescriptorPlan::Contains(std::string_view QualifiedName) const noexcept {
  return Find(QualifiedName) != nullptr;
}

std::size_t DescriptorPlan::CountOf(PlanEntryKind Category) const noexcept {
  std::size_t Result = 0;
  for (const DescriptorPlanEntry &Entry : Entries) {
    if (Entry.Category == Category)
      ++Result;
  }
  return Result;
}

bool PlanContributesReflection(const DescriptorPlan &Plan) noexcept {
  for (const DescriptorPlanEntry &Entry : Plan.PlannedEntries()) {
    if (Entry.Record || Entry.OverloadSetRecord || Entry.TypeFields ||
        Entry.ModuleFields)
      return true;
  }
  return false;
}

std::size_t PlannedReflectionRecordCount(const DescriptorPlan &Plan) noexcept {
  std::size_t Result = 0;
  for (const DescriptorPlanEntry &Entry : Plan.PlannedEntries()) {
    if (Entry.Record)
      ++Result;

    if (Entry.OverloadSetRecord)
      ++Result;
  }
  return Result;
}

std::vector<std::size_t> DescriptorPlan::CanonicalOrder() const {
  std::vector<std::size_t> Order(Entries.size());
  std::iota(Order.begin(), Order.end(), std::size_t{0});
  std::stable_sort(
      Order.begin(), Order.end(), [this](std::size_t Left, std::size_t Right) {
        return PlanEntryPrecedes(Entries[Left].Symbol, Entries[Left].Identity,
                                 Entries[Right].Symbol,
                                 Entries[Right].Identity);
      });
  return Order;
}

bool PlanEntryPrecedes(const SymbolDescriptor &Left,
                       const SymbolId &LeftIdentity,
                       const SymbolDescriptor &Right,
                       const SymbolId &RightIdentity) {
  if (const auto Order = CompareText(Left.QualifiedName, Right.QualifiedName);
      Order != std::strong_ordering::equal)
    return Order == std::strong_ordering::less;
  if (const auto Order = Left.Kind <=> Right.Kind;
      Order != std::strong_ordering::equal)
    return Order == std::strong_ordering::less;

  if (Left.Signature.has_value() != Right.Signature.has_value())
    return !Left.Signature.has_value();
  if (Left.Signature) {
    if (const auto Order = CompareSignature(*Left.Signature, *Right.Signature);
        Order != std::strong_ordering::equal)
      return Order == std::strong_ordering::less;
  }

  return LeftIdentity < RightIdentity;
}

} // namespace Luna::Detail
