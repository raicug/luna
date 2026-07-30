// clang-format off
#include "state/registration/class_plan.hpp"

#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/identity/symbol_descriptor.hpp"
#include "state/reflection/storage.hpp"
#include "state/registration/checks.hpp"
#include "state/registration/parameter_shape.hpp"
#include "state/registration/plan.hpp"
#include "state/registration/scope_plan.hpp"
#include "state/type/structural_types.hpp"
#include "state/type/type_record.hpp"
#include "state/userdata/class_operators.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr std::size_t MaximumClassByteCount = 1U << 20;

constexpr std::size_t MaximumClassAlignment = 64;

[[nodiscard]] std::string ClassSubject(const StagedClass &Declaration) {
  return SubjectText(SymbolKindText(SymbolKind::Class),
                     Declaration.QualifiedName);
}

[[nodiscard]] SymbolId IdentityOf(const SymbolDescriptor &Symbol) {
  if (const auto Identity = SymbolIdentityRegistry::ComputeIdentity(Symbol))
    return *Identity;
  return SymbolId();
}

[[nodiscard]] bool IsPowerOfTwo(std::size_t Value) noexcept {
  return Value != 0 && (Value & (Value - 1)) == 0;
}

[[nodiscard]] std::string
ConstructionSubject(const StagedConstruction &Declaration) {
  return SubjectText(SymbolKindText(Declaration.Kind),
                     Declaration.QualifiedName);
}

[[nodiscard]] std::string MethodSubject(const StagedMethod &Declaration) {
  return SubjectText(SymbolKindText(Declaration.Kind),
                     Declaration.QualifiedName);
}

[[nodiscard]] std::vector<ReflectionRelationFields>
DeclaredClassRelations(const StagedClass &Declaration, const TypeId &Own) {
  std::vector<ReflectionRelationFields> Related;

  std::vector<std::string> BaseKeys;
  for (const BaseRequest &Edge : Declaration.Relationships.Bases)
    BaseKeys.push_back(Edge.Base.Text());
  std::sort(BaseKeys.begin(), BaseKeys.end());
  for (const std::string &Key : BaseKeys) {
    const BaseRequest *Edge = nullptr;
    for (const BaseRequest &Candidate : Declaration.Relationships.Bases) {
      if (Candidate.Base.Text() == Key) {
        Edge = &Candidate;
        break;
      }
    }
    if (Edge == nullptr)
      continue;
    ReflectionRelationFields Fields;
    Fields.Kind = TypeRelationKind::Base;
    Fields.Type = ClassTypeIdentityOf(Edge->Base);
    Fields.Note = Edge->IsAccessible ? "accessible" : "inaccessible";
    if (Fields.Type.IsValid())
      Related.push_back(std::move(Fields));
  }

  std::vector<std::string> CastKeys;
  for (const CastRequest &Edge : Declaration.Relationships.Casts)
    CastKeys.push_back(Edge.Source.Text());
  std::sort(CastKeys.begin(), CastKeys.end());
  for (const std::string &Key : CastKeys) {
    const CastRequest *Edge = nullptr;
    for (const CastRequest &Candidate : Declaration.Relationships.Casts) {
      if (Candidate.Source.Text() == Key) {
        Edge = &Candidate;
        break;
      }
    }
    if (Edge == nullptr)
      continue;
    ReflectionRelationFields Fields;
    Fields.Kind = TypeRelationKind::Cast;
    Fields.Type = ClassTypeIdentityOf(Edge->Source);
    Fields.Note = Edge->Policy;
    if (Fields.Type.IsValid())
      Related.push_back(std::move(Fields));
  }

  if (!Own.IsValid())
    return Related;

  for (const ClassOperatorDescriptor &Described : ClassOperatorDescriptors()) {
    for (const StagedOperator &Staged : Declaration.Operators) {
      if (Staged.Selected != Described.Selected)
        continue;
      ReflectionRelationFields Fields;
      Fields.Kind = TypeRelationKind::Operand;
      Fields.Type = Own;
      Fields.Note = std::string(ClassOperatorText(Described.Selected)) + " " +
                    Staged.Segment;
      Related.push_back(std::move(Fields));
      break;
    }
  }
  return Related;
}

} // namespace

