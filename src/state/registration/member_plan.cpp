// clang-format off
#include "state/registration/member_plan.hpp"

#include <luna/binding/class_member.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/identity/symbol_descriptor.hpp"
#include "state/reflection/storage.hpp"
#include "state/registration/checks.hpp"
#include "state/registration/plan.hpp"
#include "state/registration/scope_plan.hpp"
#include "state/type/type_record.hpp"
#include "state/userdata/class_operators.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] SymbolId IdentityOf(const SymbolDescriptor &Symbol) {
  if (const auto Identity = SymbolIdentityRegistry::ComputeIdentity(Symbol))
    return *Identity;
  return SymbolId();
}

[[nodiscard]] std::string MemberSubject(const StagedMember &Declaration) {
  return SubjectText(SymbolKindText(Declaration.Kind),
                     Declaration.QualifiedName);
}

} // namespace

bool IsReservedMemberName(std::string_view Segment) noexcept {
  return Segment.size() >= ReservedMemberPrefix.size() &&
         Segment.substr(0, ReservedMemberPrefix.size()) == ReservedMemberPrefix;
}

std::string_view MemberCollisionText(MemberCollision Collision) noexcept {
  switch (Collision) {
  case MemberCollision::None:
    return "none";
  case MemberCollision::ReservedSystemName:
    return "reserved_system_name";
  case MemberCollision::SameCategory:
    return "same_category";
  case MemberCollision::IncompatibleCategory:
    return "incompatible_category";
  case MemberCollision::InheritedAmbiguity:
    return "inherited_ambiguity";
  }
  return "none";
}

StagedMember *FindStagedClassMember(std::vector<StagedMember> &Members,
                                    std::string_view Segment) {
  for (StagedMember &Staged : Members) {
    if (Staged.Segment == Segment)
      return &Staged;
  }
  return nullptr;
}

DescriptorPlanEntry MakeMemberPlanEntry(const StagedMember &Declaration,
                                        const TypeDescriptor &OwnerType,
                                        const SymbolId &ClassSymbol) {
  DescriptorPlanEntry Entry;

  Entry.Category = PlanEntryKind::ClassMember;
  Entry.VmPath = Declaration.QualifiedName;

  Entry.Symbol = MakeClassMemberSymbol(
      Declaration.Kind, Declaration.QualifiedName, ClassSymbol, OwnerType);
  Entry.Identity = IdentityOf(Entry.Symbol);

  ReflectionRecordFields Record;
  Record.Kind = Declaration.Kind;
  Record.Id = Entry.Identity;
  Record.Name = std::string(FinalSegment(Declaration.QualifiedName));
  Record.QualifiedName = Declaration.QualifiedName;
  Record.Scope = ScopeId(ClassSymbol);
  Record.Declaration = ClassSymbol;
  Record.Descriptor = Declaration.ValueType;
  if (const auto Identity =
          TypeIdentityRegistry::ComputeIdentity(Declaration.ValueType))
    Record.Type = *Identity;
  Record.Returns = ReturnShape::Zero;

  if (const auto Receiver =
          TypeIdentityRegistry::ComputeIdentity(Declaration.ReceiverType))
    Record.ReceiverType = *Receiver;
  Record.ReceiverPermitsConst = PermitsMemberRead(Declaration.Access) &&
                                !Declaration.ReadRequiresMutableReceiver;
  Record.MemberIsReadable = PermitsMemberRead(Declaration.Access);
  Record.MemberIsWritable = PermitsMemberWrite(Declaration.Access);
  Record.MemberAccessText = std::string(MemberAccessText(Declaration.Access));
  Record.MemberEvaluationText =
      std::string(PropertyEvaluationText(Declaration.Evaluation));
  Record.MemberOwnershipText =
      std::string(MemberOwnershipText(Declaration.Ownership));

  Record.Signature = std::string(SymbolKindText(Declaration.Kind)) + " " +
                     CanonicalTypeText(Declaration.ValueType) + "[" +
                     CanonicalTypeText(Declaration.ReceiverType) + "]";

  Record.Documentation = Declaration.Documentation;
  Record.Attributes = Declaration.Attributes;
  Record.Examples = Declaration.Examples;
  Entry.Record = std::move(Record);

  PlannedClassMember Member;
  Member.Kind = Declaration.Kind;
  Member.Access = Declaration.Access;
  Member.Evaluation = Declaration.Evaluation;
  Member.Ownership = Declaration.Ownership;
  Member.ValueType = Entry.Record->Type;
  Member.ValueDescriptor = Declaration.ValueType;
  Member.ReadRequiresMutableReceiver = Declaration.ReadRequiresMutableReceiver;
  Member.Read = Declaration.Read;
  Member.Write = Declaration.Write;
  Member.Change = Declaration.Change;
  Entry.ClassMember = std::move(Member);
  return Entry;
}

