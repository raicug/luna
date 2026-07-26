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

// Luna's own storage policy for one exposed object. A class whose declared
// storage is larger than this is refused rather than allocated, so the bound is
// explicit instead of implied by the allocator.
constexpr std::size_t MaximumClassByteCount = 1U << 20;

// Luna's own alignment policy. Storage more strictly aligned than this is
// refused, because Luna would have to guess how to obtain it.
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

// The declared relationships and operators of one class, as canonically ordered
// reflection relations. A base and a cast are named by the canonical type of
// the class they connect to, and an operator by the class's own type plus the
// operator it answers, so no relation ever clones the record that declares it.
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

  // Canonical operator order is the order Luna supports the operators in, never
  // the order the class declared them.
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

  // A class is its own category: it owns one Luna table at exactly the path its
  // qualified name names, and every later member declares itself inside it.
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

  // The declared storage shape travels with the declaration, so publication
  // records it without ever seeing the consumer's type again.
  Entry.ClassStorage = Declaration.Policy;

  if (!Declaration.Relationships.IsEmpty())
    Entry.Relationships = Declaration.Relationships;
  return Entry;
}

DescriptorPlanEntry MakeClassMetatablePlanEntry(const StagedClass &Declaration,
                                                const SymbolId &ClassSymbol) {
  DescriptorPlanEntry Entry;

  // The metatable identity of the class. It publishes no reflection record,
  // because a metatable is Luna's own private identity rather than part of the
  // declared surface a consumer or a generator reads.
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

  // A construction candidate is a callable candidate: the same category, the
  // same virtual-machine path shape, the same installation, and the same
  // rollback journal every other callable uses.
  Entry.Category = PlanEntryKind::Function;
  Entry.VmPath = Declaration.QualifiedName;

  const TypeDescriptor Owner = ClassTypeOf(Class);
  const CallableMetadata &Metadata = Declaration.Callable->Metadata();
  const CallableSignatureDescriptor Signature =
      CanonicalFoundationSignature(Metadata);
  const std::string LocalName =
      std::string(FinalSegment(Declaration.QualifiedName));

  // The overload set of the member name. Its identity depends only on the name
  // and the class scope, never on which candidate declared it first.
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

  // What a consumer and a generator need to know about the result: how the
  // produced object will be owned, and which allocator policy produced its
  // storage. A class that selected its own protocol is reflected by every
  // candidate that creates its values through it, whichever order the two were
  // declared in.
  Record.OwnershipResult =
      std::string(ConstructionOwnershipText(Declaration.Ownership));
  Record.AllocatorPolicy = ReflectedAllocatorPolicy(Class, Declaration);
  Record.Documentation = Declaration.Documentation;
  Record.Attributes = Declaration.Attributes;
  Record.Examples = Declaration.Examples;
  Entry.Record = std::move(Record);

  Entry.Callable.emplace(std::move(*Declaration.Callable));
  return Entry;
}

DescriptorPlanEntry MakeMethodPlanEntry(const StagedClass &Class,
                                        const StagedMethod &Declaration,
                                        const SymbolId &ClassSymbol) {
  DescriptorPlanEntry Entry;

  // A member candidate is a callable candidate: the same category, the same
  // virtual-machine path shape, the same installation, and the same rollback
  // journal every other callable uses.
  Entry.Category = PlanEntryKind::Function;
  Entry.VmPath = Declaration.QualifiedName;

  const TypeDescriptor Owner = ClassTypeOf(Class);
  const CallableMetadata &Metadata = Declaration.Callable->Metadata();

  // The receiver travels in the canonical signature, so an instance method and
  // a static method of one name are told apart by the receiver alone.
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

  PlannedClassOperator Published;
  Published.Selected = Declaration.Selected;
  Published.Segment = Declaration.Segment;
  Entry.OperatorFields = std::move(Published);

  Entry.Callable.emplace(std::move(*Declaration.Callable));
  return Entry;
}

std::string ReflectedAllocatorPolicy(const StagedClass &Class,
                                     const StagedConstruction &Declaration) {
  // Only a candidate whose object Luna creates itself is allocated through the
  // class's protocol; an adopted object was never Luna's to allocate.
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

  // A refusal the declaration itself recorded ranks first, exactly as it does
  // for a construction candidate.
  if (!Declaration.Refusal.empty())
    return MalformedMetadataDiagnostic(Subject, Declaration.Refusal);
  if (!Declaration.HasTarget())
    return NullCallableDiagnostic(Subject);

  // What the candidate declares and what its metadata says must agree: an
  // instance method describes the class it operates on, and a static method
  // describes no receiver at all.
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

  // One member name is either an instance member or a static one, never both:
  // a call site cannot tell the two apart by anything other than the receiver,
  // and a receiver is exactly what would already have been validated or refused
  // before the ordinary arguments were even inspected.
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

  // A refusal the declaration itself recorded ranks first: an incompatible
  // explicit ownership policy is a description mistake, not a target mistake.
  if (!Declaration.Refusal.empty())
    return MalformedMetadataDiagnostic(Subject, Declaration.Refusal);
  if (!Declaration.HasTarget())
    return NullCallableDiagnostic(Subject);

  // Luna only owns an object it can also destroy, and the class declaration
  // already refuses a class it could never destroy; an ownership result that
  // needs Luna-owned storage additionally needs a storage shape Luna could
  // allocate.
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

  // A user-defined leaf type is accepted only with an explicit validated stable
  // key; the reserved Luna prefix is never available to one.
  if (!Declaration.Key.IsValid())
    return MalformedMetadataDiagnostic(
        Subject,
        "the stable type key '" + Declaration.Key.Text() + "' is not valid (" +
            std::string(StableTypeKeyStatusText(Declaration.Key.Status())) +
            ").");

  // The declared storage shape is what Luna allocates a Lua-owned value with,
  // so a shape Luna cannot honor is refused here instead of at allocation.
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

  // Luna releases every value it owns exactly once, so a class it could never
  // destroy is refused rather than exposed with an unreachable release path.
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

  // The steps a protocol declares are the whole cleanup rule, so a protocol
  // Luna could allocate through but never give storage back through, or
  // construct through but never destroy through, is refused here rather than
  // leaking one value per call.
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

  // The declared storage shape of the class is what a value of it occupies, so
  // a protocol that hands out less, or less strictly aligned, storage than that
  // is refused before anything is ever constructed in it.
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