TypeDescriptor ClassTypeOf(const StagedClass &Declaration) {
  return TypeDescriptor::ForClass(Declaration.Key);
}

DescriptorPlanEntry MakeClassPlanEntry(const StagedClass &Declaration,
                                       SymbolId Parent) {
  DescriptorPlanEntry Entry;

  Entry.Category = PlanEntryKind::ClassSymbol;
  Entry.VmPath = Declaration.QualifiedName;

  const TypeDescriptor Type = ClassTypeOf(Declaration);
  Entry.Symbol = MakeClassMemberSymbol(SymbolKind::Class,
                                       Declaration.QualifiedName, Parent, Type);
  Entry.Identity = IdentityOf(Entry.Symbol);

  TypeRecord Declared =
      DeclareClassTypeRecord(Declaration.Key, Declaration.QualifiedName);

  ReflectionTypeFields TypeFields;
  TypeFields.Id = Declared.Identity;
  TypeFields.Name = Declaration.QualifiedName;
  TypeFields.Descriptor = Type;
  TypeFields.Declaration = Entry.Identity;
  Entry.TypeFields = std::move(TypeFields);
  Entry.TypeConversion = std::move(Declared);

  ReflectionRecordFields Record;
  Record.Kind = SymbolKind::Class;
  Record.Id = Entry.Identity;
  Record.Name = std::string(FinalSegment(Declaration.QualifiedName));
  Record.QualifiedName = Declaration.QualifiedName;
  Record.Scope = Parent.IsValid() ? ScopeId(Parent) : ScopeId::Root();
  Record.Declaration = Entry.Identity;
  Record.Type = Entry.TypeFields->Id;
  Record.Descriptor = Type;
  Record.Returns = ReturnShape::Zero;
  Record.Documentation = Declaration.Documentation;
  Record.Attributes = Declaration.Attributes;
  Record.Examples = Declaration.Examples;
  Record.Relations = DeclaredClassRelations(Declaration, Entry.TypeFields->Id);
  Entry.Record = std::move(Record);

  Entry.ClassStorage = Declaration.Policy;

  if (!Declaration.Relationships.IsEmpty())
    Entry.Relationships = Declaration.Relationships;
  return Entry;
}

DescriptorPlanEntry MakeClassMetatablePlanEntry(const StagedClass &Declaration,
                                                const SymbolId &ClassSymbol) {
  DescriptorPlanEntry Entry;

  Entry.Category = PlanEntryKind::Metatable;
  Entry.VmPath =
      JoinQualifiedName(Declaration.QualifiedName, ClassMetatableSegment);
  Entry.Symbol = MakeClassMemberSymbol(SymbolKind::Type, Entry.VmPath,
                                       ClassSymbol, ClassTypeOf(Declaration));
  Entry.Identity = IdentityOf(Entry.Symbol);
  return Entry;
}

