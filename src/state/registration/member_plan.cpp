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

  // A member is its own category: it installs no virtual-machine value of its
  // own, because it is reached through the class rather than published at a
  // path. It still travels through the same validation, the same journal, and
  // the same publication decision as every other declaration.
  Entry.Category = PlanEntryKind::ClassMember;
  Entry.VmPath = Declaration.QualifiedName;

  // A property and a field carry no canonical signature: one member is one
  // symbol with a declared value type and a declared receiver, not one callable
  // candidate per direction.
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

  // What a consumer and a generator need to know about one member: the receiver
  // it is reached through, whether that receiver may be const, which directions
  // it permits, and when its value is produced. Cache state is deliberately
  // absent: the lazy policy is metadata, the cached value is not.
  if (const auto Receiver =
          TypeIdentityRegistry::ComputeIdentity(Declaration.ReceiverType))
    Record.ReceiverType = *Receiver;
  // A const receiver is enough for this member's reads when the declared getter
  // itself only reads the object. Writes always need a mutable receiver, which
  // is what the writability flag already says.
  Record.ReceiverPermitsConst = PermitsMemberRead(Declaration.Access) &&
                                !Declaration.ReadRequiresMutableReceiver;
  Record.MemberIsReadable = PermitsMemberRead(Declaration.Access);
  Record.MemberIsWritable = PermitsMemberWrite(Declaration.Access);
  Record.MemberAccessText = std::string(MemberAccessText(Declaration.Access));
  Record.MemberEvaluationText =
      std::string(PropertyEvaluationText(Declaration.Evaluation));
  Record.MemberOwnershipText =
      std::string(MemberOwnershipText(Declaration.Ownership));

  // The canonical member signature travels with the record so two members of
  // one class never reflect one identical ordering key.
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
  Entry.ClassMember = std::move(Member);
  return Entry;
}

std::optional<ErrorDiagnostic>
ValidateStagedMember(const StagedMember &Declaration) {
  const std::string Subject = MemberSubject(Declaration);

  // A refusal the declaration itself recorded ranks first: a policy that
  // contradicts its own accessors is a description mistake, not a target one.
  if (!Declaration.Refusal.empty())
    return MalformedMetadataDiagnostic(Subject, Declaration.Refusal);

  // Luna copies one supported value across the member boundary, so a declared
  // type it could not carry is refused here instead of at the first access.
  if (!Declaration.ValueType.IsValid())
    return MalformedMetadataDiagnostic(
        Subject, "the declared value type of this member is not one Luna can "
                 "carry across the member boundary.");
  if (!Declaration.ReceiverType.IsValid())
    return MalformedMetadataDiagnostic(
        Subject, "this member names no receiver type at all.");

  // Every direction the member permits needs the descriptor that performs it,
  // and every descriptor needs a direction that permits it.
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

  // A computed or lazy value has to be readable, and a field is always the
  // immediate value the object holds rather than a computed one.
  if (Declaration.Evaluation != PropertyEvaluation::Immediate &&
      !PermitsMemberRead(Declaration.Access))
    return MalformedMetadataDiagnostic(
        Subject, "a computed or lazy member must permit reads.");
  if (Declaration.Kind == SymbolKind::Field &&
      Declaration.Evaluation != PropertyEvaluation::Immediate)
    return MalformedMetadataDiagnostic(
        Subject, "a field holds the value the object already has, so it never "
                 "declares a computed or lazy evaluation.");

  // A field copies its value across the boundary, so no reference into an
  // object Luna does not own can escape through it.
  if (Declaration.Kind == SymbolKind::Field &&
      Declaration.Ownership != MemberOwnership::Copied)
    return MalformedMetadataDiagnostic(
        Subject, "a Luna field copies its declared value across the member "
                 "boundary, so it cannot declare another ownership.");
  return std::nullopt;
}

MemberCollision
ClassifyMemberCollision(const MemberCollisionRequest &Request) noexcept {
  // 1. Luna's own metamethod and system namespace. A member may never take it,
  // whichever category the request names.
  if (IsReservedMemberName(Request.Segment))
    return MemberCollision::ReservedSystemName;

  // 2. and 3. A name this class already declares: the same category is a
  // duplicate, and any other category is an incompatible-category collision.
  if (Request.NameIsDeclared) {
    if (Request.ExistingCategory == PlanEntryKind::ClassMember &&
        Request.ExistingKind == Request.Kind)
      return MemberCollision::SameCategory;
    return MemberCollision::IncompatibleCategory;
  }

  // 4. One name reachable from more than one base. Unreachable until base edges
  // exist, so this arm fixes the position rather than the behavior.
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
    // A metamethod Luna installs itself names the behavior it carries, so the
    // refusal says which promise the declaration would have replaced.
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