std::optional<ErrorDiagnostic>
ValidateStagedMember(const StagedMember &Declaration) {
  const std::string Subject = MemberSubject(Declaration);

  if (!Declaration.Refusal.empty())
    return MalformedMetadataDiagnostic(Subject, Declaration.Refusal);

  if (!Declaration.ValueType.IsValid())
    return MalformedMetadataDiagnostic(
        Subject, "the declared value type of this member is not one Luna can "
                 "carry across the member boundary.");
  if (!Declaration.ReceiverType.IsValid())
    return MalformedMetadataDiagnostic(
        Subject, "this member names no receiver type at all.");

  if (PermitsMemberRead(Declaration.Access) && !Declaration.HasReader())
    return MalformedMetadataDiagnostic(
        Subject, "this member permits reads but generated no getter.");
  if (PermitsMemberWrite(Declaration.Access) && !Declaration.HasWriter())
    return MalformedMetadataDiagnostic(
        Subject, "this member permits writes but generated no setter.");
  if (!PermitsMemberRead(Declaration.Access) && Declaration.HasReader())
    return MalformedMetadataDiagnostic(
        Subject, "this member generated a getter but permits no reads.");
  if (!PermitsMemberWrite(Declaration.Access) && Declaration.HasWriter())
    return MalformedMetadataDiagnostic(
        Subject, "this member generated a setter but permits no writes.");

  if (Declaration.Evaluation != PropertyEvaluation::Immediate &&
      !PermitsMemberRead(Declaration.Access))
    return MalformedMetadataDiagnostic(
        Subject, "a computed or lazy member must permit reads.");
  if (Declaration.Kind == SymbolKind::Field &&
      Declaration.Evaluation != PropertyEvaluation::Immediate)
    return MalformedMetadataDiagnostic(
        Subject, "a field holds the value the object already has, so it never "
                 "declares a computed or lazy evaluation.");

  if (Declaration.Kind == SymbolKind::Field &&
      Declaration.Ownership != MemberOwnership::Copied)
    return MalformedMetadataDiagnostic(
        Subject, "a Luna field copies its declared value across the member "
                 "boundary, so it cannot declare another ownership.");
  return std::nullopt;
}

MemberCollision
ClassifyMemberCollision(const MemberCollisionRequest &Request) noexcept {
  if (IsReservedMemberName(Request.Segment))
    return MemberCollision::ReservedSystemName;

  if (Request.NameIsDeclared) {
    if (Request.ExistingCategory == PlanEntryKind::ClassMember &&
        Request.ExistingKind == Request.Kind)
      return MemberCollision::SameCategory;
    return MemberCollision::IncompatibleCategory;
  }

  if (Request.InheritedNameIsAmbiguous)
    return MemberCollision::InheritedAmbiguity;
  return MemberCollision::None;
}

std::optional<ErrorDiagnostic>
DiagnoseMemberCollision(const MemberCollisionRequest &Request) {
  const std::string Subject =
      SubjectText(SymbolKindText(Request.Kind), Request.QualifiedName);

  switch (ClassifyMemberCollision(Request)) {
  case MemberCollision::None:
    return std::nullopt;
  case MemberCollision::ReservedSystemName:
    if (const ReservedMetamethod *Owned =
            FindReservedMetamethod(Request.Segment))
      return ReservedMetamethodDiagnostic(
          Subject, Request.Segment, ReservedMetamethodRoleText(Owned->Role));
    return ReservedMemberNameDiagnostic(Subject, Request.Segment);
  case MemberCollision::SameCategory:
    return DuplicateNameDiagnostic(Subject);
  case MemberCollision::IncompatibleCategory:
    return IncompatibleCategoryDiagnostic(
        Subject,
        Request.ExistingCategory == PlanEntryKind::ClassMember
            ? SymbolKindText(Request.ExistingKind)
            : PlanEntryKindText(Request.ExistingCategory),
        Request.ExistingIsPending);
  case MemberCollision::InheritedAmbiguity:
    return AmbiguousInheritedMemberDiagnostic(Subject, Request.Segment);
  }
  return std::nullopt;
}

} // namespace Luna::Detail