DescriptorPlanEntry
MakeConstructionPlanEntry(const StagedClass &Class,
                          const StagedConstruction &Declaration,
                          const SymbolId &ClassSymbol) {
  DescriptorPlanEntry Entry;

  Entry.Category = PlanEntryKind::Function;
  Entry.VmPath = Declaration.QualifiedName;

  const TypeDescriptor Owner = ClassTypeOf(Class);
  const CallableMetadata &Metadata = Declaration.Callable->Metadata();
  const CallableSignatureDescriptor Signature =
      CanonicalFoundationSignature(Metadata);
  const std::string LocalName =
      std::string(FinalSegment(Declaration.QualifiedName));

  const SymbolDescriptor SetSymbol =
      MakeOverloadSetSymbol(Declaration.QualifiedName, ClassSymbol);
  const SymbolId SetIdentity = IdentityOf(SetSymbol);

  Entry.Symbol =
      MakeClassMemberSymbol(Declaration.Kind, Declaration.QualifiedName,
                            ClassSymbol, Owner, Signature);
  Entry.Identity = IdentityOf(Entry.Symbol);

  if (SetIdentity.IsValid()) {
    ReflectionRecordFields SetRecord;
    SetRecord.Kind = SymbolKind::OverloadSet;
    SetRecord.Id = SetIdentity;
    SetRecord.Name = LocalName;
    SetRecord.QualifiedName = Declaration.QualifiedName;
    SetRecord.Scope = ScopeId(ClassSymbol);
    Entry.OverloadSetRecord = std::move(SetRecord);
  }

  ReflectionRecordFields Record;
  Record.Kind = Declaration.Kind;
  Record.Id = Entry.Identity;
  Record.Name = LocalName;
  Record.QualifiedName = Declaration.QualifiedName;
  Record.Signature = CanonicalSignatureText(Signature);
  Record.Scope =
      SetIdentity.IsValid() ? ScopeId(SetIdentity) : ScopeId(ClassSymbol);
  Record.Declaration = ClassSymbol;
  Record.OverloadSet = SetIdentity;
  Record.Descriptor = Signature.ReturnType;
  if (const auto Identity =
          TypeIdentityRegistry::ComputeIdentity(Signature.ReturnType))
    Record.Type = *Identity;
  Record.Parameters = MakeReflectedParameters(Metadata);
  Record.ReturnValues = MakeReflectedReturns(Metadata);
  Record.Returns = ReflectedReturnShape(Metadata);

  Record.OwnershipResult =
      std::string(ConstructionOwnershipText(Declaration.Ownership));
  Record.AllocatorPolicy = ReflectedAllocatorPolicy(Class, Declaration);
  Record.Documentation = Declaration.Documentation;
  Record.Attributes = Declaration.Attributes;
  Record.Examples = Declaration.Examples;
  Entry.Record = std::move(Record);

  // A constructor, factory, or singleton declares parameters through the same
  // machinery a method does, so a converted parameter stages the same pending
  // type record here. Without this a construction entry compiled cleanly and
  // then failed preparation with an unavailable canonical type.
  Entry.ParameterTypeConversions = MakeParameterTypeConversions(Metadata);

  Entry.Callable.emplace(std::move(*Declaration.Callable));
  return Entry;
}

DescriptorPlanEntry MakeMethodPlanEntry(const StagedClass &Class,
                                        const StagedMethod &Declaration,
                                        const SymbolId &ClassSymbol) {
  DescriptorPlanEntry Entry;

  Entry.Category = PlanEntryKind::Function;
  Entry.VmPath = Declaration.QualifiedName;

  const TypeDescriptor Owner = ClassTypeOf(Class);
  const CallableMetadata &Metadata = Declaration.Callable->Metadata();

  const CallableSignatureDescriptor Signature =
      CanonicalFoundationSignature(Metadata);
  const std::string LocalName =
      std::string(FinalSegment(Declaration.QualifiedName));

  const SymbolDescriptor SetSymbol =
      MakeOverloadSetSymbol(Declaration.QualifiedName, ClassSymbol);
  const SymbolId SetIdentity = IdentityOf(SetSymbol);

  Entry.Symbol =
      MakeClassMemberSymbol(Declaration.Kind, Declaration.QualifiedName,
                            ClassSymbol, Owner, Signature);
  Entry.Identity = IdentityOf(Entry.Symbol);

  if (SetIdentity.IsValid()) {
    ReflectionRecordFields SetRecord;
    SetRecord.Kind = SymbolKind::OverloadSet;
    SetRecord.Id = SetIdentity;
    SetRecord.Name = LocalName;
    SetRecord.QualifiedName = Declaration.QualifiedName;
    SetRecord.Scope = ScopeId(ClassSymbol);
    Entry.OverloadSetRecord = std::move(SetRecord);
  }

  ReflectionRecordFields Record;
  Record.Kind = Declaration.Kind;
  Record.Id = Entry.Identity;
  Record.Name = LocalName;
  Record.QualifiedName = Declaration.QualifiedName;
  Record.Signature = CanonicalSignatureText(Signature);
  Record.Scope =
      SetIdentity.IsValid() ? ScopeId(SetIdentity) : ScopeId(ClassSymbol);
  Record.Declaration = ClassSymbol;
  Record.OverloadSet = SetIdentity;
  Record.Descriptor = Signature.ReturnType;
  if (const auto Identity =
          TypeIdentityRegistry::ComputeIdentity(Signature.ReturnType))
    Record.Type = *Identity;
  Record.Parameters = MakeReflectedParameters(Metadata);
  Record.ReturnValues = MakeReflectedReturns(Metadata);
  Record.Returns = ReflectedReturnShape(Metadata);
  Record.Documentation = Declaration.Documentation;
  Record.Attributes = Declaration.Attributes;
  Record.Examples = Declaration.Examples;
  Entry.Record = std::move(Record);

  Entry.ParameterTypeConversions = MakeParameterTypeConversions(Metadata);

  Entry.Callable.emplace(std::move(*Declaration.Callable));
  return Entry;
}

