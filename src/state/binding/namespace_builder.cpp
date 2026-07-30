// clang-format off
#include "state/binding/namespace_builder.hpp"

#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/constant_value.hpp>
#include <luna/binding/enum_builder.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/binding/state_handle.hpp"
#include "state/impl.hpp"
#include "state/module/load.hpp"
#include "state/registration/checks.hpp"
#include "state/registration/class_plan.hpp"
#include "state/registration/plan.hpp"
#include "state/registration/scope_plan.hpp"
#include "state/registration/value_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna {
namespace Detail {
namespace {

[[nodiscard]] std::string EnumerationSubjectOf(std::string_view QualifiedName) {
  return SubjectText(SymbolKindText(SymbolKind::Enumeration), QualifiedName);
}

[[nodiscard]] StagedEnumerator *FindStagedMember(StagedEnumeration &Declaration,
                                                 std::string_view Member) {
  for (StagedEnumerator &Staged : Declaration.Enumerators) {
    if (Staged.Segment == Member)
      return &Staged;
  }
  return nullptr;
}

[[nodiscard]] ErrorDiagnostic
UndeclaredMemberDiagnostic(const StagedEnumeration &Declaration,
                           std::string_view Member) {
  return MalformedMetadataDiagnostic(
      EnumerationSubjectOf(Declaration.QualifiedName),
      "'" + std::string(Member) +
          "' is not a declared enumerator or alias of this enumeration; "
          "declare it before documenting or annotating it.");
}

[[nodiscard]] ErrorDiagnostic
UndeclaredClassMemberDiagnostic(const StagedClass &Declaration,
                                std::string_view Member) {
  return MalformedMetadataDiagnostic(
      SubjectText(SymbolKindText(SymbolKind::Class), Declaration.QualifiedName),
      "'" + std::string(Member) +
          "' is not a declared member of this class; declare it before "
          "documenting or annotating it.");
}

} // namespace

std::string_view BuilderHandleStatusText(BuilderHandleStatus Status) noexcept {
  switch (Status) {
  case BuilderHandleStatus::Usable:
    return "usable";
  case BuilderHandleStatus::OwnerDestroyed:
    return "its State was destroyed";
  case BuilderHandleStatus::OwnerMoved:
    return "its State moved to another owner object";
  case BuilderHandleStatus::DifferentState:
    return "its owner object now holds a different State";
  case BuilderHandleStatus::ReplacedGeneration:
    return "its scope belongs to a replaced lifecycle generation";
  case BuilderHandleStatus::ForeignThread:
    return "it was used outside the State's owner thread";
  case BuilderHandleStatus::Frozen:
    return "its State is frozen and rejects registration";
  case BuilderHandleStatus::NotReady:
    return "its State is not ready";
  }
  return "it is no longer usable";
}

bool IsFatalHandleStatus(BuilderHandleStatus Status) noexcept {
  switch (Status) {
  case BuilderHandleStatus::OwnerDestroyed:
  case BuilderHandleStatus::OwnerMoved:
  case BuilderHandleStatus::DifferentState:
  case BuilderHandleStatus::ReplacedGeneration:
    return true;
  default:
    return false;
  }
}

std::shared_ptr<NamespaceBuilderState>
NamespaceBuilderState::Create(State &Owner) {
  return std::shared_ptr<NamespaceBuilderState>(
      new NamespaceBuilderState(Owner));
}

NamespaceBuilder
NamespaceBuilderState::MakeBuilder(std::shared_ptr<NamespaceBuilderState> Plan,
                                   std::size_t ScopeNode) {
  return NamespaceBuilder(std::move(Plan), ScopeNode);
}

NamespaceBuilderState::NamespaceBuilderState(State &Owner) noexcept
    : Owner(&Owner) {
  if (const State::Impl *Implementation = Owner.Implementation.get()) {
    Handle = Implementation->HandleToken();
    Identity = Implementation->LogicalIdentity();
    OwnerEpoch = Implementation->OwnerEpoch();
    LifecycleGeneration = Implementation->LifecycleGeneration();
  }
}

State *NamespaceBuilderState::LiveOwner() const noexcept {
  const std::shared_ptr<StateHandleToken> Token = Handle.lock();
  if (!Token || Token->Owner != Owner || Token->OwnerEpoch != OwnerEpoch)
    return nullptr;
  return Owner;
}

BuilderHandleStatus NamespaceBuilderState::Classify() const noexcept {
  const std::shared_ptr<StateHandleToken> Token = Handle.lock();
  if (!Token)
    return BuilderHandleStatus::OwnerDestroyed;
  if (Token->Owner != Owner)
    return BuilderHandleStatus::OwnerMoved;
  if (!(Token->Identity == Identity))
    return BuilderHandleStatus::DifferentState;
  if (Token->OwnerEpoch != OwnerEpoch)
    return BuilderHandleStatus::OwnerMoved;

  const State::Impl *Implementation =
      Owner ? Owner->Implementation.get() : nullptr;
  if (!Implementation)
    return BuilderHandleStatus::OwnerDestroyed;

  if (!Implementation->IsOwnerThread())
    return BuilderHandleStatus::ForeignThread;

  if (!(Implementation->LogicalIdentity() == Identity))
    return BuilderHandleStatus::DifferentState;
  if (Implementation->LifecycleGeneration() != LifecycleGeneration)
    return BuilderHandleStatus::ReplacedGeneration;
  if (!Implementation->IsReady())
    return BuilderHandleStatus::NotReady;
  if (Implementation->IsFrozen())
    return BuilderHandleStatus::Frozen;
  return BuilderHandleStatus::Usable;
}

ErrorDiagnostic
NamespaceBuilderState::StaleDiagnostic(BuilderHandleStatus Status,
                                       std::string_view QualifiedName) const {
  const std::string Subject =
      SubjectText(PlanEntryKindText(PlanEntryKind::Scope),
                  QualifiedName.empty() ? std::string_view("the root scope")
                                        : QualifiedName);
  return StaleBuilderDiagnostic(Subject, BuilderHandleStatusText(Status));
}

void NamespaceBuilderState::RecordFailure(ErrorDiagnostic Diagnostic) {
  if (!Failure)
    Failure = std::move(Diagnostic);
}

bool NamespaceBuilderState::CanStage(std::size_t ScopeNode) {
  const BuilderHandleStatus Status = Classify();
  if (Status == BuilderHandleStatus::ForeignThread) {
    const std::string_view QualifiedName = QualifiedNameOf(ScopeNode);
    RecordFailure(ForeignThreadDiagnostic(
        SubjectText(PlanEntryKindText(PlanEntryKind::Scope),
                    QualifiedName.empty() ? std::string_view("the root scope")
                                          : QualifiedName)));
    return false;
  }
  if (IsFatalHandleStatus(Status)) {
    RecordFailure(StaleDiagnostic(Status, QualifiedNameOf(ScopeNode)));
    return false;
  }
  return true;
}

std::size_t NamespaceBuilderState::Stage(std::size_t ScopeNode,
                                         std::string_view Segment) {
  const std::string_view Parent = QualifiedNameOf(ScopeNode);

  if (!CanStage(ScopeNode))
    return ScopeNode;

  if (auto Diagnostic = ValidateNamespaceSegment(Segment)) {
    RecordFailure(std::move(*Diagnostic));
    return ScopeNode;
  }

  StagedNamespace Declaration;
  Declaration.Segment = std::string(Segment);
  Declaration.QualifiedName = JoinQualifiedName(Parent, Segment);

  if (auto Diagnostic =
          ValidateCanonicalQualifiedName(Declaration.QualifiedName)) {
    RecordFailure(std::move(*Diagnostic));
    return ScopeNode;
  }

  for (std::size_t Index = 0; Index < Staged.Namespaces.size(); ++Index) {
    if (Staged.Namespaces[Index].QualifiedName == Declaration.QualifiedName)
      return Index + 1;
  }

  Staged.Namespaces.push_back(std::move(Declaration));
  return Staged.Namespaces.size();
}

void NamespaceBuilderState::StageFunction(std::size_t ScopeNode,
                                          std::string_view Name,
                                          ErasedCallableDescriptor Descriptor) {
  const std::string_view Parent = QualifiedNameOf(ScopeNode);

  if (!CanStage(ScopeNode))
    return;

  if (auto Diagnostic = ValidateNamespaceSegment(Name)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  StagedFunction Declaration;
  Declaration.Segment = std::string(Name);
  Declaration.QualifiedName = JoinQualifiedName(Parent, Name);

  if (auto Diagnostic =
          ValidateCanonicalQualifiedName(Declaration.QualifiedName)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  Declaration.Callable =
      std::make_shared<ErasedCallableDescriptor>(std::move(Descriptor));
  Staged.Functions.push_back(std::move(Declaration));
}

void NamespaceBuilderState::StageConstant(std::size_t ScopeNode,
                                          std::string_view Name,
                                          ConstantRequest Request) {
  const std::string_view Parent = QualifiedNameOf(ScopeNode);

  if (!CanStage(ScopeNode))
    return;

  if (auto Diagnostic = ValidateNamespaceSegment(Name)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  StagedConstant Declaration;
  Declaration.Segment = std::string(Name);
  Declaration.QualifiedName = JoinQualifiedName(Parent, Name);
  Declaration.Request = std::move(Request);

  if (auto Diagnostic =
          ValidateCanonicalQualifiedName(Declaration.QualifiedName)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  Staged.Constants.push_back(std::move(Declaration));
}

std::size_t NamespaceBuilderState::StageEnumeration(
    std::size_t ScopeNode, std::string_view Name, const StableTypeKey &Key,
    const EnumerationPolicy &Policy) {
  const std::string_view Parent = QualifiedNameOf(ScopeNode);

  if (!CanStage(ScopeNode))
    return 0;

  if (auto Diagnostic = ValidateNamespaceSegment(Name)) {
    RecordFailure(std::move(*Diagnostic));
    return 0;
  }

  StagedEnumeration Declaration;
  Declaration.Segment = std::string(Name);
  Declaration.QualifiedName = JoinQualifiedName(Parent, Name);
  Declaration.Key = Key;
  Declaration.Policy = Policy;

  if (auto Diagnostic =
          ValidateCanonicalQualifiedName(Declaration.QualifiedName)) {
    RecordFailure(std::move(*Diagnostic));
    return 0;
  }

  if (!Key.IsValid()) {
    RecordFailure(MalformedMetadataDiagnostic(
        EnumerationSubjectOf(Declaration.QualifiedName),
        "the stable type key '" + Key.Text() + "' is not valid (" +
            std::string(StableTypeKeyStatusText(Key.Status())) + ")."));
    return 0;
  }

  Staged.Enumerations.push_back(std::move(Declaration));
  return Staged.Enumerations.size();
}

std::size_t NamespaceBuilderState::StageClass(std::size_t ScopeNode,
                                              std::string_view Name,
                                              const StableTypeKey &Key,
                                              const ClassPolicy &Policy) {
  const std::string_view Parent = QualifiedNameOf(ScopeNode);

  if (!CanStage(ScopeNode))
    return 0;

  if (auto Diagnostic = ValidateNamespaceSegment(Name)) {
    RecordFailure(std::move(*Diagnostic));
    return 0;
  }

  StagedClass Declaration;
  Declaration.Segment = std::string(Name);
  Declaration.QualifiedName = JoinQualifiedName(Parent, Name);
  Declaration.Key = Key;
  Declaration.Policy = Policy;

  if (auto Diagnostic =
          ValidateCanonicalQualifiedName(Declaration.QualifiedName)) {
    RecordFailure(std::move(*Diagnostic));
    return 0;
  }

  if (auto Diagnostic = ValidateStagedClass(Declaration)) {
    RecordFailure(std::move(*Diagnostic));
    return 0;
  }

  Staged.Classes.push_back(std::move(Declaration));
  return Staged.Classes.size();
}

void NamespaceBuilderState::StageModule(std::size_t ScopeNode,
                                        ModuleManifest Manifest,
                                        ModuleRegistration Registration) {
  if (!CanStage(ScopeNode))
    return;

  StagedModule Declaration;
  Declaration.ParentQualifiedName = std::string(QualifiedNameOf(ScopeNode));
  Declaration.Manifest = std::move(Manifest);
  Declaration.Registration = std::move(Registration);
  Staged.Modules.push_back(std::move(Declaration));
}

std::size_t NamespaceBuilderState::StagePath(std::string_view QualifiedName) {
  std::size_t Node = RootScopeNode;
  std::size_t Start = 0;
  while (Start <= QualifiedName.size() && !QualifiedName.empty()) {
    const std::size_t Separator =
        QualifiedName.find(QualifiedNameSeparator, Start);
    const std::size_t End =
        Separator == std::string_view::npos ? QualifiedName.size() : Separator;
    Node = Stage(Node, QualifiedName.substr(Start, End - Start));
    if (Separator == std::string_view::npos)
      break;
    Start = Separator + 1;
  }
  return Node;
}

StagedAnnotationTarget
NamespaceBuilderState::ScopeAnnotationTarget(std::size_t ScopeNode,
                                             std::string_view Member) {
  const std::string_view Scope = QualifiedNameOf(ScopeNode);

  if (Member.empty()) {
    if (ScopeNode == RootScopeNode || ScopeNode > Staged.Namespaces.size()) {
      RecordFailure(MalformedMetadataDiagnostic(
          SubjectText(PlanEntryKindText(PlanEntryKind::Scope),
                      "the root scope"),
          "the root scope declares no symbol of its own, so it carries no "
          "documentation, attributes, or examples."));
      return StagedAnnotationTarget();
    }
    StagedNamespace &Declaration = Staged.Namespaces[ScopeNode - 1];
    return StagedAnnotationTarget{&Declaration.Documentation,
                                  &Declaration.Attributes,
                                  &Declaration.Examples};
  }

  const std::string QualifiedName = JoinQualifiedName(Scope, Member);
  for (StagedFunction &Declaration : Staged.Functions) {
    if (Declaration.QualifiedName == QualifiedName)
      return StagedAnnotationTarget{&Declaration.Documentation,
                                    &Declaration.Attributes,
                                    &Declaration.Examples};
  }
  for (StagedConstant &Declaration : Staged.Constants) {
    if (Declaration.QualifiedName == QualifiedName)
      return StagedAnnotationTarget{&Declaration.Documentation,
                                    &Declaration.Attributes,
                                    &Declaration.Examples};
  }
  for (StagedEnumeration &Declaration : Staged.Enumerations) {
    if (Declaration.QualifiedName == QualifiedName)
      return StagedAnnotationTarget{&Declaration.Documentation,
                                    &Declaration.Attributes,
                                    &Declaration.Examples};
  }
  for (StagedClass &Declaration : Staged.Classes) {
    if (Declaration.QualifiedName == QualifiedName)
      return StagedAnnotationTarget{&Declaration.Documentation,
                                    &Declaration.Attributes,
                                    &Declaration.Examples};
  }
  for (StagedNamespace &Declaration : Staged.Namespaces) {
    if (Declaration.QualifiedName == QualifiedName)
      return StagedAnnotationTarget{&Declaration.Documentation,
                                    &Declaration.Attributes,
                                    &Declaration.Examples};
  }

  RecordFailure(MalformedMetadataDiagnostic(
      SubjectText(PlanEntryKindText(PlanEntryKind::Scope),
                  Scope.empty() ? std::string_view("the root scope") : Scope),
      "'" + std::string(Member) +
          "' is not a declaration staged in this scope; declare it before "
          "documenting or annotating it."));
  return StagedAnnotationTarget();
}

void NamespaceBuilderState::StageScopeDocumentation(std::size_t ScopeNode,
                                                    std::string_view Member,
                                                    std::string_view Text) {
  if (!CanStage(ScopeNode))
    return;
  if (const StagedAnnotationTarget Target =
          ScopeAnnotationTarget(ScopeNode, Member);
      Target.IsValid())
    *Target.Documentation = std::string(Text);
}

void NamespaceBuilderState::StageScopeAttribute(
    std::size_t ScopeNode, std::string_view Member, std::string_view Name,
    std::string_view AttributeValue) {
  if (!CanStage(ScopeNode))
    return;
  if (Name.empty()) {
    const std::string_view Scope = QualifiedNameOf(ScopeNode);
    RecordFailure(MalformedMetadataDiagnostic(
        SubjectText(PlanEntryKindText(PlanEntryKind::Scope),
                    Scope.empty() ? std::string_view("the root scope") : Scope),
        "an attribute name is empty."));
    return;
  }

  if (const StagedAnnotationTarget Target =
          ScopeAnnotationTarget(ScopeNode, Member);
      Target.IsValid()) {
    ReflectionAttributeFields Attribute;
    Attribute.Name = std::string(Name);
    Attribute.Value = std::string(AttributeValue);
    Target.Attributes->push_back(std::move(Attribute));
  }
}

void NamespaceBuilderState::StageScopeExample(std::size_t ScopeNode,
                                              std::string_view Member,
                                              std::string_view Text) {
  if (!CanStage(ScopeNode))
    return;
  if (const StagedAnnotationTarget Target =
          ScopeAnnotationTarget(ScopeNode, Member);
      Target.IsValid())
    Target.Examples->push_back(std::string(Text));
}

StagedEnumeration *
NamespaceBuilderState::EnumerationAt(std::size_t EnumerationNode) noexcept {
  if (EnumerationNode == 0 || EnumerationNode > Staged.Enumerations.size())
    return nullptr;
  return &Staged.Enumerations[EnumerationNode - 1];
}

void NamespaceBuilderState::StageEnumerator(std::size_t EnumerationNode,
                                            std::string_view Name,
                                            std::int64_t Numeric) {
  StagedEnumeration *Declaration = EnumerationAt(EnumerationNode);
  if (!Declaration || !CanStage(RootScopeNode))
    return;

  if (auto Diagnostic = ValidateNamespaceSegment(Name)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  StagedEnumerator Enumerator;
  Enumerator.Segment = std::string(Name);
  Enumerator.Numeric = Numeric;
  Declaration->Enumerators.push_back(std::move(Enumerator));
}

void NamespaceBuilderState::StageAlias(std::size_t EnumerationNode,
                                       std::string_view AliasName,
                                       std::string_view CanonicalName) {
  StagedEnumeration *Declaration = EnumerationAt(EnumerationNode);
  if (!Declaration || !CanStage(RootScopeNode))
    return;

  if (auto Diagnostic = ValidateNamespaceSegment(AliasName)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }
  if (auto Diagnostic = ValidateNamespaceSegment(CanonicalName)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  StagedEnumerator Alias;
  Alias.Segment = std::string(AliasName);
  Alias.IsAlias = true;
  Alias.CanonicalSegment = std::string(CanonicalName);
  Declaration->Enumerators.push_back(std::move(Alias));
}

void NamespaceBuilderState::StageBitflags(std::size_t EnumerationNode,
                                          bool HasDeclaredMask,
                                          std::int64_t SupportedBits) {
  StagedEnumeration *Declaration = EnumerationAt(EnumerationNode);
  if (!Declaration)
    return;

  Declaration->IsBitflags = true;
  if (!HasDeclaredMask)
    return;
  Declaration->HasDeclaredMask = true;
  Declaration->SupportedBits = SupportedBits;
}

void NamespaceBuilderState::StageUnscopedOptIn(std::size_t EnumerationNode) {
  if (StagedEnumeration *Declaration = EnumerationAt(EnumerationNode))
    Declaration->UnscopedIsAllowed = true;
}

void NamespaceBuilderState::StageObjectRepresentation(
    std::size_t EnumerationNode) {
  if (StagedEnumeration *Declaration = EnumerationAt(EnumerationNode))
    Declaration->PublishesObjects = true;
}

StagedAnnotationTarget
NamespaceBuilderState::EnumerationAnnotationTarget(std::size_t EnumerationNode,
                                                   std::string_view Member) {
  StagedEnumeration *Declaration = EnumerationAt(EnumerationNode);
  if (!Declaration)
    return StagedAnnotationTarget();

  if (Member.empty())
    return StagedAnnotationTarget{&Declaration->Documentation,
                                  &Declaration->Attributes,
                                  &Declaration->Examples};

  StagedEnumerator *Enumerator = FindStagedMember(*Declaration, Member);
  if (!Enumerator) {
    RecordFailure(UndeclaredMemberDiagnostic(*Declaration, Member));
    return StagedAnnotationTarget();
  }
  return StagedAnnotationTarget{&Enumerator->Documentation,
                                &Enumerator->Attributes, &Enumerator->Examples};
}

void NamespaceBuilderState::StageEnumerationDocumentation(
    std::size_t EnumerationNode, std::string_view Member,
    std::string_view Text) {
  if (const StagedAnnotationTarget Target =
          EnumerationAnnotationTarget(EnumerationNode, Member);
      Target.IsValid())
    *Target.Documentation = std::string(Text);
}

void NamespaceBuilderState::StageEnumerationAttribute(
    std::size_t EnumerationNode, std::string_view Member, std::string_view Name,
    std::string_view AttributeValue) {
  StagedEnumeration *Declaration = EnumerationAt(EnumerationNode);
  if (!Declaration)
    return;

  if (Name.empty()) {
    RecordFailure(MalformedMetadataDiagnostic(
        EnumerationSubjectOf(Declaration->QualifiedName),
        "an attribute name is empty."));
    return;
  }

  if (const StagedAnnotationTarget Target =
          EnumerationAnnotationTarget(EnumerationNode, Member);
      Target.IsValid()) {
    ReflectionAttributeFields Attribute;
    Attribute.Name = std::string(Name);
    Attribute.Value = std::string(AttributeValue);
    Target.Attributes->push_back(std::move(Attribute));
  }
}

void NamespaceBuilderState::StageEnumerationExample(std::size_t EnumerationNode,
                                                    std::string_view Member,
                                                    std::string_view Text) {
  if (const StagedAnnotationTarget Target =
          EnumerationAnnotationTarget(EnumerationNode, Member);
      Target.IsValid())
    Target.Examples->push_back(std::string(Text));
}

StagedClass *NamespaceBuilderState::ClassAt(std::size_t ClassNode) noexcept {
  if (ClassNode == 0 || ClassNode > Staged.Classes.size())
    return nullptr;
  return &Staged.Classes[ClassNode - 1];
}

StagedConstruction *
NamespaceBuilderState::ConstructionAt(StagedClass &Declaration,
                                      std::string_view Member) noexcept {
  return FindStagedConstruction(Declaration, Member);
}

StagedMethod *
NamespaceBuilderState::MethodAt(StagedClass &Declaration,
                                std::string_view Member) noexcept {
  return FindStagedMethod(Declaration, Member);
}

void NamespaceBuilderState::StageClassMember(std::size_t ClassNode,
                                             std::string_view Name,
                                             MethodRequest Request) {
  StagedClass *Declaration = ClassAt(ClassNode);
  if (!Declaration || !CanStage(RootScopeNode))
    return;

  if (auto Diagnostic = ValidateNamespaceSegment(Name)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  StagedMethod Staging;
  Staging.Segment = std::string(Name);
  Staging.QualifiedName = JoinQualifiedName(Declaration->QualifiedName, Name);
  Staging.Kind = Request.Kind;
  Staging.DeclaresReceiver = Request.DeclaresReceiver;
  Staging.ReceiverIsConst = Request.ReceiverIsConst;
  Staging.Refusal = std::move(Request.Refusal);
  if (Request.Callable)
    Staging.Callable = std::make_shared<ErasedCallableDescriptor>(
        std::move(*Request.Callable));

  if (auto Diagnostic = ValidateCanonicalQualifiedName(Staging.QualifiedName)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  if (auto Diagnostic = ValidateStagedMethod(*Declaration, Staging)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  Declaration->Methods.push_back(std::move(Staging));
}

StagedMember *
NamespaceBuilderState::AccessorAt(StagedClass &Declaration,
                                  std::string_view Member) noexcept {
  return FindStagedClassMember(Declaration.Members, Member);
}

void NamespaceBuilderState::StageClassAccessor(std::size_t ClassNode,
                                               std::string_view Name,
                                               MemberRequest Request) {
  StagedClass *Declaration = ClassAt(ClassNode);
  if (!Declaration || !CanStage(RootScopeNode))
    return;

  if (auto Diagnostic = ValidateNamespaceSegment(Name)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  StagedMember Staging;
  Staging.Segment = std::string(Name);
  Staging.QualifiedName = JoinQualifiedName(Declaration->QualifiedName, Name);
  Staging.Kind = Request.Kind;
  Staging.Access = Request.Access;
  Staging.Evaluation = Request.Evaluation;
  Staging.Ownership = Request.Ownership;
  Staging.ValueType = Request.ValueType;
  Staging.ReceiverType = Request.ReceiverType;
  Staging.ReadRequiresMutableReceiver = Request.ReadRequiresMutableReceiver;
  Staging.Read = std::move(Request.Read);
  Staging.Write = std::move(Request.Write);
  Staging.Change = std::move(Request.Change);
  Staging.ConvertedRead = std::move(Request.ConvertedRead);
  Staging.ConvertedWrite = std::move(Request.ConvertedWrite);
  Staging.Refusal = std::move(Request.Refusal);

  if (auto Diagnostic = ValidateCanonicalQualifiedName(Staging.QualifiedName)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  MemberCollisionRequest Collision;
  Collision.Segment = Staging.Segment;
  Collision.QualifiedName = Staging.QualifiedName;
  Collision.Kind = Staging.Kind;
  if (const StagedMember *Existing =
          FindStagedClassMember(Declaration->Members, Staging.Segment)) {
    Collision.NameIsDeclared = true;
    Collision.ExistingKind = Existing->Kind;
    Collision.ExistingCategory = PlanEntryKind::ClassMember;
    Collision.ExistingIsPending = true;
  } else if (FindStagedConstruction(*Declaration, Staging.Segment) != nullptr ||
             FindStagedMethod(*Declaration, Staging.Segment) != nullptr) {
    Collision.NameIsDeclared = true;
    Collision.ExistingKind = SymbolKind::FunctionCandidate;
    Collision.ExistingCategory = PlanEntryKind::Function;
    Collision.ExistingIsPending = true;
  }
  if (auto Diagnostic = DiagnoseMemberCollision(Collision)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  if (auto Diagnostic = ValidateStagedMember(Staging)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  Declaration->Members.push_back(std::move(Staging));
}

void NamespaceBuilderState::StageClassConstruction(
    std::size_t ClassNode, std::string_view Name, ConstructionRequest Request) {
  StagedClass *Declaration = ClassAt(ClassNode);
  if (!Declaration || !CanStage(RootScopeNode))
    return;

  if (auto Diagnostic = ValidateNamespaceSegment(Name)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  StagedConstruction Staging;
  Staging.Segment = std::string(Name);
  Staging.QualifiedName = JoinQualifiedName(Declaration->QualifiedName, Name);
  Staging.Kind = Request.Kind;
  Staging.Ownership = Request.Ownership;
  Staging.AllocatorPolicy = std::move(Request.AllocatorPolicy);
  Staging.Refusal = std::move(Request.Refusal);
  if (Request.Callable)
    Staging.Callable = std::make_shared<ErasedCallableDescriptor>(
        std::move(*Request.Callable));

  if (auto Diagnostic = ValidateCanonicalQualifiedName(Staging.QualifiedName)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  if (auto Diagnostic = ValidateStagedConstruction(*Declaration, Staging)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  Declaration->Constructions.push_back(std::move(Staging));
}

void NamespaceBuilderState::StageClassAllocator(std::size_t ClassNode,
                                                const ClassAllocator &Storage) {
  StagedClass *Declaration = ClassAt(ClassNode);
  if (!Declaration || !CanStage(RootScopeNode))
    return;

  if (auto Diagnostic = ValidateSelectedClassStorage(*Declaration, Storage)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  Declaration->SelectsStorage = true;
  Declaration->Storage = Storage;
}

void NamespaceBuilderState::StageClassBase(std::size_t ClassNode,
                                           BaseRequest Request) {
  StagedClass *Declaration = ClassAt(ClassNode);
  if (!Declaration || !CanStage(RootScopeNode))
    return;
  Declaration->Relationships.Bases.push_back(std::move(Request));
}

void NamespaceBuilderState::StageClassCast(std::size_t ClassNode,
                                           CastRequest Request) {
  StagedClass *Declaration = ClassAt(ClassNode);
  if (!Declaration || !CanStage(RootScopeNode))
    return;
  Declaration->Relationships.Casts.push_back(std::move(Request));
}

StagedOperator *
NamespaceBuilderState::OperatorAt(StagedClass &Declaration,
                                  std::string_view Member) noexcept {
  return FindStagedOperator(Declaration.Operators, Member);
}

void NamespaceBuilderState::StageClassOperator(std::size_t ClassNode,
                                               ClassOperator Selected,
                                               MethodRequest Request) {
  StagedClass *Declaration = ClassAt(ClassNode);
  if (!Declaration || !CanStage(RootScopeNode))
    return;

  const ClassOperatorDescriptor *Described = FindClassOperator(Selected);
  if (Described == nullptr) {
    RecordFailure(MalformedMetadataDiagnostic(
        SubjectText(SymbolKindText(SymbolKind::Operator),
                    Declaration->QualifiedName),
        "this operator is not one Luna supports on a class."));
    return;
  }

  StagedOperator Staging;
  Staging.Selected = Selected;
  Staging.Segment = std::string(Described->Segment);
  Staging.QualifiedName =
      JoinQualifiedName(Declaration->QualifiedName, Staging.Segment);
  Staging.DeclaresReceiver = Request.DeclaresReceiver;
  Staging.ReceiverIsConst = Request.ReceiverIsConst;
  Staging.Refusal = std::move(Request.Refusal);
  if (Request.Callable)
    Staging.Callable = std::make_shared<ErasedCallableDescriptor>(
        std::move(*Request.Callable));

  if (auto Diagnostic = ValidateCanonicalQualifiedName(Staging.QualifiedName)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  if (auto Diagnostic = ValidateStagedOperator(Staging)) {
    RecordFailure(std::move(*Diagnostic));
    return;
  }

  Declaration->Operators.push_back(std::move(Staging));
}

StagedAnnotationTarget
NamespaceBuilderState::ClassAnnotationTarget(std::size_t ClassNode,
                                             std::string_view Member) {
  StagedClass *Declaration = ClassAt(ClassNode);
  if (!Declaration)
    return StagedAnnotationTarget();

  if (Member.empty())
    return StagedAnnotationTarget{&Declaration->Documentation,
                                  &Declaration->Attributes,
                                  &Declaration->Examples};

  if (StagedConstruction *Staged = ConstructionAt(*Declaration, Member))
    return StagedAnnotationTarget{&Staged->Documentation, &Staged->Attributes,
                                  &Staged->Examples};
  if (StagedMethod *Staged = MethodAt(*Declaration, Member))
    return StagedAnnotationTarget{&Staged->Documentation, &Staged->Attributes,
                                  &Staged->Examples};
  if (StagedMember *Staged = AccessorAt(*Declaration, Member))
    return StagedAnnotationTarget{&Staged->Documentation, &Staged->Attributes,
                                  &Staged->Examples};

  if (StagedOperator *Staged = OperatorAt(*Declaration, Member))
    return StagedAnnotationTarget{&Staged->Documentation, &Staged->Attributes,
                                  &Staged->Examples};

  RecordFailure(UndeclaredClassMemberDiagnostic(*Declaration, Member));
  return StagedAnnotationTarget();
}

void NamespaceBuilderState::StageClassDocumentation(std::size_t ClassNode,
                                                    std::string_view Member,
                                                    std::string_view Text) {
  if (const StagedAnnotationTarget Target =
          ClassAnnotationTarget(ClassNode, Member);
      Target.IsValid())
    *Target.Documentation = std::string(Text);
}

void NamespaceBuilderState::StageClassAttribute(
    std::size_t ClassNode, std::string_view Member, std::string_view Name,
    std::string_view AttributeValue) {
  StagedClass *Declaration = ClassAt(ClassNode);
  if (!Declaration)
    return;

  if (Name.empty()) {
    RecordFailure(MalformedMetadataDiagnostic(
        SubjectText(SymbolKindText(SymbolKind::Class),
                    Declaration->QualifiedName),
        "an attribute name is empty."));
    return;
  }

  if (const StagedAnnotationTarget Target =
          ClassAnnotationTarget(ClassNode, Member);
      Target.IsValid()) {
    ReflectionAttributeFields Attribute;
    Attribute.Name = std::string(Name);
    Attribute.Value = std::string(AttributeValue);
    Target.Attributes->push_back(std::move(Attribute));
  }
}

void NamespaceBuilderState::StageClassExample(std::size_t ClassNode,
                                              std::string_view Member,
                                              std::string_view Text) {
  if (const StagedAnnotationTarget Target =
          ClassAnnotationTarget(ClassNode, Member);
      Target.IsValid())
    Target.Examples->push_back(std::string(Text));
}

StagedAnnotationTarget
NamespaceBuilderState::ClassOperatorAnnotationTarget(std::size_t ClassNode,
                                                     ClassOperator Selected) {
  StagedClass *Declaration = ClassAt(ClassNode);
  if (!Declaration)
    return StagedAnnotationTarget();

  StagedOperator *Staged = FindStagedOperator(Declaration->Operators, Selected);
  if (!Staged) {
    RecordFailure(UndeclaredClassMemberDiagnostic(*Declaration,
                                                  ClassOperatorText(Selected)));
    return StagedAnnotationTarget();
  }
  return StagedAnnotationTarget{&Staged->Documentation, &Staged->Attributes,
                                &Staged->Examples};
}

void NamespaceBuilderState::StageClassOperatorDocumentation(
    std::size_t ClassNode, ClassOperator Selected, std::string_view Text) {
  if (const StagedAnnotationTarget Target =
          ClassOperatorAnnotationTarget(ClassNode, Selected);
      Target.IsValid())
    *Target.Documentation = std::string(Text);
}

void NamespaceBuilderState::StageClassOperatorAttribute(
    std::size_t ClassNode, ClassOperator Selected, std::string_view Name,
    std::string_view AttributeValue) {
  StagedClass *Declaration = ClassAt(ClassNode);
  if (!Declaration)
    return;

  if (Name.empty()) {
    RecordFailure(MalformedMetadataDiagnostic(
        SubjectText(SymbolKindText(SymbolKind::Class),
                    Declaration->QualifiedName),
        "an attribute name is empty."));
    return;
  }

  if (const StagedAnnotationTarget Target =
          ClassOperatorAnnotationTarget(ClassNode, Selected);
      Target.IsValid()) {
    ReflectionAttributeFields Attribute;
    Attribute.Name = std::string(Name);
    Attribute.Value = std::string(AttributeValue);
    Target.Attributes->push_back(std::move(Attribute));
  }
}

void NamespaceBuilderState::StageClassOperatorExample(std::size_t ClassNode,
                                                      ClassOperator Selected,
                                                      std::string_view Text) {
  if (const StagedAnnotationTarget Target =
          ClassOperatorAnnotationTarget(ClassNode, Selected);
      Target.IsValid())
    Target.Examples->push_back(std::string(Text));
}

std::string_view
NamespaceBuilderState::QualifiedNameOf(std::size_t ScopeNode) const noexcept {
  if (ScopeNode == RootScopeNode || ScopeNode > Staged.Namespaces.size())
    return std::string_view();
  return Staged.Namespaces[ScopeNode - 1].QualifiedName;
}

std::string_view NamespaceBuilderState::QualifiedNameOfEnumeration(
    std::size_t EnumerationNode) const noexcept {
  if (EnumerationNode == 0 || EnumerationNode > Staged.Enumerations.size())
    return std::string_view();
  return Staged.Enumerations[EnumerationNode - 1].QualifiedName;
}

std::string_view NamespaceBuilderState::QualifiedNameOfClass(
    std::size_t ClassNode) const noexcept {
  if (ClassNode == 0 || ClassNode > Staged.Classes.size())
    return std::string_view();
  return Staged.Classes[ClassNode - 1].QualifiedName;
}

RegistrationResult NamespaceBuilderState::Commit() {
  std::string_view First;
  if (!Staged.Namespaces.empty())
    First = Staged.Namespaces.front().QualifiedName;
  else if (!Staged.Enumerations.empty())
    First = Staged.Enumerations.front().QualifiedName;
  else if (!Staged.Classes.empty())
    First = Staged.Classes.front().QualifiedName;
  else if (!Staged.Constants.empty())
    First = Staged.Constants.front().QualifiedName;
  else if (!Staged.Functions.empty())
    First = Staged.Functions.front().QualifiedName;
  else if (!Staged.Modules.empty())
    First = Staged.Modules.front().Manifest.Identity();

  if (Committed)
    return RegistrationResult::Failure(StaleBuilderDiagnostic(
        SubjectText(PlanEntryKindText(PlanEntryKind::Scope),
                    First.empty() ? std::string_view("the root scope") : First),
        "its plan was already committed"));

  const BuilderHandleStatus Status = Classify();
  if (Status == BuilderHandleStatus::ForeignThread) {
    return RegistrationResult::Failure(ForeignThreadDiagnostic(SubjectText(
        PlanEntryKindText(PlanEntryKind::Scope),
        First.empty() ? std::string_view("the root scope") : First)));
  }
  if (IsFatalHandleStatus(Status))
    return RegistrationResult::Failure(StaleDiagnostic(Status, First));

  State *Live = LiveOwner();
  if (!Live || !Live->Implementation)
    return RegistrationResult::Failure(
        StaleDiagnostic(BuilderHandleStatus::OwnerDestroyed, First));

  Committed = true;
  return Live->Implementation->RegisterBuilderPlan(Staged, Failure);
}

EnumStaging::EnumStaging() noexcept = default;

EnumStaging::EnumStaging(std::shared_ptr<NamespaceBuilderState> Plan,
                         std::size_t Node) noexcept
    : Plan(std::move(Plan)), Node(Node) {}

EnumStaging::EnumStaging(EnumStaging &&Other) noexcept = default;

EnumStaging &EnumStaging::operator=(EnumStaging &&Other) noexcept = default;

EnumStaging::~EnumStaging() = default;

void EnumStaging::StageValue(std::string_view Name, std::int64_t Numeric) {
  if (Plan)
    Plan->StageEnumerator(Node, Name, Numeric);
}

void EnumStaging::StageAlias(std::string_view AliasName,
                             std::string_view CanonicalName) {
  if (Plan)
    Plan->StageAlias(Node, AliasName, CanonicalName);
}

void EnumStaging::StageBitflags(bool HasDeclaredMask,
                                std::int64_t SupportedBits) {
  if (Plan)
    Plan->StageBitflags(Node, HasDeclaredMask, SupportedBits);
}

void EnumStaging::StageUnscopedOptIn() {
  if (Plan)
    Plan->StageUnscopedOptIn(Node);
}

void EnumStaging::StageObjectRepresentation() {
  if (Plan)
    Plan->StageObjectRepresentation(Node);
}

void EnumStaging::StageDocumentation(std::string_view Member,
                                     std::string_view Text) {
  if (Plan)
    Plan->StageEnumerationDocumentation(Node, Member, Text);
}

void EnumStaging::StageAttribute(std::string_view Member, std::string_view Name,
                                 std::string_view AttributeValue) {
  if (Plan)
    Plan->StageEnumerationAttribute(Node, Member, Name, AttributeValue);
}

void EnumStaging::StageExample(std::string_view Member, std::string_view Text) {
  if (Plan)
    Plan->StageEnumerationExample(Node, Member, Text);
}

RegistrationResult EnumStaging::Commit() {
  if (!Plan)
    return RegistrationResult::Failure(StaleBuilderDiagnostic(
        SubjectText(SymbolKindText(SymbolKind::Enumeration),
                    "the requested enumeration"),
        "it owns no pending plan"));
  return Plan->Commit();
}

std::string_view EnumStaging::QualifiedName() const noexcept {
  if (!Plan)
    return std::string_view();
  return Plan->QualifiedNameOfEnumeration(Node);
}

EnumStaging StageEnumeration(std::shared_ptr<NamespaceBuilderState> Plan,
                             std::size_t ScopeNode, std::string_view Name,
                             const StableTypeKey &Key,
                             const EnumerationPolicy &Policy) {
  if (!Plan)
    return EnumStaging();
  const std::size_t Node = Plan->StageEnumeration(ScopeNode, Name, Key, Policy);
  return EnumStaging(std::move(Plan), Node);
}

EnumStaging StageRootEnumeration(State &Owner, std::string_view Name,
                                 const StableTypeKey &Key,
                                 const EnumerationPolicy &Policy) {
  std::shared_ptr<NamespaceBuilderState> Plan =
      NamespaceBuilderState::Create(Owner);
  const std::size_t Node = Plan->StageEnumeration(
      NamespaceBuilderState::RootScopeNode, Name, Key, Policy);
  return EnumStaging(std::move(Plan), Node);
}

ClassStaging::ClassStaging() noexcept = default;

ClassStaging::ClassStaging(std::shared_ptr<NamespaceBuilderState> Plan,
                           std::size_t Node) noexcept
    : Plan(std::move(Plan)), Node(Node) {}

ClassStaging::ClassStaging(ClassStaging &&Other) noexcept = default;

ClassStaging &ClassStaging::operator=(ClassStaging &&Other) noexcept = default;

ClassStaging::~ClassStaging() = default;

void ClassStaging::StageDocumentation(std::string_view Member,
                                      std::string_view Text) {
  if (Plan)
    Plan->StageClassDocumentation(Node, Member, Text);
}

void ClassStaging::StageAttribute(std::string_view Member,
                                  std::string_view Name,
                                  std::string_view AttributeValue) {
  if (Plan)
    Plan->StageClassAttribute(Node, Member, Name, AttributeValue);
}

void ClassStaging::StageExample(std::string_view Member,
                                std::string_view Text) {
  if (Plan)
    Plan->StageClassExample(Node, Member, Text);
}

void ClassStaging::StageOperatorDocumentation(ClassOperator Selected,
                                              std::string_view Text) {
  if (Plan)
    Plan->StageClassOperatorDocumentation(Node, Selected, Text);
}

void ClassStaging::StageOperatorAttribute(ClassOperator Selected,
                                          std::string_view Name,
                                          std::string_view AttributeValue) {
  if (Plan)
    Plan->StageClassOperatorAttribute(Node, Selected, Name, AttributeValue);
}

void ClassStaging::StageOperatorExample(ClassOperator Selected,
                                        std::string_view Text) {
  if (Plan)
    Plan->StageClassOperatorExample(Node, Selected, Text);
}

void ClassStaging::StageConstruction(std::string_view Name,
                                     ConstructionRequest Request) {
  if (Plan)
    Plan->StageClassConstruction(Node, Name, std::move(Request));
}

void ClassStaging::StageMember(std::string_view Name, MethodRequest Request) {
  if (Plan)
    Plan->StageClassMember(Node, Name, std::move(Request));
}

void ClassStaging::StageAccessor(std::string_view Name, MemberRequest Request) {
  if (Plan)
    Plan->StageClassAccessor(Node, Name, std::move(Request));
}

void ClassStaging::StageAllocator(const ClassAllocator &Storage) {
  if (Plan)
    Plan->StageClassAllocator(Node, Storage);
}

void ClassStaging::StageBase(BaseRequest Request) {
  if (Plan)
    Plan->StageClassBase(Node, std::move(Request));
}

void ClassStaging::StageCast(CastRequest Request) {
  if (Plan)
    Plan->StageClassCast(Node, std::move(Request));
}

void ClassStaging::StageOperator(ClassOperator Selected,
                                 MethodRequest Request) {
  if (Plan)
    Plan->StageClassOperator(Node, Selected, std::move(Request));
}

RegistrationResult ClassStaging::Commit() {
  if (!Plan)
    return RegistrationResult::Failure(StaleBuilderDiagnostic(
        SubjectText(SymbolKindText(SymbolKind::Class), "the requested class"),
        "it owns no pending plan"));
  return Plan->Commit();
}

std::string_view ClassStaging::QualifiedName() const noexcept {
  if (!Plan)
    return std::string_view();
  return Plan->QualifiedNameOfClass(Node);
}

ClassStaging StageClassDeclaration(std::shared_ptr<NamespaceBuilderState> Plan,
                                   std::size_t ScopeNode, std::string_view Name,
                                   const StableTypeKey &Key,
                                   const ClassPolicy &Policy) {
  if (!Plan)
    return ClassStaging();
  const std::size_t Node = Plan->StageClass(ScopeNode, Name, Key, Policy);
  return ClassStaging(std::move(Plan), Node);
}

ClassStaging StageRootClassDeclaration(State &Owner, std::string_view Name,
                                       const StableTypeKey &Key,
                                       const ClassPolicy &Policy) {
  std::shared_ptr<NamespaceBuilderState> Plan =
      NamespaceBuilderState::Create(Owner);
  const std::size_t Node =
      Plan->StageClass(NamespaceBuilderState::RootScopeNode, Name, Key, Policy);
  return ClassStaging(std::move(Plan), Node);
}

} // namespace Detail

NamespaceBuilder::NamespaceBuilder(
    std::shared_ptr<Detail::NamespaceBuilderState> Plan,
    std::size_t Scope) noexcept
    : Plan(std::move(Plan)), Scope(Scope) {}

NamespaceBuilder::NamespaceBuilder(NamespaceBuilder &&Other) noexcept = default;

NamespaceBuilder &
NamespaceBuilder::operator=(NamespaceBuilder &&Other) noexcept = default;

NamespaceBuilder::~NamespaceBuilder() = default;

NamespaceBuilder NamespaceBuilder::RegisterNamespace(std::string_view Name) {
  if (!Plan)
    return NamespaceBuilder(nullptr, Scope);
  return NamespaceBuilder(Plan, Plan->Stage(Scope, Name));
}

void NamespaceBuilder::StageFunction(std::string_view Name,
                                     ErasedCallableDescriptor Descriptor) {
  if (Plan)
    Plan->StageFunction(Scope, Name, std::move(Descriptor));
}

void NamespaceBuilder::StageConstant(std::string_view Name,
                                     Detail::ConstantRequest Request) {
  if (Plan)
    Plan->StageConstant(Scope, Name, std::move(Request));
}

void NamespaceBuilder::StageModule(ModuleManifest Manifest,
                                   Detail::ModuleRegistration Registration) {
  if (Plan)
    Plan->StageModule(Scope, std::move(Manifest), std::move(Registration));
}

void NamespaceBuilder::StageDocumentation(std::string_view Member,
                                          std::string_view Text) {
  if (Plan)
    Plan->StageScopeDocumentation(Scope, Member, Text);
}

void NamespaceBuilder::StageAttribute(std::string_view Member,
                                      std::string_view Name,
                                      std::string_view AttributeValue) {
  if (Plan)
    Plan->StageScopeAttribute(Scope, Member, Name, AttributeValue);
}

void NamespaceBuilder::StageExample(std::string_view Member,
                                    std::string_view Text) {
  if (Plan)
    Plan->StageScopeExample(Scope, Member, Text);
}

std::string_view NamespaceBuilder::QualifiedName() const noexcept {
  if (!Plan)
    return std::string_view();
  return Plan->QualifiedNameOf(Scope);
}

RegistrationResult NamespaceBuilder::Commit() {
  if (!Plan)
    return RegistrationResult::Failure(Detail::StaleBuilderDiagnostic(
        Detail::SubjectText(
            Detail::PlanEntryKindText(Detail::PlanEntryKind::Scope),
            "the root scope"),
        "it owns no pending plan"));
  return Plan->Commit();
}

NamespaceBuilder BindingRegistry::RegisterNamespace(std::string_view Name) {
  std::shared_ptr<Detail::NamespaceBuilderState> Plan =
      Detail::NamespaceBuilderState::Create(*Owner);
  const std::size_t Node =
      Plan->Stage(Detail::NamespaceBuilderState::RootScopeNode, Name);
  return NamespaceBuilder(std::move(Plan), Node);
}

RegistrationResult
BindingRegistry::CommitProvidedModule(ModuleManifest Manifest,
                                      Detail::ModuleRegistration Registration) {
  if (!Owner || !Owner->Implementation)
    return RegistrationResult::Failure(Detail::StaleBuilderDiagnostic(
        Detail::ModuleSubject(Manifest), "its State was destroyed"));
  return Owner->Implementation->ProvideModuleDefinition(
      std::move(Manifest), std::move(Registration));
}

RegistrationResult
BindingRegistry::CommitModule(ModuleManifest Manifest,
                              Detail::ModuleRegistration Registration) {
  if (!Owner || !Owner->Implementation)
    return RegistrationResult::Failure(Detail::StaleBuilderDiagnostic(
        Detail::ModuleSubject(Manifest), "its State was destroyed"));
  return Owner->Implementation->RegisterModuleGraph(std::move(Manifest),
                                                    std::move(Registration));
}

RegistrationResult
BindingRegistry::CommitConstant(std::string_view Name,
                                Detail::ConstantRequest Request) {
  std::shared_ptr<Detail::NamespaceBuilderState> Plan =
      Detail::NamespaceBuilderState::Create(*Owner);
  Plan->StageConstant(Detail::NamespaceBuilderState::RootScopeNode, Name,
                      std::move(Request));
  return Plan->Commit();
}

} // namespace Luna