DescriptorPlanEntry MakeOperatorPlanEntry(const StagedClass &Class,
                                          const StagedOperator &Declaration,
                                          const SymbolId &ClassSymbol) {
  DescriptorPlanEntry Entry;

  Entry.Category = PlanEntryKind::Function;
  Entry.VmPath = Declaration.QualifiedName;

  const TypeDescriptor Owner = ClassTypeOf(Class);
  const CallableMetadata &Metadata = Declaration.Callable->Metadata();
  const CallableSignatureDescriptor Signature =
      CanonicalFoundationSignature(Metadata);
  const std::string LocalName =
      std::string(FinalSegment(Declaration.QualifiedName));

  const SymbolDescriptor SetSymbol =
      MakeOverloadSetSymbol(Declaration.QualifiedName, ClassSymbol);
  const SymbolId SetIdentity = IdentityOf(SetSymbol);

  Entry.Symbol =
      MakeClassMemberSymbol(SymbolKind::Operator, Declaration.QualifiedName,
                            ClassSymbol, Owner, Signature);
  Entry.Identity = IdentityOf(Entry.Symbol);

  if (SetIdentity.IsValid()) {
    ReflectionRecordFields SetRecord;
    SetRecord.Kind = SymbolKind::OverloadSet;
    SetRecord.Id = SetIdentity;
    SetRecord.Name = LocalName;
    SetRecord.QualifiedName = Declaration.QualifiedName;
    SetRecord.Scope = ScopeId(ClassSymbol);
    Entry.OverloadSetRecord = std::move(SetRecord);
  }

  ReflectionRecordFields Record;
  Record.Kind = SymbolKind::Operator;
  Record.Id = Entry.Identity;
  Record.Name = LocalName;
  Record.QualifiedName = Declaration.QualifiedName;
  Record.Signature = std::string(ClassOperatorText(Declaration.Selected)) +
                     " " + CanonicalSignatureText(Signature);
  Record.Scope =
      SetIdentity.IsValid() ? ScopeId(SetIdentity) : ScopeId(ClassSymbol);
  Record.Declaration = ClassSymbol;
  Record.OverloadSet = SetIdentity;
  Record.Descriptor = Signature.ReturnType;
  if (const auto Identity =
          TypeIdentityRegistry::ComputeIdentity(Signature.ReturnType))
    Record.Type = *Identity;
  Record.Parameters = MakeReflectedParameters(Metadata);
  Record.ReturnValues = MakeReflectedReturns(Metadata);
  Record.Returns = ReflectedReturnShape(Metadata);
  Record.Documentation = Declaration.Documentation;
  Record.Attributes = Declaration.Attributes;
  Record.Examples = Declaration.Examples;
  Entry.Record = std::move(Record);

  Entry.ParameterTypeConversions = MakeParameterTypeConversions(Metadata);

  PlannedClassOperator Published;
  Published.Selected = Declaration.Selected;
  Published.Segment = Declaration.Segment;
  Entry.OperatorFields = std::move(Published);

  Entry.Callable.emplace(std::move(*Declaration.Callable));
  return Entry;
}

std::string ReflectedAllocatorPolicy(const StagedClass &Class,
                                     const StagedConstruction &Declaration) {
  const bool CreatesStorage =
      Declaration.AllocatorPolicy == ConstructedStoragePolicyName;
  if (Class.SelectsStorage && CreatesStorage)
    return std::string(Class.Storage.PolicyIdentity());
  return Declaration.AllocatorPolicy;
}

StagedConstruction *FindStagedConstruction(StagedClass &Declaration,
                                           std::string_view Segment) {
  for (StagedConstruction &Staged : Declaration.Constructions) {
    if (Staged.Segment == Segment)
      return &Staged;
  }
  return nullptr;
}

StagedMethod *FindStagedMethod(StagedClass &Declaration,
                               std::string_view Segment) {
  for (StagedMethod &Staged : Declaration.Methods) {
    if (Staged.Segment == Segment)
      return &Staged;
  }
  return nullptr;
}

std::optional<ErrorDiagnostic>
ValidateStagedMethod(const StagedClass &Class,
                     const StagedMethod &Declaration) {
  const std::string Subject = MethodSubject(Declaration);

  if (!Declaration.Refusal.empty())
    return MalformedMetadataDiagnostic(Subject, Declaration.Refusal);
  if (!Declaration.HasTarget())
    return NullCallableDiagnostic(Subject);

  if (Declaration.Callable->Metadata().ReturnType().IsAsynchronous())
    return MalformedMetadataDiagnostic(
        Subject, "asynchronous delivery is available on namespace and root "
                 "functions, so a class member returns its value directly.");

  const ReceiverMetadata *Receiver =
      Declaration.Callable->Metadata().Receiver();
  if (Declaration.DeclaresReceiver != (Receiver != nullptr))
    return MalformedMetadataDiagnostic(
        Subject, "the declared receiver and the callable metadata disagree "
                 "about whether this member operates on an instance.");
  if (Receiver) {
    if (!(Receiver->Class() == Class.Key))
      return MalformedMetadataDiagnostic(
          Subject, "this member declares a receiver of another class than the "
                   "one it is declared in.");
    if (Receiver->IsConst() != Declaration.ReceiverIsConst)
      return MalformedMetadataDiagnostic(
          Subject, "the declared receiver constness and the callable metadata "
                   "disagree.");
  }

  for (const StagedMethod &Existing : Class.Methods) {
    if (&Existing == &Declaration || Existing.Segment != Declaration.Segment)
      continue;
    if (Existing.DeclaresReceiver != Declaration.DeclaresReceiver)
      return MalformedMetadataDiagnostic(
          Subject, "'" + Declaration.Segment +
                       "' is already declared in this class with a different "
                       "member category; one member name is either an instance "
                       "member or a static member, never both.");
  }
  return std::nullopt;
}

std::optional<ErrorDiagnostic>
ValidateStagedConstruction(const StagedClass &Class,
                           const StagedConstruction &Declaration) {
  const std::string Subject = ConstructionSubject(Declaration);

  if (!Declaration.Refusal.empty())
    return MalformedMetadataDiagnostic(Subject, Declaration.Refusal);
  if (!Declaration.HasTarget())
    return NullCallableDiagnostic(Subject);

  if (Declaration.Ownership == ConstructionOwnership::LuaOwned &&
      Class.Policy.IsAbstract)
    return MalformedMetadataDiagnostic(
        Subject, "this class is abstract, so Luna could never construct a "
                 "value of it.");
  if (Declaration.AllocatorPolicy.empty())
    return MalformedMetadataDiagnostic(
        Subject, "the declaration names no allocator policy.");
  return std::nullopt;
}

std::optional<ErrorDiagnostic>
ValidateStagedClass(const StagedClass &Declaration) {
  const std::string Subject = ClassSubject(Declaration);

  if (!Declaration.Key.IsValid())
    return MalformedMetadataDiagnostic(
        Subject,
        "the stable type key '" + Declaration.Key.Text() + "' is not valid (" +
            std::string(StableTypeKeyStatusText(Declaration.Key.Status())) +
            ").");

  if (Declaration.Policy.ByteCount == 0)
    return MalformedMetadataDiagnostic(
        Subject, "the declared storage size of this class is zero.");
  if (Declaration.Policy.ByteCount > MaximumClassByteCount)
    return ValueOutOfRangeDiagnostic(
        Subject, "the declared storage size of one registered class",
        static_cast<long long>(Declaration.Policy.ByteCount), 1,
        static_cast<long long>(MaximumClassByteCount));
  if (!IsPowerOfTwo(Declaration.Policy.Alignment))
    return MalformedMetadataDiagnostic(
        Subject, "the declared storage alignment of this class is not a power "
                 "of two.");
  if (Declaration.Policy.Alignment > MaximumClassAlignment)
    return ValueOutOfRangeDiagnostic(
        Subject, "the declared storage alignment of one registered class",
        static_cast<long long>(Declaration.Policy.Alignment), 1,
        static_cast<long long>(MaximumClassAlignment));

  if (!Declaration.Policy.IsDestructible)
    return MalformedMetadataDiagnostic(
        Subject, "this class is not destructible, so Luna could never release "
                 "a value of it.");
  return std::nullopt;
}

std::optional<ErrorDiagnostic>
ValidateSelectedClassStorage(const StagedClass &Declaration,
                             const ClassAllocator &Storage) {
  const std::string Subject = ClassSubject(Declaration);

  if (!Storage.IsDeclared())
    return MalformedMetadataDiagnostic(
        Subject, "the selected allocator names no storage protocol at all.");
  if (Storage.PolicyIdentity().empty())
    return MalformedMetadataDiagnostic(
        Subject, "the selected allocator names no policy identity, so no "
                 "candidate of this class could reflect the storage it came "
                 "from.");
  if (!Storage.DeclaresAllocation())
    return MalformedMetadataDiagnostic(
        Subject, "the selected allocator declares no allocation step, so Luna "
                 "could never obtain storage for a value of this class.");
  if (!Storage.DeclaresDestruction())
    return MalformedMetadataDiagnostic(
        Subject, "the selected allocator declares no destruction step, so Luna "
                 "could never destroy a value it constructed.");
  if (!Storage.OwnsStorage())
    return MalformedMetadataDiagnostic(
        Subject,
        "the selected allocator declares no deallocation step, so Luna "
        "could never give back the storage it allocated.");

  const StorageRequest Requested = Storage.Storage();
  if (!Requested.IsUsable())
    return MalformedMetadataDiagnostic(
        Subject, "the selected allocator names no usable storage size and "
                 "power-of-two alignment.");
  if (Requested.ByteCount < Declaration.Policy.ByteCount)
    return ValueOutOfRangeDiagnostic(
        Subject, "the storage size of the selected allocator",
        static_cast<long long>(Requested.ByteCount),
        static_cast<long long>(Declaration.Policy.ByteCount),
        static_cast<long long>(MaximumClassByteCount));
  if (Requested.Alignment < Declaration.Policy.Alignment)
    return ValueOutOfRangeDiagnostic(
        Subject, "the storage alignment of the selected allocator",
        static_cast<long long>(Requested.Alignment),
        static_cast<long long>(Declaration.Policy.Alignment),
        static_cast<long long>(MaximumClassAlignment));
  return std::nullopt;
}

} // namespace Luna::Detail
