// clang-format off
#include "state/impl.hpp"

#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/module_registration.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/module/module_manifest.hpp>

#include "state/binding/namespace_builder.hpp"
#include "state/freeze/cache.hpp"
#include "state/identity/identity_registry.hpp"
#include "state/module/load.hpp"
#include "state/module/registry.hpp"
#include "state/module/resolution.hpp"
#include "state/reflection/storage.hpp"
#include "state/registration/checks.hpp"
#include "state/registration/class_plan.hpp"
#include "state/registration/overload_group.hpp"
#include "state/registration/plan.hpp"
#include "state/registration/record.hpp"
#include "state/registration/scope_plan.hpp"
#include "state/registration/submission.hpp"
#include "state/registration/validation.hpp"
#include "state/registration/value_plan.hpp"
#include "state/testing/fault_point.hpp"
#include "state/transaction/capture.hpp"
#include "state/transaction/installation.hpp"
#include "state/transaction/lifecycle_staging.hpp"
#include "state/transaction/preparation.hpp"
#include "state/transaction/transaction.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"
#include "state/userdata/class_registry.hpp"
#include "state/userdata/exposure.hpp"
#include "state/userdata/identity.hpp"
#include "state/userdata/value_exposure.hpp"
#include "state/vm/closure_installer.hpp"
#include "state/vm/namespace_table.hpp"
#include "state/vm/saved_value.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace Luna {
namespace {

[[nodiscard]] ErrorDiagnostic Internal(std::string Message) {
  return ErrorDiagnostic::Create(ErrorCategory::Internal, std::move(Message));
}

[[nodiscard]] ErrorDiagnostic
PreparationDiagnostic(std::string_view Subject,
                      Detail::PreparationStatus Status,
                      Detail::ReflectionGenerationStatus ReflectionStatus,
                      Detail::TypeDeclarationStatus TypeStatus) {
  std::string Message = "Cannot register " + std::string(Subject) +
                        ": preparation of the candidate generation failed (" +
                        std::string(Detail::PreparationStatusText(Status));
  if (Status == Detail::PreparationStatus::InconsistentReflection)
    Message +=
        ": " +
        std::string(Detail::ReflectionGenerationStatusText(ReflectionStatus));
  if (Status == Detail::PreparationStatus::InconsistentTypes)
    Message +=
        ": " + std::string(Detail::TypeDeclarationStatusText(TypeStatus));
  Message += ").";
  return Internal(std::move(Message));
}

struct ClassSurfaceDeclaration final {
  std::string Segment;
  SymbolKind Kind = SymbolKind::Property;
  TypeId OwnerType;
  std::string OwnerName;
  SymbolId Declaration;
};

[[nodiscard]] bool IsInheritableMemberKind(SymbolKind Kind) noexcept {
  return Kind == SymbolKind::Method || Kind == SymbolKind::StaticMethod ||
         Kind == SymbolKind::Property || Kind == SymbolKind::Field;
}

[[nodiscard]] Detail::DescriptorPlanEntry *
PendingClassOf(Detail::DescriptorPlan &Plan, const TypeId &Type) noexcept {
  for (std::size_t Index = 0; Index < Plan.Size(); ++Index) {
    Detail::DescriptorPlanEntry *Entry = Plan.At(Index);
    if (Entry != nullptr &&
        Entry->Category == Detail::PlanEntryKind::ClassSymbol &&
        Entry->TypeFields && Entry->TypeFields->Id == Type)
      return Entry;
  }
  return nullptr;
}

[[nodiscard]] SymbolId ClassSymbolOf(Detail::DescriptorPlan &Plan,
                                     const Detail::ClassRegistry &Classes,
                                     const TypeId &Type) noexcept {
  if (const Detail::DescriptorPlanEntry *Pending = PendingClassOf(Plan, Type))
    return Pending->Identity;
  if (const Detail::RegisteredClass *Committed = Classes.Find(Type))
    return Committed->ClassSymbol;
  return SymbolId();
}

[[nodiscard]] std::vector<ClassSurfaceDeclaration>
DeclaredSurfaceOf(Detail::DescriptorPlan &Plan,
                  const Detail::ClassRegistry &Classes,
                  const Detail::ReflectionStorage &Captured,
                  const TypeId &OwnerType, std::string OwnerName) {
  std::vector<ClassSurfaceDeclaration> Declared;
  const SymbolId Owner = ClassSymbolOf(Plan, Classes, OwnerType);
  if (!Owner.IsValid())
    return Declared;

  for (const Detail::DescriptorPlanEntry &Entry : Plan.PlannedEntries()) {
    if (!(Entry.Symbol.Parent == Owner) || !Entry.Record ||
        !IsInheritableMemberKind(Entry.Record->Kind))
      continue;
    Declared.push_back(ClassSurfaceDeclaration{Entry.Record->Name,
                                               Entry.Record->Kind, OwnerType,
                                               OwnerName, Entry.Identity});
  }
  for (std::size_t Index = 0; Index < Captured.RecordCount(); ++Index) {
    const Detail::ReflectionRecordFields *Record = Captured.RecordAt(Index);
    if (Record == nullptr || !(Record->Declaration == Owner) ||
        !IsInheritableMemberKind(Record->Kind))
      continue;
    Declared.push_back(ClassSurfaceDeclaration{
        Record->Name, Record->Kind, OwnerType, OwnerName, Record->Id});
  }

  std::sort(Declared.begin(), Declared.end(),
            [](const ClassSurfaceDeclaration &Left,
               const ClassSurfaceDeclaration &Right) {
              if (Left.Segment != Right.Segment)
                return Left.Segment < Right.Segment;
              if (Left.OwnerName != Right.OwnerName)
                return Left.OwnerName < Right.OwnerName;
              if (Left.Kind != Right.Kind)
                return Left.Kind < Right.Kind;
              return Left.Declaration < Right.Declaration;
            });
  Declared.erase(std::unique(Declared.begin(), Declared.end(),
                             [](const ClassSurfaceDeclaration &Left,
                                const ClassSurfaceDeclaration &Right) {
                               return Left.Declaration == Right.Declaration;
                             }),
                 Declared.end());
  return Declared;
}

[[nodiscard]] bool
DeclaresSegment(const std::vector<ClassSurfaceDeclaration> &Declarations,
                std::string_view Segment) noexcept {
  for (const ClassSurfaceDeclaration &Declaration : Declarations) {
    if (Declaration.Segment == Segment)
      return true;
  }
  return false;
}

[[nodiscard]] bool
RelationPrecedes(const Detail::ReflectionRelationFields &Left,
                 const Detail::ReflectionRelationFields &Right) noexcept {
  if (Left.Kind != Right.Kind)
    return Left.Kind < Right.Kind;
  if (Left.Note != Right.Note)
    return Left.Note < Right.Note;
  if (Left.Type != Right.Type)
    return Left.Type < Right.Type;
  return Left.Declaration < Right.Declaration;
}

void AttachCanonicalClassRelations(
    Detail::DescriptorPlan &Plan,
    const Detail::RelationshipCandidate &Relationships,
    const Detail::ClassRegistry &Classes,
    const Detail::ReflectionStorage &Captured) {
  for (std::size_t Index = 0; Index < Plan.Size(); ++Index) {
    Detail::DescriptorPlanEntry *Class = Plan.At(Index);
    if (Class == nullptr ||
        Class->Category != Detail::PlanEntryKind::ClassSymbol ||
        !Class->TypeFields || !Class->Record)
      continue;

    const TypeId Derived = Class->TypeFields->Id;
    const auto Own = DeclaredSurfaceOf(Plan, Classes, Captured, Derived,
                                       Class->Record->QualifiedName);

    std::vector<TypeId> Bases = Relationships.ReachableBases(Derived);
    std::sort(Bases.begin(), Bases.end(),
              [&Relationships](const TypeId &Left, const TypeId &Right) {
                const auto *LeftClass = Relationships.Find(Left);
                const auto *RightClass = Relationships.Find(Right);
                const std::string_view LeftName = LeftClass != nullptr
                                                      ? LeftClass->QualifiedName
                                                      : std::string_view();
                const std::string_view RightName =
                    RightClass != nullptr ? RightClass->QualifiedName
                                          : std::string_view();
                return LeftName != RightName ? LeftName < RightName
                                             : Left < Right;
              });

    std::vector<ClassSurfaceDeclaration> Inherited;
    for (const TypeId &Base : Bases) {
      const Detail::RelationshipClass *Owner = Relationships.Find(Base);
      if (Owner == nullptr)
        continue;

      bool AlreadyReflected = false;
      for (Detail::ReflectionRelationFields &Relation :
           Class->Record->Relations) {
        if (Relation.Kind != TypeRelationKind::Base || !(Relation.Type == Base))
          continue;
        Relation.Note = "accessible direct";
        Relation.Declaration = ClassSymbolOf(Plan, Classes, Base);
        AlreadyReflected = true;
      }
      if (!AlreadyReflected) {
        Detail::ReflectionRelationFields Relation;
        Relation.Kind = TypeRelationKind::Base;
        Relation.Type = Base;
        Relation.Declaration = ClassSymbolOf(Plan, Classes, Base);
        Relation.Note = "accessible inherited";
        Class->Record->Relations.push_back(std::move(Relation));
      }

      for (ClassSurfaceDeclaration &Member : DeclaredSurfaceOf(
               Plan, Classes, Captured, Base, Owner->QualifiedName)) {
        if (!DeclaresSegment(Own, Member.Segment))
          Inherited.push_back(std::move(Member));
      }
    }

    for (Detail::ReflectionRelationFields &Relation :
         Class->Record->Relations) {
      if (Relation.Kind == TypeRelationKind::Cast)
        Relation.Declaration = ClassSymbolOf(Plan, Classes, Relation.Type);
    }

    for (const Detail::DescriptorPlanEntry &Entry : Plan.PlannedEntries()) {
      if (!(Entry.Symbol.Parent == Class->Identity) || !Entry.OperatorFields ||
          !Entry.Record)
        continue;
      const std::string Prefix =
          std::string(ClassOperatorText(Entry.OperatorFields->Selected)) + " ";
      for (Detail::ReflectionRelationFields &Relation :
           Class->Record->Relations) {
        if (Relation.Kind == TypeRelationKind::Operand &&
            Relation.Note.starts_with(Prefix) &&
            !Relation.Declaration.IsValid())
          Relation.Declaration = Entry.Record->OverloadSet.IsValid()
                                     ? Entry.Record->OverloadSet
                                     : Entry.Identity;
      }
    }

    for (const ClassSurfaceDeclaration &Member : Inherited) {
      std::vector<TypeId> Owners;
      for (const ClassSurfaceDeclaration &Other : Inherited) {
        if (Other.Segment != Member.Segment)
          continue;
        bool Seen = false;
        for (const TypeId &Owner : Owners)
          Seen = Seen || Owner == Other.OwnerType;
        if (!Seen)
          Owners.push_back(Other.OwnerType);
      }

      Detail::ReflectionRelationFields Relation;
      Relation.Kind = TypeRelationKind::Inherited;
      Relation.Type = Member.OwnerType;
      Relation.Declaration = Member.Declaration;
      Relation.Note = Member.Segment + " " +
                      std::string(SymbolKindText(Member.Kind)) +
                      " declared_by=" + Member.OwnerName +
                      (Owners.size() > 1 ? " ambiguous" : " unambiguous");
      Class->Record->Relations.push_back(std::move(Relation));
    }

    std::sort(Class->Record->Relations.begin(), Class->Record->Relations.end(),
              RelationPrecedes);
  }
}

[[nodiscard]] std::string
TransactionSubject(const Detail::RegistrationTransaction &Active) {
  const std::vector<std::size_t> Order = Active.Plan().CanonicalOrder();
  if (Order.empty())
    return "the requested symbols";

  const Detail::DescriptorPlanEntry *First = Active.Plan().At(Order.front());
  if (!First)
    return "the requested symbols";
  if (Active.Plan().Size() == 1 &&
      First->Category == Detail::PlanEntryKind::Function)
    return Detail::GlobalSubject(First->VmPath);
  return Detail::SubjectText(Detail::PlanEntryKindText(First->Category),
                             First->Symbol.QualifiedName);
}

[[nodiscard]] ErrorDiagnostic
InstallationDiagnostic(const Detail::InstallationOutcome &Outcome,
                       bool Restored) {
  const std::string Path = "for global '" + Outcome.Path + "'.";
  if (!Restored ||
      Outcome.Status == Detail::InstallationStatus::RestorationFailure)
    return Internal("Binding internal rollback failed " + Path);

  switch (Outcome.Status) {
  case Detail::InstallationStatus::MissingStagedResource:
    return Internal("Binding installation could not find the staged resource " +
                    Path);
  case Detail::InstallationStatus::JournalFailure:
    return Internal("Binding installation could not journal the prior value " +
                    Path);
  case Detail::InstallationStatus::StackCapacityFailure:
    return Internal("Binding installation could not reserve stack capacity " +
                    Path);
  default:
    break;
  }
  return Internal("Binding installation failed and was rolled back " + Path);
}

[[nodiscard]] ErrorDiagnostic
ConstantRequestDiagnostic(std::string_view Subject,
                          const Detail::ConstantRequest &Request) {
  switch (Request.Status) {
  case Detail::ConstantValueStatus::MissingStableKey:
    return Detail::MalformedMetadataDiagnostic(
        Subject, "a user-defined constant type requires an explicit validated "
                 "stable type key.");
  case Detail::ConstantValueStatus::OutOfRange:
    return Detail::ValueOutOfRangeDiagnostic(
        Subject, "the exact-integer domain Luna converts a constant through",
        static_cast<long long>(Request.ReceivedInteger),
        static_cast<long long>(std::numeric_limits<int>::min()),
        static_cast<long long>(std::numeric_limits<int>::max()));
  default:
    break;
  }
  return Detail::MalformedMetadataDiagnostic(
      Subject, "the value has no canonical Luna type.");
}

[[nodiscard]] TypeId IdentityOfDescriptor(const TypeDescriptor &Type) {
  if (const auto Identity = Detail::TypeIdentityRegistry::ComputeIdentity(Type))
    return *Identity;
  return TypeId();
}

[[nodiscard]] const Detail::TypeRecord *
FindPlannedOrCommittedType(const Detail::TypeGeneration &Types,
                           const Detail::RegistrationTransaction &Active,
                           const TypeDescriptor &Type) {
  for (const Detail::DescriptorPlanEntry &Planned :
       Active.Plan().PlannedEntries()) {
    if (Planned.TypeConversion && Planned.TypeConversion->Descriptor == Type)
      return &*Planned.TypeConversion;
  }
  return Types.Find(Type);
}

[[nodiscard]] std::optional<ErrorDiagnostic>
CheckDeclaredEnumeratorValue(std::string_view Subject,
                             const Detail::TypeGeneration &Types,
                             const Detail::RegistrationTransaction &Active,
                             const Detail::ConstantRequest &Request) {
  if (Request.Type.Kind() != TypeKind::Enumeration)
    return std::nullopt;

  const Detail::TypeRecord *Record =
      FindPlannedOrCommittedType(Types, Active, Request.Type);
  if (!Record || !Record->Enumeration)
    return std::nullopt;

  const int *Numeric = std::get_if<int>(&Request.Constant);
  if (!Numeric)
    return Detail::MalformedMetadataDiagnostic(
        Subject, "the enumeration constant carries no integral value.");

  if (Record->Enumeration->Accepts(static_cast<std::int64_t>(*Numeric)))
    return std::nullopt;

  if (Record->Enumeration->IsBitflags)
    return Detail::UnsupportedFlagBitsDiagnostic(
        Subject, static_cast<long long>(*Numeric),
        static_cast<long long>(Record->Enumeration->SupportedBits));
  return Detail::MalformedMetadataDiagnostic(
      Subject, "value " + std::to_string(*Numeric) +
                   " is not a declared enumerator of its enumeration.");
}

struct UnrecognizedCallbackFailure final {};

[[noreturn]] void ThrowFromRegistrationCallback(bool StandardException) {
  if (StandardException)
    throw std::runtime_error("registration callback threw");
  throw UnrecognizedCallbackFailure();
}

[[nodiscard]] std::string
ObserveGlobalValueKind(Detail::VirtualMachineOwner &Machine, bool Ready,
                       const std::string &Path) {
  if (!Ready)
    return "<unavailable>";
  Detail::SavedVmValue Saved;
  if (!Machine.CaptureGlobalValue(Path, Saved))
    return "<unavailable>";
  std::string Kind(Detail::VmValueKindText(Saved.Kind));
  Machine.ReleaseSavedValue(Saved);
  return Kind;
}

[[nodiscard]] ErrorDiagnostic
ConsistencyDiagnostic(std::string_view Subject,
                      Detail::ConsistencyStatus Status, bool Restored) {
  std::string Message =
      "Cannot register " + std::string(Subject) +
      ": internal metadata contradicted the attempt before publication (" +
      std::string(Detail::ConsistencyStatusText(Status)) + ")";
  Message += Restored ? "." : "; internal rollback failed.";
  return Internal(std::move(Message));
}

} // namespace

State::Impl::Impl()
    : Identities(Lifecycle.Identity()), LazyValues(Lifecycle.Identity()),
      Handle(std::make_shared<Detail::StateHandleToken>()) {
  Handle->Identity = Lifecycle.Identity();
  Handle->OwnerEpoch = Lifecycle.OwnerEpoch();

  Userdata.InstallCacheRemover([this](const Detail::NativeIdentity &Identity) {
    if (Identity.Address == nullptr)
      return;
    if (IsReady() && Userdata.PermitsVirtualMachineAccess()) {
      static_cast<void>(
          VirtualMachine.RetireExposedValue(UserdataAccess(), Identity));
      return;
    }

    static_cast<void>(LazyValues.Drop(Identity, nullptr));
    static_cast<void>(Identities.Forget(Identity));
  });

  Detail::PublishUserdataCollectionRoute(Lifecycle.Identity(), &Userdata);

  AsyncCalls.BindOrigin(Lifecycle.Identity(), Lifecycle.Generation());
  static_cast<void>(VirtualMachine.PublishAsyncCallRegistry(&AsyncCalls));
  static_cast<void>(VirtualMachine.PublishDelegateRegistry(&Delegates));
  static_cast<void>(
      VirtualMachine.PublishUserdataCaptureRegistry(&UserdataCaptures));
  static_cast<void>(VirtualMachine.PublishEnumItemRegistry(&EnumItems));
  static_cast<void>(VirtualMachine.PublishProfilingRegistry(&ProfilingHooks));
}

State::Impl::~Impl() {
  Destroying = true;

  AsyncCalls.CancelEverything("the State that suspended it is gone");
  Delegates.Retire();
  UserdataCaptures.Retire();
  EnumItems.Retire();

  FrozenCaches.reset();

  Detail::StateDestructionObservation Observed;
  Observed.Observed = true;
  const bool AcceptsAccess = UserdataAccess().IsUsable();
  const bool AcceptsExposure = UserdataExposure().IsUsable();
  Observed.RefusedNewInvocations =
      !IsReady() && !AcceptsAccess && !AcceptsExposure;

  Observed.RetainedCleanupMetadata = Userdata.RetainsCleanupMetadata();
  const Detail::UserdataCollectionCounters BeforeClose =
      Detail::ObserveUserdataCollections();
  VirtualMachine.Finalize();
  const Detail::UserdataCollectionCounters AfterClose =
      Detail::ObserveUserdataCollections();
  Observed.ReleasedDuringClose =
      static_cast<std::size_t>(AfterClose.Released - BeforeClose.Released);

  Observed.ReleasedAfterClose =
      Userdata.ReleaseAll(Detail::ReleaseCause::StateDestruction);
  Observed.IncompleteMetadata = Userdata.Counters().IncompleteMetadata;

  Identities.Clear();
  static_cast<void>(LazyValues.Clear());

  Detail::RetireUserdataCollectionRoute(Lifecycle.Identity());
  Detail::RecordStateDestruction(Observed);
}

bool State::Impl::IsReady() const noexcept {
  return !Destroying && VirtualMachine.IsReady();
}

void State::Impl::AdoptOwner(State &Object) noexcept {
  Handle->Owner = &Object;
  Handle->Identity = Lifecycle.Identity();
  Handle->OwnerEpoch = Lifecycle.OwnerEpoch();
}

void State::Impl::AdvanceOwnerEpoch() noexcept {
  Lifecycle.AdvanceOwnerEpoch();
  Handle->OwnerEpoch = Lifecycle.OwnerEpoch();
}

Detail::TransactionCapture State::Impl::CaptureTransactionEntry() const {
  Detail::TransactionCapture Capture;
  Capture.OwnerThread = Lifecycle.OwnerThread();

  if (!Lifecycle.IsOwnerThread()) {
    Capture.Generations = Detail::GenerationSet::Initial();
    return Capture;
  }

  Capture.VirtualMachineIsReady = IsReady();
  Capture.Phase = Lifecycle.Phase();

  Capture.EntryStackDepth =
      Lifecycle.IsOwnerThread() && Capture.VirtualMachineIsReady
          ? VirtualMachine.StackDepth()
          : 0;

  Capture.Identity = Lifecycle.Identity();
  Capture.OwnerEpoch = Lifecycle.OwnerEpoch();
  Capture.LifecycleGeneration = Lifecycle.Generation();
  Capture.Generations = CurrentGenerations();
  return Capture;
}

RegistrationResult State::Impl::ReportSubmissionFailure(
    const Detail::ActiveTransactionScope &Scope, ErrorDiagnostic Diagnostic) {
  Scope.ReportFailure(Diagnostic);
  if (Scope.IsOuter())
    Scope.Active().MarkRolledBack();
  return RegistrationResult::Failure(std::move(Diagnostic));
}

RegistrationResult State::Impl::SubmitFunctionDeclaration(
    const Detail::ActiveTransactionScope &Scope, std::string_view GlobalName,
    ErasedCallableDescriptor &&Descriptor,
    Detail::PreparedSubmission &Prepared) {
  Detail::RegistrationTransaction &Active = Scope.Active();

  Detail::DescriptorPlanEntry Planned = Detail::MakeFunctionPlanEntry(
      std::string(GlobalName), std::move(Descriptor));

  const Detail::OverloadJoinStatus Join = Detail::ClassifyOverloadJoin(
      Active.Symbols(), GlobalName,
      Planned.Symbol.Signature ? *Planned.Symbol.Signature
                               : Detail::CallableSignatureDescriptor());
  const bool JoinsOverloadSet =
      Join == Detail::OverloadJoinStatus::JoinsOverloadSet;

  if (JoinsOverloadSet)
    Detail::JoinPlannedOverloadSet(Planned);

  Detail::RegistrationValidationRequest Request;
  Request.Precedence = Detail::RegistrationPrecedence::FoundationRootFunction;
  Request.Name = GlobalName;
  Request.Entry = &Planned;
  Request.Category = Detail::PlanEntryKind::Function;
  Request.HasTarget = Planned.Callable && Planned.Callable->HasTarget();
  Request.JoinsOverloadSet = JoinsOverloadSet;
  Request.VmPathIsOccupied = !JoinsOverloadSet && Bindings.Contains(GlobalName);

  if (auto Diagnostic = Detail::ValidateRegistration(Request, Active.Entry(),
                                                     Active.Symbols()))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  const std::size_t PlanIndex = Active.Append(std::move(Planned));

  const std::string Subject = Detail::GlobalSubject(GlobalName);

  Detail::PreparedGenerations Candidate;
  const Detail::PreparationStatus Status =
      Faults.Consume(Detail::StateFaultPoint::TransactionPreparation)
          ? Detail::PreparationStatus::AllocationFailure
          : Detail::PrepareGenerations(Active, Reflection, Candidate);
  if (Status != Detail::PreparationStatus::Prepared)
    return ReportSubmissionFailure(
        Scope,
        PreparationDiagnostic(Subject, Status, Candidate.ReflectionStatus,
                              Candidate.TypeStatus));

  Prepared =
      Detail::PrepareFunctionResources(Active, PlanIndex, Bindings, Faults);
  if (!Prepared.IsPrepared())
    return ReportSubmissionFailure(Scope, std::move(*Prepared.Failure));

  return RegistrationResult::Success();
}

Detail::ParentScopeResolution
State::Impl::ResolveParentScope(const Detail::RegistrationTransaction &Active,
                                std::string_view ParentName) const {
  Detail::ParentScopeResolution Resolution;
  if (ParentName.empty())
    return Resolution;

  const auto Parent = Active.Symbols().Find(ParentName);
  if (!Parent || Parent->Category != Detail::PlanEntryKind::Scope ||
      !Parent->Symbol || Parent->Symbol->Kind != SymbolKind::Namespace) {
    Resolution.IsOwned = false;
    return Resolution;
  }

  Resolution.Identity = Parent->Identity;
  if (Parent->IsPending)
    return Resolution;

  const Detail::TransactionCapture &Entry = Active.Entry();
  const Detail::NamespaceOwnership *Ownership = Namespaces.Find(ParentName);
  const Detail::VmPathObservation Observed =
      VirtualMachine.ObserveVmPath(std::string(ParentName));
  if (!Ownership || !Detail::NamespaceOwnershipTable::Matches(
                        *Ownership, Entry.Identity, ParentName,
                        Parent->Identity, Observed.Table))
    Resolution.IsOwned = false;
  else if (!Detail::NamespaceOwnershipTable::IsCurrent(
               *Ownership, Entry.LifecycleGeneration))
    Resolution.IsCurrent = false;
  return Resolution;
}

RegistrationResult State::Impl::SubmitNamespaceDeclaration(
    const Detail::ActiveTransactionScope &Scope,
    const Detail::StagedNamespace &Declaration) {
  Detail::RegistrationTransaction &Active = Scope.Active();
  const std::string &Name = Declaration.QualifiedName;
  const std::string Subject = Detail::SubjectText(
      Detail::PlanEntryKindText(Detail::PlanEntryKind::Scope), Name);
  const Detail::TransactionCapture &Entry = Active.Entry();

  const auto Planned = Active.Symbols().Find(Name);
  if (Planned && Planned->IsPending &&
      Planned->Category == Detail::PlanEntryKind::Scope && Planned->Symbol &&
      Planned->Symbol->Kind == SymbolKind::Namespace)
    return RegistrationResult::Success();

  const std::string_view ParentName = Detail::ParentQualifiedName(Name);
  const Detail::ParentScopeResolution Parent =
      ResolveParentScope(Active, ParentName);
  const SymbolId ParentIdentity = Parent.Identity;
  const bool ScopeIsOwned = Parent.IsOwned;
  const bool ScopeIsCurrent = Parent.IsCurrent;

  const Detail::VmPathObservation Observed = VirtualMachine.ObserveVmPath(Name);
  const Detail::NamespaceOwnership *Ownership = Namespaces.Find(Name);
  const bool IsCommittedNamespace =
      Planned && !Planned->IsPending &&
      Planned->Category == Detail::PlanEntryKind::Scope && Planned->Symbol &&
      Planned->Symbol->Kind == SymbolKind::Namespace;
  const bool IsOwnedNamespace =
      IsCommittedNamespace && Ownership != nullptr &&
      Detail::NamespaceOwnershipTable::Matches(*Ownership, Entry.Identity, Name,
                                               Planned->Identity,
                                               Observed.Table) &&
      Detail::NamespaceOwnershipTable::IsCurrent(*Ownership,
                                                 Entry.LifecycleGeneration);

  if (IsOwnedNamespace)
    return RegistrationResult::Success();

  const bool UnownedPath = Observed.Exists();
  const std::string_view OccupantKind =
      UnownedPath ? Detail::VmValueKindText(Observed.Kind) : std::string_view();

  Detail::DescriptorPlanEntry Candidate =
      Detail::MakeNamespacePlanEntry(Name, ParentIdentity);

  Detail::ApplyDeclaredAnnotations(Candidate, Declaration.Documentation,
                                   Declaration.Attributes,
                                   Declaration.Examples);

  Detail::RegistrationValidationRequest Request;
  Request.Precedence = Detail::RegistrationPrecedence::GeneralOperation;
  Request.Name = Name;
  Request.Entry = &Candidate;
  Request.Category = Detail::PlanEntryKind::Scope;
  Request.ScopeIsOwned = ScopeIsOwned;
  Request.ScopeIsCurrent = ScopeIsCurrent;
  Request.ParentQualifiedName = ParentName;
  Request.VmPathHoldsUnownedValue = UnownedPath;
  Request.VmPathValueKindText = OccupantKind;

  if (auto Diagnostic =
          Detail::ValidateRegistration(Request, Entry, Active.Symbols()))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  static_cast<void>(Active.Append(std::move(Candidate)));

  Detail::PreparedGenerations Prepared;
  const Detail::PreparationStatus Status =
      Faults.Consume(Detail::StateFaultPoint::TransactionPreparation)
          ? Detail::PreparationStatus::AllocationFailure
          : Detail::PrepareGenerations(Active, Reflection, Prepared);
  if (Status != Detail::PreparationStatus::Prepared)
    return ReportSubmissionFailure(
        Scope, PreparationDiagnostic(Subject, Status, Prepared.ReflectionStatus,
                                     Prepared.TypeStatus));

  return RegistrationResult::Success();
}

RegistrationResult State::Impl::SubmitScopedFunctionDeclaration(
    const Detail::ActiveTransactionScope &Scope,
    const Detail::StagedFunction &Declaration) {
  Detail::RegistrationTransaction &Active = Scope.Active();
  const std::string &Name = Declaration.QualifiedName;
  const std::string Subject = Detail::SubjectText(
      Detail::PlanEntryKindText(Detail::PlanEntryKind::Function), Name);
  const Detail::TransactionCapture &Entry = Active.Entry();

  if (!Declaration.Callable)
    return ReportSubmissionFailure(Scope,
                                   Detail::NullCallableDiagnostic(Subject));

  const std::string_view ParentName = Detail::ParentQualifiedName(Name);
  const Detail::ParentScopeResolution Parent =
      ResolveParentScope(Active, ParentName);

  const Detail::VmPathObservation Observed = VirtualMachine.ObserveVmPath(Name);
  const bool UnownedPath = Observed.Exists();

  Detail::DescriptorPlanEntry Candidate = Detail::MakeFunctionPlanEntry(
      Name, std::move(*Declaration.Callable), Parent.Identity);

  Detail::ApplyDeclaredAnnotations(Candidate, Declaration.Documentation,
                                   Declaration.Attributes,
                                   Declaration.Examples);

  const Detail::OverloadJoinStatus Join = Detail::ClassifyOverloadJoin(
      Active.Symbols(), Name,
      Candidate.Symbol.Signature ? *Candidate.Symbol.Signature
                                 : Detail::CallableSignatureDescriptor());
  const bool JoinsOverloadSet =
      Join == Detail::OverloadJoinStatus::JoinsOverloadSet &&
      Bindings.Contains(Name);

  if (Join == Detail::OverloadJoinStatus::JoinsOverloadSet)
    Detail::JoinPlannedOverloadSet(Candidate);

  Detail::RegistrationValidationRequest Request;
  Request.Precedence = Detail::RegistrationPrecedence::GeneralOperation;
  Request.Name = Name;
  Request.Entry = &Candidate;
  Request.Category = Detail::PlanEntryKind::Function;
  Request.HasTarget = Candidate.Callable && Candidate.Callable->HasTarget();
  Request.ScopeIsOwned = Parent.IsOwned;
  Request.ScopeIsCurrent = Parent.IsCurrent;
  Request.ParentQualifiedName = ParentName;
  Request.JoinsOverloadSet = JoinsOverloadSet;
  Request.VmPathIsOccupied = !JoinsOverloadSet && Bindings.Contains(Name);
  Request.VmPathHoldsUnownedValue = UnownedPath && !JoinsOverloadSet;
  Request.VmPathValueKindText = Request.VmPathHoldsUnownedValue
                                    ? Detail::VmValueKindText(Observed.Kind)
                                    : std::string_view();

  if (auto Diagnostic =
          Detail::ValidateRegistration(Request, Entry, Active.Symbols()))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  const std::size_t PlanIndex = Active.Append(std::move(Candidate));

  if (auto Result = PrepareSubmittedDeclarations(Scope, Subject);
      !Result.IsSuccess())
    return Result;

  Detail::PreparedSubmission Prepared =
      Detail::PrepareFunctionResources(Active, PlanIndex, Bindings, Faults);
  if (!Prepared.IsPrepared())
    return ReportSubmissionFailure(Scope, std::move(*Prepared.Failure));

  return RegistrationResult::Success();
}

RegistrationResult State::Impl::SubmitConstantDeclaration(
    const Detail::ActiveTransactionScope &Scope,
    const Detail::StagedConstant &Declaration) {
  Detail::RegistrationTransaction &Active = Scope.Active();
  const std::string &Name = Declaration.QualifiedName;
  const std::string Subject =
      Detail::SubjectText(SymbolKindText(SymbolKind::Constant), Name);
  const Detail::TransactionCapture &Entry = Active.Entry();

  if (!Declaration.Request.IsSupported())
    return ReportSubmissionFailure(
        Scope, ConstantRequestDiagnostic(Subject, Declaration.Request));

  const std::string_view ParentName = Detail::ParentQualifiedName(Name);
  const Detail::ParentScopeResolution Parent =
      ResolveParentScope(Active, ParentName);

  const Detail::VmPathObservation Observed = VirtualMachine.ObserveVmPath(Name);
  const bool UnownedPath = Observed.Exists();

  const std::shared_ptr<const Detail::TypeGeneration> Types =
      Entry.SharedGenerations()->Types();
  Detail::DescriptorPlanEntry Candidate = Detail::MakeConstantPlanEntry(
      Declaration, Parent.Identity,
      IdentityOfDescriptor(Declaration.Request.Type));

  Detail::RegistrationValidationRequest Request;
  Request.Precedence = Detail::RegistrationPrecedence::GeneralOperation;
  Request.Name = Name;
  Request.Entry = &Candidate;
  Request.Category = Detail::PlanEntryKind::Value;
  Request.SubjectKindText = SymbolKindText(SymbolKind::Constant);
  Request.ScopeIsOwned = Parent.IsOwned;
  Request.ScopeIsCurrent = Parent.IsCurrent;
  Request.ParentQualifiedName = ParentName;
  Request.VmPathHoldsUnownedValue = UnownedPath;
  Request.VmPathValueKindText =
      UnownedPath ? Detail::VmValueKindText(Observed.Kind) : std::string_view();

  if (auto Diagnostic =
          Detail::ValidateRegistration(Request, Entry, Active.Symbols()))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  if (auto Diagnostic = CheckDeclaredEnumeratorValue(Subject, *Types, Active,
                                                     Declaration.Request))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  static_cast<void>(Active.Append(std::move(Candidate)));
  return PrepareSubmittedDeclarations(Scope, Subject);
}

RegistrationResult
State::Impl::SubmitValueDeclaration(const Detail::ActiveTransactionScope &Scope,
                                    const Detail::StagedValue &Declaration) {
  Detail::RegistrationTransaction &Active = Scope.Active();
  const std::string &Name = Declaration.QualifiedName;
  const std::string Subject =
      Detail::SubjectText(SymbolKindText(SymbolKind::Constant), Name);
  const Detail::TransactionCapture &Entry = Active.Entry();

  if (auto Diagnostic = Detail::ValidateStagedValue(Declaration))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  const std::string_view ParentName = Detail::ParentQualifiedName(Name);
  const Detail::ParentScopeResolution Parent =
      ResolveParentScope(Active, ParentName);

  const Detail::VmPathObservation Observed = VirtualMachine.ObserveVmPath(Name);
  const bool UnownedPath = Observed.Exists();

  const TypeDescriptor Class = TypeDescriptor::ForClass(Declaration.Class);
  Detail::DescriptorPlanEntry Candidate = Detail::MakeInstanceValuePlanEntry(
      Declaration, Parent.Identity, IdentityOfDescriptor(Class));

  Detail::RegistrationValidationRequest Request;
  Request.Precedence = Detail::RegistrationPrecedence::GeneralOperation;
  Request.Name = Name;
  Request.Entry = &Candidate;
  Request.Category = Detail::PlanEntryKind::Value;
  Request.SubjectKindText = SymbolKindText(SymbolKind::Constant);
  Request.ScopeIsOwned = Parent.IsOwned;
  Request.ScopeIsCurrent = Parent.IsCurrent;
  Request.ParentQualifiedName = ParentName;
  Request.VmPathHoldsUnownedValue = UnownedPath;
  Request.VmPathValueKindText =
      UnownedPath ? Detail::VmValueKindText(Observed.Kind) : std::string_view();
  Request.DeclaredValueType = &Class;

  if (auto Diagnostic =
          Detail::ValidateRegistration(Request, Entry, Active.Symbols()))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  static_cast<void>(Active.Append(std::move(Candidate)));
  return PrepareSubmittedDeclarations(Scope, Subject);
}

RegistrationResult State::Impl::SubmitEnumerationDeclaration(
    const Detail::ActiveTransactionScope &Scope,
    const Detail::StagedEnumeration &Declaration) {
  Detail::RegistrationTransaction &Active = Scope.Active();
  const std::string &Name = Declaration.QualifiedName;
  const std::string Subject =
      Detail::SubjectText(SymbolKindText(SymbolKind::Enumeration), Name);
  const Detail::TransactionCapture &Entry = Active.Entry();

  if (auto Diagnostic = Detail::ValidateStagedEnumeration(Declaration))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  const std::string_view ParentName = Detail::ParentQualifiedName(Name);
  const Detail::ParentScopeResolution Parent =
      ResolveParentScope(Active, ParentName);

  const Detail::VmPathObservation Observed = VirtualMachine.ObserveVmPath(Name);
  const bool UnownedPath = Observed.Exists();

  Detail::DescriptorPlanEntry Candidate =
      Detail::MakeEnumerationPlanEntry(Declaration, Parent.Identity);
  const SymbolId Enumeration = Candidate.Identity;
  const TypeId Type =
      Candidate.TypeFields ? Candidate.TypeFields->Id : TypeId();

  Detail::RegistrationValidationRequest Request;
  Request.Precedence = Detail::RegistrationPrecedence::GeneralOperation;
  Request.Name = Name;
  Request.Entry = &Candidate;
  Request.Category = Detail::PlanEntryKind::Scope;
  Request.SubjectKindText = SymbolKindText(SymbolKind::Enumeration);
  Request.ScopeIsOwned = Parent.IsOwned;
  Request.ScopeIsCurrent = Parent.IsCurrent;
  Request.ParentQualifiedName = ParentName;
  Request.VmPathHoldsUnownedValue = UnownedPath;
  Request.VmPathValueKindText =
      UnownedPath ? Detail::VmValueKindText(Observed.Kind) : std::string_view();

  if (auto Diagnostic =
          Detail::ValidateRegistration(Request, Entry, Active.Symbols()))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  static_cast<void>(Active.Append(std::move(Candidate)));

  std::vector<std::pair<std::string, SymbolId>> Canonical;
  for (const Detail::StagedEnumerator &Enumerator : Declaration.Enumerators) {
    if (Enumerator.IsAlias)
      continue;
    Detail::DescriptorPlanEntry Member = Detail::MakeEnumeratorPlanEntry(
        Declaration, Enumerator, Enumeration, Type, SymbolId());
    if (auto Diagnostic = CheckPlannedMember(Scope, Member))
      return ReportSubmissionFailure(Scope, std::move(*Diagnostic));
    Canonical.emplace_back(Enumerator.Segment, Member.Identity);
    static_cast<void>(Active.Append(std::move(Member)));
  }

  for (const Detail::StagedEnumerator &Enumerator : Declaration.Enumerators) {
    if (!Enumerator.IsAlias)
      continue;
    SymbolId Target;
    for (const auto &Declared : Canonical) {
      if (Declared.first == Enumerator.CanonicalSegment) {
        Target = Declared.second;
        break;
      }
    }
    Detail::DescriptorPlanEntry Member = Detail::MakeEnumeratorPlanEntry(
        Declaration, Enumerator, Enumeration, Type, Target);
    if (auto Diagnostic = CheckPlannedMember(Scope, Member))
      return ReportSubmissionFailure(Scope, std::move(*Diagnostic));
    static_cast<void>(Active.Append(std::move(Member)));
  }

  return PrepareSubmittedDeclarations(Scope, Subject);
}

RegistrationResult State::Impl::SubmitClassDeclaration(
    const Detail::ActiveTransactionScope &Scope,
    const Detail::StagedClass &Declaration,
    const Detail::RelationshipCandidate &Relationships) {
  Detail::RegistrationTransaction &Active = Scope.Active();
  const std::string &Name = Declaration.QualifiedName;
  const std::string Subject =
      Detail::SubjectText(SymbolKindText(SymbolKind::Class), Name);
  const Detail::TransactionCapture &Entry = Active.Entry();

  if (auto Diagnostic = Detail::ValidateStagedClass(Declaration))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  const std::string_view ParentName = Detail::ParentQualifiedName(Name);
  const Detail::ParentScopeResolution Parent =
      ResolveParentScope(Active, ParentName);

  const Detail::VmPathObservation Observed = VirtualMachine.ObserveVmPath(Name);
  const bool UnownedPath = Observed.Exists();

  Detail::DescriptorPlanEntry Candidate =
      Detail::MakeClassPlanEntry(Declaration, Parent.Identity);

  Detail::RegistrationValidationRequest Request;
  Request.Precedence = Detail::RegistrationPrecedence::GeneralOperation;
  Request.Name = Name;
  Request.Entry = &Candidate;
  Request.Category = Detail::PlanEntryKind::ClassSymbol;
  Request.SubjectKindText = SymbolKindText(SymbolKind::Class);
  Request.ScopeIsOwned = Parent.IsOwned;
  Request.ScopeIsCurrent = Parent.IsCurrent;
  Request.ParentQualifiedName = ParentName;
  Request.VmPathHoldsUnownedValue = UnownedPath;
  Request.VmPathValueKindText =
      UnownedPath ? Detail::VmValueKindText(Observed.Kind) : std::string_view();

  if (auto Diagnostic =
          Detail::ValidateRegistration(Request, Entry, Active.Symbols()))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  const SymbolId ClassSymbol = Candidate.Identity;
  static_cast<void>(Active.Append(std::move(Candidate)));

  Detail::DescriptorPlanEntry Metatable =
      Detail::MakeClassMetatablePlanEntry(Declaration, ClassSymbol);
  if (auto Diagnostic = CheckPlannedMember(Scope, Metatable))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));
  static_cast<void>(Active.Append(std::move(Metatable)));

  for (const Detail::StagedConstant &Member : Declaration.Constants) {
    if (auto Result = SubmitClassConstantDeclaration(Scope, Declaration,
                                                     ClassSymbol, Member);
        !Result.IsSuccess())
      return Result;
  }

  for (const Detail::StagedConstruction &Member : Declaration.Constructions) {
    if (auto Result = SubmitConstructionDeclaration(Scope, Declaration,
                                                    ClassSymbol, Member);
        !Result.IsSuccess())
      return Result;
  }

  for (const Detail::StagedMethod &Member : Declaration.Methods) {
    if (auto Result = SubmitMemberDeclaration(Scope, Declaration, ClassSymbol,
                                              Member, Relationships);
        !Result.IsSuccess())
      return Result;
  }

  for (const Detail::StagedMember &Member : Declaration.Members) {
    if (auto Result = SubmitAccessorDeclaration(Scope, Declaration, ClassSymbol,
                                                Member, Relationships);
        !Result.IsSuccess())
      return Result;
  }

  for (const Detail::StagedOperator &Member : Declaration.Operators) {
    if (auto Result =
            SubmitOperatorDeclaration(Scope, Declaration, ClassSymbol, Member);
        !Result.IsSuccess())
      return Result;
  }

  return PrepareSubmittedDeclarations(Scope, Subject);
}

RegistrationResult State::Impl::SubmitClassConstantDeclaration(
    const Detail::ActiveTransactionScope &Scope,
    const Detail::StagedClass &Class, const SymbolId &ClassSymbol,
    const Detail::StagedConstant &Declaration) {
  Detail::RegistrationTransaction &Active = Scope.Active();
  const std::string &Name = Declaration.QualifiedName;
  const std::string Subject =
      Detail::SubjectText(SymbolKindText(SymbolKind::Constant), Name);
  const Detail::TransactionCapture &Entry = Active.Entry();

  if (!Declaration.Request.IsSupported())
    return ReportSubmissionFailure(
        Scope, ConstantRequestDiagnostic(Subject, Declaration.Request));

  const std::shared_ptr<const Detail::TypeGeneration> Types =
      Entry.SharedGenerations()->Types();

  const Detail::VmPathObservation Observed = VirtualMachine.ObserveVmPath(Name);
  const bool UnownedPath = Observed.Exists();

  Detail::DescriptorPlanEntry Candidate = Detail::MakeConstantPlanEntry(
      Declaration, ClassSymbol, IdentityOfDescriptor(Declaration.Request.Type));

  Detail::RegistrationValidationRequest Request;
  Request.Precedence = Detail::RegistrationPrecedence::GeneralOperation;
  Request.Name = Name;
  Request.Entry = &Candidate;
  Request.Category = Detail::PlanEntryKind::Value;
  Request.SubjectKindText = SymbolKindText(SymbolKind::Constant);
  Request.ScopeIsOwned = true;
  Request.ScopeIsCurrent = true;
  Request.ParentQualifiedName = Class.QualifiedName;
  Request.VmPathIsOccupied = Bindings.Contains(Name);
  Request.VmPathHoldsUnownedValue = UnownedPath;
  Request.VmPathValueKindText =
      UnownedPath ? Detail::VmValueKindText(Observed.Kind) : std::string_view();

  if (auto Diagnostic =
          Detail::ValidateRegistration(Request, Entry, Active.Symbols()))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  if (auto Diagnostic = CheckDeclaredEnumeratorValue(Subject, *Types, Active,
                                                     Declaration.Request))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  static_cast<void>(Active.Append(std::move(Candidate)));
  return PrepareSubmittedDeclarations(Scope, Subject);
}

RegistrationResult State::Impl::SubmitConstructionDeclaration(
    const Detail::ActiveTransactionScope &Scope,
    const Detail::StagedClass &Class, const SymbolId &ClassSymbol,
    const Detail::StagedConstruction &Declaration) {
  Detail::RegistrationTransaction &Active = Scope.Active();
  const std::string &Name = Declaration.QualifiedName;
  const std::string Subject =
      Detail::SubjectText(SymbolKindText(Declaration.Kind), Name);
  const Detail::TransactionCapture &Entry = Active.Entry();

  if (auto Diagnostic = Detail::ValidateStagedConstruction(Class, Declaration))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  const Detail::VmPathObservation Observed = VirtualMachine.ObserveVmPath(Name);
  const bool UnownedPath = Observed.Exists();

  Detail::DescriptorPlanEntry Candidate =
      Detail::MakeConstructionPlanEntry(Class, Declaration, ClassSymbol);

  const Detail::OverloadJoinStatus Join = Detail::ClassifyOverloadJoin(
      Active.Symbols(), Name,
      Candidate.Symbol.Signature ? *Candidate.Symbol.Signature
                                 : Detail::CallableSignatureDescriptor());
  const bool JoinsOverloadSet =
      Join == Detail::OverloadJoinStatus::JoinsOverloadSet &&
      Bindings.Contains(Name);
  if (Join == Detail::OverloadJoinStatus::JoinsOverloadSet)
    Detail::JoinPlannedOverloadSet(Candidate);

  Detail::RegistrationValidationRequest Request;
  Request.Precedence = Detail::RegistrationPrecedence::GeneralOperation;
  Request.Name = Name;
  Request.Entry = &Candidate;
  Request.Category = Detail::PlanEntryKind::Function;
  Request.SubjectKindText = SymbolKindText(Declaration.Kind);
  Request.HasTarget = Candidate.Callable && Candidate.Callable->HasTarget();

  Request.ScopeIsOwned = true;
  Request.ScopeIsCurrent = true;
  Request.ParentQualifiedName = Class.QualifiedName;
  Request.JoinsOverloadSet = JoinsOverloadSet;
  Request.VmPathIsOccupied = !JoinsOverloadSet && Bindings.Contains(Name);
  Request.VmPathHoldsUnownedValue = UnownedPath && !JoinsOverloadSet;
  Request.VmPathValueKindText = Request.VmPathHoldsUnownedValue
                                    ? Detail::VmValueKindText(Observed.Kind)
                                    : std::string_view();

  if (auto Diagnostic =
          Detail::ValidateRegistration(Request, Entry, Active.Symbols()))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  const std::size_t PlanIndex = Active.Append(std::move(Candidate));

  if (auto Result = PrepareSubmittedDeclarations(Scope, Subject);
      !Result.IsSuccess())
    return Result;

  Detail::PreparedSubmission Prepared =
      Detail::PrepareFunctionResources(Active, PlanIndex, Bindings, Faults);
  if (!Prepared.IsPrepared())
    return ReportSubmissionFailure(Scope, std::move(*Prepared.Failure));

  return RegistrationResult::Success();
}

RegistrationResult State::Impl::SubmitMemberDeclaration(
    const Detail::ActiveTransactionScope &Scope,
    const Detail::StagedClass &Class, const SymbolId &ClassSymbol,
    const Detail::StagedMethod &Declaration,
    const Detail::RelationshipCandidate &Relationships) {
  Detail::RegistrationTransaction &Active = Scope.Active();
  const std::string &Name = Declaration.QualifiedName;
  const std::string Subject =
      Detail::SubjectText(SymbolKindText(Declaration.Kind), Name);
  const Detail::TransactionCapture &Entry = Active.Entry();

  if (auto Diagnostic = Detail::ValidateStagedMethod(Class, Declaration))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  Detail::MemberCollisionRequest Collision;
  Collision.Segment = Declaration.Segment;
  Collision.QualifiedName = Name;
  Collision.Kind = Declaration.Kind;
  Collision.InheritedNameIsAmbiguous =
      Relationships.InheritedDeclarationCount(
          Detail::ClassTypeIdentityOf(Class.Key), Declaration.Segment) > 1;
  if (auto Diagnostic = Detail::DiagnoseMemberCollision(Collision))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  const Detail::VmPathObservation Observed = VirtualMachine.ObserveVmPath(Name);
  const bool UnownedPath = Observed.Exists();

  Detail::DescriptorPlanEntry Candidate =
      Detail::MakeMethodPlanEntry(Class, Declaration, ClassSymbol);

  const Detail::OverloadJoinStatus Join = Detail::ClassifyOverloadJoin(
      Active.Symbols(), Name,
      Candidate.Symbol.Signature ? *Candidate.Symbol.Signature
                                 : Detail::CallableSignatureDescriptor());
  const bool JoinsOverloadSet =
      Join == Detail::OverloadJoinStatus::JoinsOverloadSet &&
      Bindings.Contains(Name);
  if (Join == Detail::OverloadJoinStatus::JoinsOverloadSet)
    Detail::JoinPlannedOverloadSet(Candidate);

  Detail::RegistrationValidationRequest Request;
  Request.Precedence = Detail::RegistrationPrecedence::GeneralOperation;
  Request.Name = Name;
  Request.Entry = &Candidate;
  Request.Category = Detail::PlanEntryKind::Function;
  Request.SubjectKindText = SymbolKindText(Declaration.Kind);
  Request.HasTarget = Candidate.Callable && Candidate.Callable->HasTarget();

  Request.ScopeIsOwned = true;
  Request.ScopeIsCurrent = true;
  Request.ParentQualifiedName = Class.QualifiedName;
  Request.JoinsOverloadSet = JoinsOverloadSet;
  Request.VmPathIsOccupied = !JoinsOverloadSet && Bindings.Contains(Name);
  Request.VmPathHoldsUnownedValue = UnownedPath && !JoinsOverloadSet;
  Request.VmPathValueKindText = Request.VmPathHoldsUnownedValue
                                    ? Detail::VmValueKindText(Observed.Kind)
                                    : std::string_view();

  if (auto Diagnostic =
          Detail::ValidateRegistration(Request, Entry, Active.Symbols()))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  const std::size_t PlanIndex = Active.Append(std::move(Candidate));

  if (auto Result = PrepareSubmittedDeclarations(Scope, Subject);
      !Result.IsSuccess())
    return Result;

  Detail::PreparedSubmission Prepared =
      Detail::PrepareFunctionResources(Active, PlanIndex, Bindings, Faults);
  if (!Prepared.IsPrepared())
    return ReportSubmissionFailure(Scope, std::move(*Prepared.Failure));

  return RegistrationResult::Success();
}

RegistrationResult State::Impl::SubmitOperatorDeclaration(
    const Detail::ActiveTransactionScope &Scope,
    const Detail::StagedClass &Class, const SymbolId &ClassSymbol,
    const Detail::StagedOperator &Declaration) {
  Detail::RegistrationTransaction &Active = Scope.Active();
  const std::string &Name = Declaration.QualifiedName;
  const std::string Subject =
      Detail::SubjectText(SymbolKindText(SymbolKind::Operator), Name);
  const Detail::TransactionCapture &Entry = Active.Entry();

  if (auto Diagnostic = Detail::ValidateStagedOperator(Declaration))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  const Detail::VmPathObservation Observed = VirtualMachine.ObserveVmPath(Name);
  const bool UnownedPath = Observed.Exists();

  Detail::DescriptorPlanEntry Candidate =
      Detail::MakeOperatorPlanEntry(Class, Declaration, ClassSymbol);

  const Detail::OverloadJoinStatus Join = Detail::ClassifyOverloadJoin(
      Active.Symbols(), Name,
      Candidate.Symbol.Signature ? *Candidate.Symbol.Signature
                                 : Detail::CallableSignatureDescriptor());
  const bool JoinsOverloadSet =
      Join == Detail::OverloadJoinStatus::JoinsOverloadSet &&
      Bindings.Contains(Name);
  if (Join == Detail::OverloadJoinStatus::JoinsOverloadSet)
    Detail::JoinPlannedOverloadSet(Candidate);

  Detail::RegistrationValidationRequest Request;
  Request.Precedence = Detail::RegistrationPrecedence::GeneralOperation;
  Request.Name = Name;
  Request.Entry = &Candidate;
  Request.Category = Detail::PlanEntryKind::Function;
  Request.SubjectKindText = SymbolKindText(SymbolKind::Operator);
  Request.HasTarget = Candidate.Callable && Candidate.Callable->HasTarget();
  Request.ScopeIsOwned = true;
  Request.ScopeIsCurrent = true;
  Request.ParentQualifiedName = Class.QualifiedName;
  Request.JoinsOverloadSet = JoinsOverloadSet;
  Request.VmPathIsOccupied = !JoinsOverloadSet && Bindings.Contains(Name);
  Request.VmPathHoldsUnownedValue = UnownedPath && !JoinsOverloadSet;
  Request.VmPathValueKindText = Request.VmPathHoldsUnownedValue
                                    ? Detail::VmValueKindText(Observed.Kind)
                                    : std::string_view();

  if (auto Diagnostic =
          Detail::ValidateRegistration(Request, Entry, Active.Symbols()))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  const std::size_t PlanIndex = Active.Append(std::move(Candidate));

  if (auto Result = PrepareSubmittedDeclarations(Scope, Subject);
      !Result.IsSuccess())
    return Result;

  Detail::PreparedSubmission Prepared =
      Detail::PrepareFunctionResources(Active, PlanIndex, Bindings, Faults);
  if (!Prepared.IsPrepared())
    return ReportSubmissionFailure(Scope, std::move(*Prepared.Failure));

  return RegistrationResult::Success();
}

RegistrationResult State::Impl::SubmitAccessorDeclaration(
    const Detail::ActiveTransactionScope &Scope,
    const Detail::StagedClass &Class, const SymbolId &ClassSymbol,
    const Detail::StagedMember &Declaration,
    const Detail::RelationshipCandidate &Relationships) {
  Detail::RegistrationTransaction &Active = Scope.Active();
  const std::string &Name = Declaration.QualifiedName;
  const std::string Subject =
      Detail::SubjectText(SymbolKindText(Declaration.Kind), Name);
  const Detail::TransactionCapture &Entry = Active.Entry();

  if (auto Diagnostic = Detail::ValidateStagedMember(Declaration))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  Detail::MemberCollisionRequest Collision;
  Collision.Segment = Declaration.Segment;
  Collision.QualifiedName = Name;
  Collision.Kind = Declaration.Kind;
  if (const auto Existing = Active.Symbols().Find(Name)) {
    Collision.NameIsDeclared = true;
    if (Existing->Symbol != nullptr)
      Collision.ExistingKind = Existing->Symbol->Kind;
    Collision.ExistingCategory = Existing->Category;
    Collision.ExistingIsPending = Existing->IsPending;
  }
  Collision.InheritedNameIsAmbiguous =
      Relationships.InheritedDeclarationCount(
          Detail::ClassTypeIdentityOf(Class.Key), Declaration.Segment) > 1;
  if (auto Diagnostic = Detail::DiagnoseMemberCollision(Collision))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  Detail::DescriptorPlanEntry Candidate = Detail::MakeMemberPlanEntry(
      Declaration, Detail::ClassTypeOf(Class), ClassSymbol);

  Detail::RegistrationValidationRequest Request;
  Request.Precedence = Detail::RegistrationPrecedence::GeneralOperation;
  Request.Name = Name;
  Request.Entry = &Candidate;
  Request.Category = Detail::PlanEntryKind::ClassMember;
  Request.SubjectKindText = SymbolKindText(Declaration.Kind);

  Request.HasTarget = true;
  Request.ScopeIsOwned = true;
  Request.ScopeIsCurrent = true;
  Request.ParentQualifiedName = Class.QualifiedName;
  Request.DeclaredValueType = &Declaration.ValueType;

  if (auto Diagnostic =
          Detail::ValidateRegistration(Request, Entry, Active.Symbols()))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  static_cast<void>(Active.Append(std::move(Candidate)));
  return PrepareSubmittedDeclarations(Scope, Subject);
}

std::optional<ErrorDiagnostic>
State::Impl::CheckPlannedMember(const Detail::ActiveTransactionScope &Scope,
                                const Detail::DescriptorPlanEntry &Member) {
  const std::string Subject = Detail::SubjectText(
      SymbolKindText(Member.Symbol.Kind), Member.Symbol.QualifiedName);

  if (!Member.IsValid())
    return Detail::MalformedMetadataDiagnostic(
        Subject, "the canonical descriptor is incomplete.");

  if (const auto Existing =
          Scope.Active().Symbols().Find(Member.Symbol.QualifiedName)) {
    if (Existing->Category == Member.Category)
      return Detail::DuplicateNameDiagnostic(Subject);
    return Detail::IncompatibleCategoryDiagnostic(
        Subject, Detail::PlanEntryKindText(Existing->Category),
        Existing->IsPending);
  }
  return std::nullopt;
}

RegistrationResult State::Impl::PrepareSubmittedDeclarations(
    const Detail::ActiveTransactionScope &Scope, const std::string &Subject) {
  Detail::PreparedGenerations Prepared;
  const Detail::PreparationStatus Status =
      Faults.Consume(Detail::StateFaultPoint::TransactionPreparation)
          ? Detail::PreparationStatus::AllocationFailure
          : Detail::PrepareGenerations(Scope.Active(), Reflection, Prepared);
  if (Status != Detail::PreparationStatus::Prepared)
    return ReportSubmissionFailure(
        Scope, PreparationDiagnostic(Subject, Status, Prepared.ReflectionStatus,
                                     Prepared.TypeStatus));
  return RegistrationResult::Success();
}

RegistrationResult State::Impl::SubmitPlanDeclarations(
    const Detail::ActiveTransactionScope &Scope,
    const Detail::BuilderPlan &Plan,
    const std::optional<ErrorDiagnostic> &StagedFailure) {
  if (StagedFailure)
    return ReportSubmissionFailure(Scope, *StagedFailure);

  for (const Detail::StagedNamespace &Declaration : Plan.Namespaces) {
    if (auto Result = SubmitNamespaceDeclaration(Scope, Declaration);
        !Result.IsSuccess())
      return Result;
  }
  for (const Detail::StagedEnumeration &Declaration : Plan.Enumerations) {
    if (auto Result = SubmitEnumerationDeclaration(Scope, Declaration);
        !Result.IsSuccess())
      return Result;
  }

  const Detail::RelationshipCandidate Relationships =
      BuildRelationshipCandidate(Plan);
  if (auto Diagnostic = Detail::ValidateRelationshipCandidate(Relationships))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  for (const Detail::StagedClass &Declaration : Plan.Classes) {
    if (auto Result = SubmitClassDeclaration(Scope, Declaration, Relationships);
        !Result.IsSuccess())
      return Result;
  }
  for (const Detail::StagedConstant &Declaration : Plan.Constants) {
    if (auto Result = SubmitConstantDeclaration(Scope, Declaration);
        !Result.IsSuccess())
      return Result;
  }
  for (const Detail::StagedValue &Declaration : Plan.Values) {
    if (auto Result = SubmitValueDeclaration(Scope, Declaration);
        !Result.IsSuccess())
      return Result;
  }
  for (const Detail::StagedFunction &Declaration : Plan.Functions) {
    if (auto Result = SubmitScopedFunctionDeclaration(Scope, Declaration);
        !Result.IsSuccess())
      return Result;
  }

  for (const Detail::StagedModule &Request : Plan.Modules) {
    if (auto Result = SubmitModuleLoad(Scope, Request); !Result.IsSuccess())
      return Result;
  }

  const std::shared_ptr<const Detail::ReflectionStorage> CapturedReflection =
      Scope.Active().Captured().Reflection();
  if (!Plan.Classes.empty() && CapturedReflection) {
    AttachCanonicalClassRelations(Scope.Active().Plan(), Relationships, Classes,
                                  *CapturedReflection);
    if (auto Result = PrepareSubmittedDeclarations(
            Scope, Detail::SubjectText(Detail::PlanEntryKindText(
                                           Detail::PlanEntryKind::ClassSymbol),
                                       "the class surface"));
        !Result.IsSuccess())
      return Result;
  }
  return RegistrationResult::Success();
}

RegistrationResult State::Impl::RegisterBuilderPlan(
    const Detail::BuilderPlan &Plan,
    const std::optional<ErrorDiagnostic> &StagedFailure) {
  std::string_view First("the requested symbols");
  if (!Plan.Namespaces.empty())
    First = Plan.Namespaces.front().QualifiedName;
  else if (!Plan.Enumerations.empty())
    First = Plan.Enumerations.front().QualifiedName;
  else if (!Plan.Classes.empty())
    First = Plan.Classes.front().QualifiedName;
  else if (!Plan.Constants.empty())
    First = Plan.Constants.front().QualifiedName;
  else if (!Plan.Functions.empty())
    First = Plan.Functions.front().QualifiedName;
  else if (!Plan.Modules.empty())
    First = Plan.Modules.front().Manifest.Identity();
  const std::string Subject = Detail::SubjectText(
      Detail::PlanEntryKindText(Detail::PlanEntryKind::Scope), First);

  if (!IsOwnerThread())
    return RegistrationResult::Failure(
        Detail::ForeignThreadDiagnostic(Subject));

  Detail::RegistrationTransaction Transaction(CaptureTransactionEntry());
  const Detail::ActiveTransactionScope Scope(ActiveTransaction, Transaction);

  if (auto Diagnostic =
          Detail::ValidateTransactionEntry(Transaction.Entry(), Subject))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  if (auto Result = SubmitPlanDeclarations(Scope, Plan, StagedFailure);
      !Result.IsSuccess()) {
    AbandonAttempt(Scope);
    return Result;
  }

  if (!Scope.IsOuter())
    return RegistrationResult::Success();

  if (Transaction.Plan().IsEmpty()) {
    Transaction.MarkCommitted();
    PublishPendingModules();
    return RegistrationResult::Success();
  }

  return CompleteOutermostTransaction(Scope, nullptr);
}

Detail::ModuleCatalog State::Impl::CandidateModuleCatalog() const {
  Detail::ModuleCatalog Candidate = Definitions.Catalog();
  for (const Detail::ModuleDefinition &Provided : PendingDefinitions)
    static_cast<void>(Candidate.Add(Provided.Manifest));
  return Candidate;
}

const Detail::ModuleDefinition *
State::Impl::FindModuleDefinition(std::string_view Identity,
                                  const SemanticVersion &Version) const {
  for (const Detail::ModuleDefinition &Provided : PendingDefinitions) {
    const ModuleManifest &Manifest = Provided.Manifest;
    if (Manifest.Identity() == Identity &&
        Manifest.Version().HasSamePrecedence(Version))
      return &Provided;
  }
  return Definitions.Find(Identity, Version);
}

const ModuleManifest *
State::Impl::FindPendingModule(std::string_view Identity) const noexcept {
  for (const ModuleManifest &Planned : PendingModules) {
    if (Planned.Identity() == Identity)
      return &Planned;
  }
  return nullptr;
}

void State::Impl::PublishPendingModules() {
  for (ModuleManifest &Loaded : PendingModules)
    static_cast<void>(Modules.Record(std::move(Loaded)));
  for (Detail::ModuleDefinition &Provided : PendingDefinitions)
    static_cast<void>(Definitions.Add(std::move(Provided)));
  PendingModules.clear();
  PendingDefinitions.clear();
}

void State::Impl::DiscardPendingModules() noexcept {
  PendingModules.clear();
  PendingDefinitions.clear();
}

void State::Impl::AbandonAttempt(
    const Detail::ActiveTransactionScope &Scope) noexcept {
  if (!Scope.IsOuter())
    return;
  DiscardStagedResources(Scope.Active());
  DiscardPendingModules();
  Scope.Active().MarkRolledBack();
}

RegistrationResult
State::Impl::ProvideModuleDefinition(ModuleManifest Manifest,
                                     Detail::ModuleRegistration Registration) {
  const std::string Subject = Detail::ModuleSubject(Manifest);
  if (!IsOwnerThread())
    return RegistrationResult::Failure(
        Detail::ForeignThreadDiagnostic(Subject));

  Detail::RegistrationTransaction Transaction(CaptureTransactionEntry());
  const Detail::ActiveTransactionScope Scope(ActiveTransaction, Transaction);

  if (auto Diagnostic =
          Detail::ValidateTransactionEntry(Transaction.Entry(), Subject))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  if (!Manifest.IsValid())
    return ReportSubmissionFailure(
        Scope,
        Detail::MalformedMetadataDiagnostic(
            Subject,
            "the manifest is not valid (" +
                std::string(ModuleManifestStatusText(Manifest.Status())) +
                ")."));
  if (!Registration.IsValid())
    return ReportSubmissionFailure(
        Scope, Detail::MalformedMetadataDiagnostic(
                   Subject, "the module carries no scoped registration "
                            "callback."));

  const Detail::ModuleCatalog Candidate = CandidateModuleCatalog();
  if (const ModuleManifest *Available =
          Candidate.Find(Manifest.Identity(), Manifest.Version())) {
    if (!(*Available == Manifest))
      return ReportSubmissionFailure(
          Scope, Detail::ModuleConflictDiagnostic(
                     Subject, "a different definition of this identity and "
                              "version is already available."));
    Transaction.MarkCommitted();
    return RegistrationResult::Success();
  }

  Detail::ModuleDefinition Definition;
  Definition.Manifest = std::move(Manifest);
  Definition.Registration = std::move(Registration);

  if (!Scope.IsOuter()) {
    PendingDefinitions.push_back(std::move(Definition));
    return RegistrationResult::Success();
  }

  const Detail::ModuleDefinitionLibrary::AddStatus Added =
      Definitions.Add(std::move(Definition));
  if (Added != Detail::ModuleDefinitionLibrary::AddStatus::Added &&
      Added != Detail::ModuleDefinitionLibrary::AddStatus::Duplicate)
    return ReportSubmissionFailure(
        Scope,
        Detail::ModuleConflictDiagnostic(
            Subject,
            "the definition could not be made available (" +
                std::string(
                    Detail::ModuleDefinitionLibrary::AddStatusText(Added)) +
                ")."));

  Transaction.MarkCommitted();
  return RegistrationResult::Success();
}

RegistrationResult State::Impl::SubmitModuleCallback(
    const Detail::ActiveTransactionScope &Scope, const ModuleManifest &Manifest,
    const Detail::ModuleRegistration &Registration,
    std::string_view ParentQualifiedName,
    const Detail::ModuleResolution &Resolution) {
  Detail::RegistrationTransaction &Active = Scope.Active();
  const std::string Subject = Detail::ModuleSubject(Manifest);

  State *Owner = Handle ? Handle->Owner : nullptr;
  if (!Owner)
    return ReportSubmissionFailure(
        Scope, Detail::MalformedMetadataDiagnostic(
                   Subject, "the State has no owner object to attach a module "
                            "builder to."));

  const std::size_t PlannedBeforeCallback = Active.Plan().Size();

  std::shared_ptr<Detail::NamespaceBuilderState> Plan =
      Detail::NamespaceBuilderState::Create(*Owner);
  const std::size_t ScopeNode =
      ParentQualifiedName.empty() ? Detail::NamespaceBuilderState::RootScopeNode
                                  : Plan->StagePath(ParentQualifiedName);
  NamespaceBuilder Builder =
      Detail::NamespaceBuilderState::MakeBuilder(Plan, ScopeNode);

  try {
    Registration.Invoke(Builder);
  } catch (const std::exception &Error) {
    ErrorDiagnostic Diagnostic = Internal("Cannot register " + Subject +
                                          ": the module registration callback "
                                          "failed (" +
                                          std::string(Error.what()) + ").");
    Active.Poison(Diagnostic);
    return RegistrationResult::Failure(std::move(Diagnostic));
  } catch (...) {
    ErrorDiagnostic Diagnostic =
        Internal("Cannot register " + Subject +
                 ": the module registration callback failed for an unknown "
                 "reason.");
    Active.Poison(Diagnostic);
    return RegistrationResult::Failure(std::move(Diagnostic));
  }

  const std::size_t PlannedBefore = Active.Plan().Size();

  if (!Plan->IsCommitted()) {
    Plan->MarkSubmitted();
    if (auto Result = SubmitPlanDeclarations(Scope, Plan->Pending(),
                                             Plan->StagedFailure());
        !Result.IsSuccess())
      return Result;
  }

  Detail::ModuleContribution Contribution;
  for (std::size_t Index = PlannedBefore; Index < Active.Plan().Size();
       ++Index) {
    const Detail::DescriptorPlanEntry *Planned = Active.Plan().At(Index);
    if (!Planned)
      continue;
    if (Planned->Category == Detail::PlanEntryKind::Scope &&
        Planned->Symbol.Kind == SymbolKind::Namespace)
      Contribution.Namespaces.push_back(Planned->Symbol.QualifiedName);
    if (Planned->TypeFields)
      Contribution.Types.push_back(Planned->TypeFields->Name);
  }

  for (std::size_t Index = PlannedBeforeCallback; Index < Active.Plan().Size();
       ++Index) {
    Detail::DescriptorPlanEntry *Planned = Active.Plan().At(Index);
    if (Planned && Planned->ModuleIdentity.empty())
      Planned->ModuleIdentity = Manifest.Identity();
  }

  Detail::DescriptorPlanEntry Candidate =
      Detail::MakeModulePlanEntry(Manifest, Resolution, Contribution);

  Candidate.ModuleIdentity = Manifest.Identity();
  if (!Candidate.IsValid())
    return ReportSubmissionFailure(
        Scope, Detail::MalformedMetadataDiagnostic(
                   Subject, "the canonical module descriptor is incomplete."));

  if (const auto Existing =
          Active.Symbols().Find(Candidate.Symbol.QualifiedName)) {
    if (Existing->Category == Detail::PlanEntryKind::Module)
      return ReportSubmissionFailure(Scope,
                                     Detail::DuplicateNameDiagnostic(Subject));
    return ReportSubmissionFailure(
        Scope, Detail::IncompatibleCategoryDiagnostic(
                   Subject, Detail::PlanEntryKindText(Existing->Category),
                   Existing->IsPending));
  }

  static_cast<void>(Active.Append(std::move(Candidate)));

  PendingModules.push_back(Manifest);
  Detail::ModuleDefinition Definition;
  Definition.Manifest = Manifest;
  Definition.Registration = Registration;
  PendingDefinitions.push_back(std::move(Definition));

  return PrepareSubmittedDeclarations(Scope, Subject);
}

RegistrationResult
State::Impl::SubmitModuleLoad(const Detail::ActiveTransactionScope &Scope,
                              const Detail::StagedModule &Request) {
  const ModuleManifest &Manifest = Request.Manifest;
  const std::string Subject = Detail::ModuleSubject(Manifest);

  if (!Manifest.IsValid())
    return ReportSubmissionFailure(
        Scope,
        Detail::MalformedMetadataDiagnostic(
            Subject,
            "the manifest is not valid (" +
                std::string(ModuleManifestStatusText(Manifest.Status())) +
                ")."));
  if (!Request.Registration.IsValid())
    return ReportSubmissionFailure(
        Scope,
        Detail::MalformedMetadataDiagnostic(
            Subject, "the module carries no scoped registration callback."));

  const Detail::ModuleLoadDecision Decision = Modules.ClassifyLoad(Manifest);
  if (Decision.Status == Detail::ModuleLoadStatus::AlreadyLoaded)
    return RegistrationResult::Success();
  if (!Decision.IsSuccess())
    return ReportSubmissionFailure(
        Scope, Detail::ModuleConflictDiagnostic(Subject, Decision.Message()));

  if (const ModuleManifest *Planned = FindPendingModule(Manifest.Identity())) {
    if (*Planned == Manifest)
      return RegistrationResult::Success();
    return ReportSubmissionFailure(
        Scope,
        Detail::ModuleConflictDiagnostic(
            Subject, "this attempt already loads '" + Planned->Key() +
                         "', so a different definition of the same identity "
                         "conflicts."));
  }

  Detail::ModuleCatalog Candidate = CandidateModuleCatalog();
  const Detail::ModuleCatalog::AddStatus Added = Candidate.Add(Manifest);
  if (Added == Detail::ModuleCatalog::AddStatus::ConflictingDefinition)
    return ReportSubmissionFailure(
        Scope, Detail::ModuleConflictDiagnostic(
                   Subject, "a different definition of this identity and "
                            "version is already available."));

  const Detail::ModuleResolution Resolution = Detail::ResolveModuleGraph(
      Candidate, Manifest.Identity(), Manifest.Version(), Modules.Pins());
  if (!Resolution.IsResolved())
    return ReportSubmissionFailure(Scope, Detail::ModuleResolutionDiagnostic(
                                              Subject, Resolution.Diagnostic));

  for (const std::string &Key : Resolution.LoadOrder) {
    const Detail::ModuleSelection *Selected = nullptr;
    for (const Detail::ModuleSelection &Selection : Resolution.Selections) {
      if (Selection.Key() == Key) {
        Selected = &Selection;
        break;
      }
    }
    if (!Selected)
      continue;

    const bool IsRequested = Selected->Identity == Manifest.Identity();
    if (!IsRequested && Modules.IsLoaded(Selected->Identity))
      continue;
    if (!IsRequested && FindPendingModule(Selected->Identity) != nullptr)
      continue;

    const ModuleManifest *Selection = &Manifest;
    const Detail::ModuleRegistration *Registration = &Request.Registration;
    if (!IsRequested) {
      const Detail::ModuleDefinition *Definition =
          FindModuleDefinition(Selected->Identity, Selected->Version);
      if (!Definition)
        return ReportSubmissionFailure(
            Scope, Detail::MalformedMetadataDiagnostic(
                       Detail::ModuleSubject(Selected->Identity,
                                             Selected->Version.ToString()),
                       "no scoped registration callback is available for this "
                       "selected dependency."));
      Selection = &Definition->Manifest;
      Registration = &Definition->Registration;
    }

    const std::string_view ParentScope =
        IsRequested ? std::string_view(Request.ParentQualifiedName)
                    : std::string_view();
    if (auto Result = SubmitModuleCallback(Scope, *Selection, *Registration,
                                           ParentScope, Resolution);
        !Result.IsSuccess())
      return Result;
  }

  return RegistrationResult::Success();
}

RegistrationResult
State::Impl::RegisterModuleGraph(ModuleManifest Manifest,
                                 Detail::ModuleRegistration Registration) {
  const std::string Subject = Detail::ModuleSubject(Manifest);
  if (!IsOwnerThread())
    return RegistrationResult::Failure(
        Detail::ForeignThreadDiagnostic(Subject));

  Detail::RegistrationTransaction Transaction(CaptureTransactionEntry());
  const Detail::ActiveTransactionScope Scope(ActiveTransaction, Transaction);

  if (auto Diagnostic =
          Detail::ValidateTransactionEntry(Transaction.Entry(), Subject))
    return ReportSubmissionFailure(Scope, std::move(*Diagnostic));

  Detail::StagedModule Request;
  Request.Manifest = std::move(Manifest);
  Request.Registration = std::move(Registration);

  if (auto Result = SubmitModuleLoad(Scope, Request); !Result.IsSuccess()) {
    AbandonAttempt(Scope);
    return Result;
  }

  if (!Scope.IsOuter())
    return RegistrationResult::Success();

  if (Transaction.Plan().IsEmpty()) {
    Transaction.MarkCommitted();
    PublishPendingModules();
    return RegistrationResult::Success();
  }

  return CompleteOutermostTransaction(Scope, nullptr);
}

void State::Impl::RecordPublishedNamespaces(
    const Detail::RegistrationTransaction &Active) {
  for (const Detail::DescriptorPlanEntry &Entry :
       Active.Plan().PlannedEntries()) {
    if (Entry.Category != Detail::PlanEntryKind::Scope ||
        Entry.Symbol.Kind != SymbolKind::Namespace)
      continue;

    const Detail::NamespaceTableInstallation Retained =
        VirtualMachine.RetainNamespaceTable(Entry.VmPath);
    if (!Retained.IsInstalled())
      continue;

    Detail::NamespaceOwnership Ownership;
    Ownership.Identity = Lifecycle.Identity();
    Ownership.Scope = Entry.Identity;
    Ownership.QualifiedName = Entry.Symbol.QualifiedName;
    Ownership.LifecycleGeneration = Lifecycle.Generation();
    Ownership.Table = Retained.Table;
    Ownership.Reference = Retained.Reference;
    Namespaces.Record(std::move(Ownership));
  }
}

Detail::RelationshipCandidate
State::Impl::BuildRelationshipCandidate(const Detail::BuilderPlan &Plan) const {
  Detail::RelationshipCandidate Candidate;

  for (const Detail::RegisteredClass &Committed : Classes.Registered()) {
    Detail::RelationshipClass Declared;
    Declared.Type = Committed.Type;
    Declared.Key = Committed.Key;
    Declared.QualifiedName = Committed.QualifiedName;
    for (const Detail::RegisteredClassDeclaration &Member :
         Committed.Declarations)
      Declared.MemberNames.push_back(Member.Segment);
    Candidate.AddClass(std::move(Declared));
  }

  for (const Detail::StagedClass &Staged : Plan.Classes) {
    Detail::RelationshipClass Declared;
    Declared.Type = Detail::ClassTypeIdentityOf(Staged.Key);
    Declared.Key = Staged.Key;
    Declared.QualifiedName = Staged.QualifiedName;
    Declared.IsPending = true;
    for (const Detail::StagedMember &Member : Staged.Members)
      Declared.MemberNames.push_back(Member.Segment);
    for (const Detail::StagedMethod &Member : Staged.Methods)
      Declared.MemberNames.push_back(Member.Segment);
    Candidate.AddClass(std::move(Declared));
  }

  const Detail::ClassRelationships &Published = Classes.Relationships();
  for (const Detail::ClassBaseEdge &Edge : Published.BaseEdges()) {
    const Detail::RegisteredClass *Derived = Classes.Find(Edge.Derived);
    const Detail::RegisteredClass *Base = Classes.Find(Edge.Base);
    if (!Derived || !Base)
      continue;
    Detail::RelationshipBase Committed;
    Committed.Derived = Derived->Type;
    Committed.DerivedName = Derived->QualifiedName;
    Committed.Base = Base->Key;
    Candidate.AddBase(std::move(Committed));
  }
  for (const Detail::ClassCastEdge &Edge : Published.CastEdges()) {
    const Detail::RegisteredClass *Target = Classes.Find(Edge.Target);
    const Detail::RegisteredClass *Source = Classes.Find(Edge.Source);
    if (!Target || !Source)
      continue;
    Detail::RelationshipCast Committed;
    Committed.Target = Target->Type;
    Committed.TargetName = Target->QualifiedName;
    Committed.Source = Source->Key;
    Candidate.AddCast(std::move(Committed));
  }

  for (const Detail::StagedClass &Staged : Plan.Classes) {
    const TypeId Derived = Detail::ClassTypeIdentityOf(Staged.Key);
    for (const Detail::BaseRequest &Edge : Staged.Relationships.Bases)
      Candidate.AddBase(
          Detail::MakeCandidateBase(Derived, Staged.QualifiedName, Edge));
    for (const Detail::CastRequest &Edge : Staged.Relationships.Casts)
      Candidate.AddCast(
          Detail::MakeCandidateCast(Derived, Staged.QualifiedName, Edge));
  }
  return Candidate;
}

void State::Impl::RecordPublishedRelationships(
    const Detail::RegistrationTransaction &Active) {
  Detail::ClassRelationships &Graph = Classes.RelationshipsForUpdate();
  bool Recorded = false;

  for (const std::size_t Index : Active.Plan().CanonicalOrder()) {
    const Detail::DescriptorPlanEntry *Entry = Active.Plan().At(Index);
    if (!Entry || Entry->Category != Detail::PlanEntryKind::ClassSymbol)
      continue;

    const Detail::RegisteredClass *Derived =
        Classes.FindBySymbol(Entry->Identity);
    if (!Derived)
      continue;

    Graph.RecordNode(Derived->Type, Derived->Metatable, Derived->QualifiedName);
    if (!Entry->Relationships)
      continue;

    for (const Detail::BaseRequest &Declared : Entry->Relationships->Bases) {
      const Detail::RegisteredClass *Base =
          Classes.Find(Detail::ClassTypeIdentityOf(Declared.Base));
      if (!Base)
        continue;
      Graph.RecordNode(Base->Type, Base->Metatable, Base->QualifiedName);

      Detail::ClassBaseEdge Edge;
      Edge.Derived = Derived->Type;
      Edge.Base = Base->Type;
      Edge.Upcast = Declared.Upcast;
      Graph.RecordBase(std::move(Edge));
      Recorded = true;
    }

    for (const Detail::CastRequest &Declared : Entry->Relationships->Casts) {
      const Detail::RegisteredClass *Source =
          Classes.Find(Detail::ClassTypeIdentityOf(Declared.Source));
      if (!Source)
        continue;
      Graph.RecordNode(Source->Type, Source->Metatable, Source->QualifiedName);

      Detail::ClassCastEdge Edge;
      Edge.Source = Source->Type;
      Edge.Target = Derived->Type;
      Edge.Policy = Declared.Policy;
      Edge.UsesRuntimeTypeAssistance = Declared.UsesRuntimeTypeAssistance;
      Edge.Compatible = Declared.Compatible;
      Edge.Downcast = Declared.Downcast;
      Graph.RecordCast(std::move(Edge));
      Recorded = true;
    }
  }

  if (Recorded)
    Graph.Rebuild();
}

void State::Impl::RecordPublishedClasses(
    const Detail::RegistrationTransaction &Active) {
  for (const std::size_t Index : Active.Plan().CanonicalOrder()) {
    const Detail::DescriptorPlanEntry *Entry = Active.Plan().At(Index);
    if (!Entry)
      continue;

    if (Entry->Category == Detail::PlanEntryKind::ClassMember) {
      if (!Entry->ClassMember || !Entry->Record)
        continue;
      Detail::RegisteredClass *Owner =
          Classes.FindForUpdate(Entry->Symbol.Parent);
      if (!Owner)
        continue;

      Detail::RegisteredClassDeclaration Declared;
      Declared.Segment = Entry->Record->Name;
      Declared.Kind = Entry->ClassMember->Kind;
      Declared.Declaration = Entry->Identity;
      Owner->Declarations.push_back(std::move(Declared));

      Detail::RegisteredMember Member;
      Member.Member = Entry->Identity;
      Member.Segment = Entry->Record->Name;
      Member.QualifiedName = Entry->Record->QualifiedName;
      Member.ClassName = Owner->QualifiedName;
      Member.Kind = Entry->ClassMember->Kind;
      Member.Access = Entry->ClassMember->Access;
      Member.Evaluation = Entry->ClassMember->Evaluation;
      Member.Ownership = Entry->ClassMember->Ownership;
      Member.ValueType = Entry->ClassMember->ValueType;
      Member.ValueDescriptor = Entry->ClassMember->ValueDescriptor;
      Member.ReadRequiresMutableReceiver =
          Entry->ClassMember->ReadRequiresMutableReceiver;
      Member.Read = Entry->ClassMember->Read;
      Member.Write = Entry->ClassMember->Write;
      Member.Change = Entry->ClassMember->Change;
      Member.ConvertedRead = Entry->ClassMember->ConvertedRead;
      Member.ConvertedWrite = Entry->ClassMember->ConvertedWrite;
      Member.InstanceRead = Entry->ClassMember->InstanceRead;
      Owner->Members.push_back(std::move(Member));
      continue;
    }

    if (Entry->Category == Detail::PlanEntryKind::Function && Entry->Record &&
        (Entry->Symbol.Kind == SymbolKind::Method ||
         Entry->Symbol.Kind == SymbolKind::StaticMethod)) {
      Detail::RegisteredClass *Owner =
          Classes.FindForUpdate(Entry->Symbol.Parent);
      if (!Owner)
        continue;
      Detail::RegisteredClassDeclaration Declared;
      Declared.Segment = Entry->Record->Name;
      Declared.Kind = Entry->Symbol.Kind;
      Declared.Declaration = Entry->Identity;
      Owner->Declarations.push_back(std::move(Declared));
      continue;
    }

    if (Entry->Category == Detail::PlanEntryKind::Function &&
        Entry->OperatorFields) {
      Detail::RegisteredClass *Owner =
          Classes.FindForUpdate(Entry->Symbol.Parent);
      if (!Owner)
        continue;
      Detail::RegisteredOperator Published;
      Published.Selected = Entry->OperatorFields->Selected;
      Published.Segment = Entry->OperatorFields->Segment;
      Published.QualifiedName = Entry->Symbol.QualifiedName;
      Published.Symbol = Entry->Identity;
      if (Owner->FindOperator(Published.Selected) == nullptr)
        Owner->Operators.push_back(std::move(Published));
      continue;
    }

    if (Entry->Category != Detail::PlanEntryKind::ClassSymbol)
      continue;

    Detail::RegisteredClass Registered;
    Registered.Origin = Lifecycle.Identity();
    Registered.ClassSymbol = Entry->Identity;
    Registered.Type = Entry->TypeFields ? Entry->TypeFields->Id : TypeId();
    if (Entry->Symbol.AssociatedType)
      Registered.Key = Entry->Symbol.AssociatedType->Key();
    Registered.QualifiedName = Entry->Symbol.QualifiedName;
    Registered.LifecycleGeneration = Lifecycle.Generation();

    if (Entry->ClassStorage)
      Registered.Policy = *Entry->ClassStorage;

    Registered.Metatable = Classes.AllocateMetatableIdentity();
    Classes.Record(std::move(Registered));

    if (IsReady())
      static_cast<void>(VirtualMachine.PublishUserdataContexts(
          UserdataAccess(), UserdataExposure()));
  }
}

RegistrationResult
State::Impl::RegisterErased(std::string_view GlobalName,
                            ErasedCallableDescriptor &&Descriptor) {
  if (!IsOwnerThread())
    return RegistrationResult::Failure(
        Detail::ForeignThreadDiagnostic(Detail::GlobalSubject(GlobalName)));

  Detail::RegistrationTransaction Transaction(CaptureTransactionEntry());
  const Detail::ActiveTransactionScope Scope(ActiveTransaction, Transaction);

  Detail::PreparedSubmission Prepared;
  if (auto Result = SubmitFunctionDeclaration(Scope, GlobalName,
                                              std::move(Descriptor), Prepared);
      !Result.IsSuccess())
    return Result;

  if (!Scope.IsOuter())
    return RegistrationResult::Success();

  return CompleteOutermostTransaction(Scope, nullptr);
}

void State::Impl::DiscardStagedResources(
    const Detail::RegistrationTransaction &Active) noexcept {
  const std::vector<std::size_t> Order = Active.Plan().CanonicalOrder();
  for (std::size_t Position = Order.size(); Position > 0; --Position) {
    const Detail::DescriptorPlanEntry *Entry =
        Active.Plan().At(Order[Position - 1]);
    if (!Entry || Entry->Category != Detail::PlanEntryKind::Function)
      continue;
    Detail::BindingRecord *Record = Bindings.Find(Entry->VmPath);
    if (Record && !Record->IsCommitted())
      static_cast<void>(Bindings.Rollback(Entry->VmPath, Record));
  }
}

RegistrationResult State::Impl::CompleteOutermostTransaction(
    const Detail::ActiveTransactionScope &Scope,
    Detail::PublicationObservation *Observed) {
  Detail::RegistrationTransaction &Active = Scope.Active();
  const std::string Subject = TransactionSubject(Active);

  if (!Active.CanPublish()) {
    DiscardStagedResources(Active);
    DiscardPendingModules();
    Active.MarkRolledBack();
    if (Active.Failure())
      return RegistrationResult::Failure(*Active.Failure());
    return RegistrationResult::Failure(
        Internal("Cannot register " + Subject +
                 ": the transaction could not publish its declarations."));
  }

  try {
    Detail::PreparedGenerations Candidate;
    const Detail::PreparationStatus Preparation =
        Faults.Consume(Detail::StateFaultPoint::TransactionPublication)
            ? Detail::PreparationStatus::AllocationFailure
            : Detail::PrepareGenerations(Active, Reflection, Candidate);
    if (Observed)
      Observed->Preparation = Preparation;
    if (Preparation != Detail::PreparationStatus::Prepared) {
      DiscardStagedResources(Active);
      DiscardPendingModules();
      return ReportSubmissionFailure(
          Scope, PreparationDiagnostic(Subject, Preparation,
                                       Candidate.ReflectionStatus,
                                       Candidate.TypeStatus));
    }

    Detail::InstallationJournal Journal(VirtualMachine, Bindings, Faults,
                                        Active.EntryStackDepth());
    const std::shared_ptr<const Detail::TypeGeneration> CandidateTypes =
        Candidate.Types ? Candidate.Types
                        : Detail::TypeGeneration::Foundation();
    const Detail::InstallationOutcome Installation =
        Detail::InstallPlannedDeclarations(Active, *CandidateTypes, Bindings,
                                           VirtualMachine, Faults, Journal);
    if (Observed)
      Observed->Installation = Installation.Status;

    if (!Installation.IsInstalled()) {
      Journal.Undo();
      DiscardStagedResources(Active);
      DiscardPendingModules();
      const bool Restored =
          Journal.RestoredEveryEntry() && Journal.RestoredEntryStackDepth();
      if (Observed) {
        Detail::ObserveJournal(Journal, *Observed);
        Observed->StackDepthAfter =
            IsReady() ? VirtualMachine.StackDepth() : Journal.EntryStackDepth();
      }
      return ReportSubmissionFailure(
          Scope, InstallationDiagnostic(Installation, Restored));
    }

    const Detail::ConsistencyStatus Consistency =
        Detail::CheckPublicationConsistency(Active, Candidate, Reflection,
                                            Bindings, VirtualMachine, Faults);
    if (Observed)
      Observed->Consistency = Consistency;
    if (Consistency != Detail::ConsistencyStatus::Consistent) {
      Journal.Undo();
      DiscardStagedResources(Active);
      DiscardPendingModules();
      const bool Restored =
          Journal.RestoredEveryEntry() && Journal.RestoredEntryStackDepth();
      if (Observed) {
        Detail::ObserveJournal(Journal, *Observed);
        Observed->StackDepthAfter =
            IsReady() ? VirtualMachine.StackDepth() : Journal.EntryStackDepth();
      }
      return ReportSubmissionFailure(
          Scope, ConsistencyDiagnostic(Subject, Consistency, Restored));
    }

    if (Candidate.ReflectionAdvances &&
        !Reflection.Publish(Candidate.Reflection)) {
      Journal.Undo();
      DiscardStagedResources(Active);
      DiscardPendingModules();
      return ReportSubmissionFailure(
          Scope,
          ConsistencyDiagnostic(
              Subject, Detail::ConsistencyStatus::ReflectionContentMismatch,
              Journal.RestoredEveryEntry() &&
                  Journal.RestoredEntryStackDepth()));
    }

    Generations = Candidate.Candidate;

    Bindings.PublishTypes(Generations->Types());
    for (const Detail::DescriptorPlanEntry &Entry :
         Active.Plan().PlannedEntries()) {
      if (Entry.Category != Detail::PlanEntryKind::Function)
        continue;
      if (Detail::BindingRecord *Record = Bindings.Find(Entry.VmPath))
        Bindings.Commit(*Record);
    }

    RecordPublishedNamespaces(Active);

    RecordPublishedClasses(Active);

    RecordPublishedRelationships(Active);

    Journal.Commit();
    Active.MarkCommitted();
    PublishPendingModules();

    if (Observed) {
      Detail::ObserveJournal(Journal, *Observed);
      Observed->IsPublished = true;
      Observed->PublishedGeneration = Generations->Generation();
      Observed->PublishedSymbols = Generations->Symbols().Size();
      Observed->ReflectionAdvanced = Candidate.ReflectionAdvances;
      Observed->PublishedReflectionGeneration = Reflection.Generation();
      Observed->StackDepthAfter =
          IsReady() ? VirtualMachine.StackDepth() : Journal.EntryStackDepth();
    }
    return RegistrationResult::Success();
  } catch (const std::exception &Error) {
    DiscardStagedResources(Active);
    DiscardPendingModules();
    return ReportSubmissionFailure(Scope,
                                   Internal("Cannot register " + Subject +
                                            ": publication failed (" +
                                            std::string(Error.what()) + ")."));
  } catch (...) {
    DiscardStagedResources(Active);
    DiscardPendingModules();
    return ReportSubmissionFailure(
        Scope, Internal("Cannot register " + Subject +
                        ": publication failed for an unknown reason."));
  }
}

Detail::JoinedSubmissionReport State::Impl::SubmitJoinedFunctionDeclarations(
    std::vector<Detail::JoinedFunctionDeclaration> Declarations,
    bool IgnoreNestedFailures, bool PublishWhenComplete) {
  Detail::JoinedSubmissionReport Report;
  Report.Submitted = Declarations.size();

  Detail::RegistrationTransaction Transaction(CaptureTransactionEntry());
  const Detail::ActiveTransactionScope Outer(ActiveTransaction, Transaction);
  Detail::RegistrationTransaction &Active = Outer.Active();
  Report.EntryStackDepth = Active.EntryStackDepth();

  for (Detail::JoinedFunctionDeclaration &Declaration : Declarations) {
    const auto Result =
        RegisterErased(Declaration.Name, std::move(Declaration.Callable));
    if (Result.IsSuccess())
      continue;

    if (!IgnoreNestedFailures)
      break;
  }

  std::vector<Detail::BindingRecord *> Staged;
  std::vector<std::string> StagedNames;
  for (const Detail::DescriptorPlanEntry &Entry :
       Active.Plan().PlannedEntries()) {
    Detail::BindingRecord *Record = Bindings.Find(Entry.VmPath);
    if (Record && !Record->IsCommitted()) {
      Staged.push_back(Record);
      StagedNames.push_back(Entry.VmPath);
    }
  }

  Report.Planned = Active.Plan().Size();
  Report.Prepared = Staged.size();
  Report.JoinedSubmissions = Active.JoinedSubmissionCount();
  Report.NestedFailures = Active.NestedFailureCount();
  Report.OuterCouldPublish = Active.CanPublish();
  Report.Failure = Active.Failure();

  const Detail::SymbolView View = Active.Symbols();
  Report.CommittedSymbolsInView = View.CommittedCount();
  Report.PendingSymbolsInView = View.PendingCount();

  Report.PublishedGenerationWhileOpen = CurrentGenerations()->Generation();
  Report.PublishedSymbolsWhileOpen = CurrentGenerations()->Symbols().Size();
  Report.ReflectionGenerationWhileOpen = Reflection.Generation();
  Report.StagedBindingsWhileOpen = Bindings.PendingCount();
  Report.StackDepthWhileOpen =
      IsReady() ? VirtualMachine.StackDepth() : Report.EntryStackDepth;
  for (const std::string &Name : StagedNames) {
    if (VirtualMachine.ObserveInstalledBinding(Name))
      ++Report.VmVisibleDeclarationsWhileOpen;
  }

  Detail::PreparedGenerations Candidate;
  Report.Preparation =
      Detail::PrepareGenerations(Active, Reflection, Candidate);
  if (Candidate.IsPrepared()) {
    Report.CandidateGeneration = Candidate.Candidate->Generation();
    Report.CandidateSymbols = Candidate.Candidate->Symbols().Size();
  }

  if (PublishWhenComplete) {
    Detail::PublicationObservation Observed;
    Observed.EntryStackDepth = Report.EntryStackDepth;
    const auto Result = CompleteOutermostTransaction(Outer, &Observed);
    if (!Result.IsSuccess() && !Report.Failure)
      Report.Failure = *Result.Diagnostic();
    Report.Publication = std::move(Observed);
    Report.Status = Active.Status();
    Report.PublishedGenerationAfter = CurrentGenerations()->Generation();
    Report.PublishedSymbolsAfter = CurrentGenerations()->Symbols().Size();
    Report.StagedBindingsAfter = Bindings.PendingCount();
    Report.CommittedBindingsAfter = Bindings.Count();
    for (const std::string &Name : StagedNames) {
      if (VirtualMachine.ObserveInstalledBinding(Name))
        ++Report.VmVisibleDeclarationsAfter;
    }
    return Report;
  }

  for (std::size_t Index = Staged.size(); Index > 0; --Index)
    static_cast<void>(
        Bindings.Rollback(StagedNames[Index - 1], Staged[Index - 1]));
  Active.MarkRolledBack();
  Report.Status = Active.Status();
  Report.PublishedGenerationAfter = CurrentGenerations()->Generation();
  Report.PublishedSymbolsAfter = CurrentGenerations()->Symbols().Size();
  Report.StagedBindingsAfter = Bindings.PendingCount();
  Report.CommittedBindingsAfter = Bindings.Count();
  return Report;
}

Detail::CallbackBoundaryObservation
State::Impl::SubmitJoinedFunctionsThroughCallback(
    std::vector<Detail::JoinedFunctionDeclaration> Declarations,
    std::size_t ThrowAfterSubmissions, bool ThrowStandardException,
    bool PublishWhenComplete) {
  Detail::CallbackBoundaryObservation Observed;
  Observed.Submitted = Declarations.size();

  Detail::RegistrationTransaction Transaction(CaptureTransactionEntry());
  const Detail::ActiveTransactionScope Outer(ActiveTransaction, Transaction);
  Detail::RegistrationTransaction &Active = Outer.Active();
  Observed.EntryStackDepth = Active.EntryStackDepth();

  try {
    for (std::size_t Index = 0; Index < Declarations.size(); ++Index) {
      if (Index == ThrowAfterSubmissions)
        ThrowFromRegistrationCallback(ThrowStandardException);
      static_cast<void>(RegisterErased(
          Declarations[Index].Name, std::move(Declarations[Index].Callable)));
    }
    if (Declarations.size() == ThrowAfterSubmissions)
      ThrowFromRegistrationCallback(ThrowStandardException);
  } catch (const std::exception &Error) {
    Observed.CallbackThrew = true;
    Observed.ExceptionContained = true;
    Observed.ExceptionKind = "standard";
    Active.Poison(Internal("Cannot register the requested symbols: a "
                           "registration callback failed (" +
                           std::string(Error.what()) + ")."));
  } catch (...) {
    Observed.CallbackThrew = true;
    Observed.ExceptionContained = true;
    Observed.ExceptionKind = "unknown";
    Active.Poison(Internal("Cannot register the requested symbols: a "
                           "registration callback failed for an unknown "
                           "reason."));
  }

  Observed.PlannedWhileOpen = Active.Plan().Size();
  Observed.PendingSymbolsInView = Active.Symbols().PendingCount();
  Observed.NestedFailures = Active.NestedFailureCount();
  Observed.CouldPublishWhileOpen = Active.CanPublish();

  const std::shared_ptr<const Detail::GenerationSet> Open =
      CurrentGenerations();
  Observed.GenerationWhileOpen = Open->Generation();
  Observed.GenerationSymbolsWhileOpen = Open->Symbols().Size();
  Observed.SnapshotWhileOpen = CaptureReflection();
  Observed.SnapshotGenerationWhileOpen =
      Observed.SnapshotWhileOpen.Generation();
  Observed.SnapshotSymbolsWhileOpen = Observed.SnapshotWhileOpen.Size();
  Observed.StagedWhileOpen = Bindings.PendingCount();
  Observed.CommittedWhileOpen = Bindings.Count() - Observed.StagedWhileOpen;
  Observed.StackDepthWhileOpen =
      IsReady() ? VirtualMachine.StackDepth() : Observed.EntryStackDepth;

  std::vector<std::string> PlannedPaths;
  for (const Detail::DescriptorPlanEntry &Entry :
       Active.Plan().PlannedEntries()) {
    PlannedPaths.push_back(Entry.VmPath);
    Observed.VmPathKindsWhileOpen.push_back(
        ObserveGlobalValueKind(VirtualMachine, IsReady(), Entry.VmPath));
    if (VirtualMachine.ObserveInstalledBinding(Entry.VmPath))
      ++Observed.DispatchVisibleWhileOpen;
  }

  std::thread Reader([this, &Observed] {
    const ReflectionSnapshot Foreign = CaptureReflection();
    Observed.ForeignSnapshotGenerationWhileOpen = Foreign.Generation();
    Observed.ForeignSnapshotSymbolsWhileOpen = Foreign.Size();
  });
  Reader.join();

  if (PublishWhenComplete) {
    Detail::PublicationObservation Publication;
    Publication.EntryStackDepth = Observed.EntryStackDepth;
    const auto Result = CompleteOutermostTransaction(Outer, &Publication);
    Observed.Published = Publication.IsPublished;
    Observed.JournalledEntries = Publication.JournalledEntries;
    Observed.InstalledPaths = Publication.InstalledPaths;
    Observed.RestoredEveryEntry = Publication.RestoredEveryEntry;
    Observed.RestoredEntryStackDepth = Publication.RestoredEntryStackDepth;
    if (!Result.IsSuccess() && Result.Diagnostic())
      Observed.Failure = *Result.Diagnostic();
  } else {
    DiscardStagedResources(Active);
    Active.MarkRolledBack();
    Observed.Failure = Active.Failure();
  }

  Observed.Status = Active.Status();
  const std::shared_ptr<const Detail::GenerationSet> After =
      CurrentGenerations();
  Observed.GenerationAfter = After->Generation();
  Observed.GenerationSymbolsAfter = After->Symbols().Size();
  Observed.StagedAfter = Bindings.PendingCount();
  Observed.CommittedAfter = Bindings.Count() - Observed.StagedAfter;
  for (const std::string &Path : PlannedPaths) {
    Observed.VmPathKindsAfter.push_back(
        ObserveGlobalValueKind(VirtualMachine, IsReady(), Path));
    if (VirtualMachine.ObserveInstalledBinding(Path))
      ++Observed.DispatchVisibleAfter;
  }
  Observed.StackDepthAfter =
      IsReady() ? VirtualMachine.StackDepth() : Observed.EntryStackDepth;
  return Observed;
}

RegistrationResult State::Impl::Freeze() {
  if (!Lifecycle.IsOwnerThread())
    return RegistrationResult::Failure(ErrorCategory::StateNotReady,
                                       "Cannot freeze State: freeze is only "
                                       "allowed on the State's owner thread.");
  if (!IsReady())
    return RegistrationResult::Failure(
        ErrorCategory::StateNotReady,
        "Cannot freeze State: State is not ready.");
  if (Lifecycle.IsFrozen())
    return RegistrationResult::Failure(
        ErrorCategory::StateNotReady,
        "Cannot freeze State: State is already frozen.");
  if (ActiveTransaction != nullptr)
    return RegistrationResult::Failure(
        ErrorCategory::Internal,
        "Cannot freeze State: a registration transaction is active.");

  const std::shared_ptr<const Detail::GenerationSet> Captured = Generations;
  if (!Captured || Reflection.Capture() != Captured->Reflection())
    return RegistrationResult::Failure(
        ErrorCategory::Internal, "Cannot freeze State: committed reflection "
                                 "and generation storage are inconsistent.");

  std::shared_ptr<const Detail::FreezeCacheStorage> Prepared;
  const Detail::FreezePreparationStatus Status =
      Faults.Consume(Detail::StateFaultPoint::FreezePreparation)
          ? Detail::FreezePreparationStatus::AllocationFailure
          : Detail::FreezeCacheStorage::Prepare(
                *Captured, Lifecycle.Identity(), Lifecycle.Generation(),
                Bindings, Namespaces, Classes, Modules, Prepared);
  if (Status != Detail::FreezePreparationStatus::Prepared || !Prepared) {
    return RegistrationResult::Failure(
        ErrorCategory::Internal,
        "Cannot freeze State: immutable cache preparation failed (" +
            std::string(Detail::FreezePreparationStatusText(Status)) + ").");
  }

  for (const Detail::FrozenNamespaceEntry &Entry : Prepared->NamespaceCache()) {
    const Detail::NamespaceOwnership *Owned =
        Namespaces.Find(Entry.QualifiedName);
    const Detail::VmPathObservation Observed =
        VirtualMachine.ObserveVmPath(Entry.QualifiedName);
    if (!Owned || !Detail::NamespaceOwnershipTable::Matches(
                      *Owned, Lifecycle.Identity(), Entry.QualifiedName,
                      Entry.Scope, Observed.Table)) {
      return RegistrationResult::Failure(
          ErrorCategory::Internal,
          "Cannot freeze State: immutable cache preparation failed "
          "(inconsistent_namespace).");
    }
  }

  if (Captured != Generations)
    return RegistrationResult::Failure(
        ErrorCategory::Internal, "Cannot freeze State: the committed "
                                 "generation changed during preparation.");
  FrozenCaches = std::move(Prepared);
  Lifecycle.Freeze();
  return RegistrationResult::Success();
}

RegistrationResult State::Impl::InstallProfilingHook(ProfilingHook Hook) {
  if (!Lifecycle.IsOwnerThread())
    return RegistrationResult::Failure(
        ErrorCategory::StateNotReady,
        "Cannot install profiling hook: this operation is only allowed on "
        "the State's owner thread.");
  if (!IsReady())
    return RegistrationResult::Failure(
        ErrorCategory::StateNotReady,
        "Cannot install profiling hook: State is not ready.");
  if (!Hook)
    return RegistrationResult::Failure(
        ErrorCategory::Internal,
        "Cannot install profiling hook: the supplied hook is empty.");
  ProfilingHooks.Install(std::move(Hook));
  return RegistrationResult::Success();
}

RegistrationResult State::Impl::ClearProfilingHook() {
  if (!Lifecycle.IsOwnerThread())
    return RegistrationResult::Failure(
        ErrorCategory::StateNotReady,
        "Cannot clear profiling hook: this operation is only allowed on the "
        "State's owner thread.");
  if (!IsReady())
    return RegistrationResult::Failure(
        ErrorCategory::StateNotReady,
        "Cannot clear profiling hook: State is not ready.");
  ProfilingHooks.Clear();
  return RegistrationResult::Success();
}

ExecutionResult State::Impl::Execute(std::string_view Source) {
  if (!IsOwnerThread()) {
    return ExecutionResult::Failure(
        ErrorCategory::StateNotReady,
        "State not ready: source execution is only allowed on the State's "
        "owner thread.");
  }
  if (!IsReady()) {
    return ExecutionResult::Failure(
        ErrorCategory::StateNotReady,
        "State not ready: source execution requires a ready State.");
  }
  AsyncCalls.BindOrigin(Lifecycle.Identity(), Lifecycle.Generation());
  return VirtualMachine.ExecuteSource(Source, Faults, &AsyncCalls);
}

ReflectionSnapshot State::Impl::CaptureReflection() const {
  return Reflection.Snapshot();
}

Detail::LifecycleSubject State::Impl::DescribeLifecycleSubject() const {
  const std::shared_ptr<const Detail::ReflectionStorage> Captured =
      Reflection.Capture();

  Detail::LifecycleSubjectSources Sources;
  Sources.Reflection = Captured.get();
  Sources.Modules = &Modules;
  Sources.Dispatch = &Bindings.Dispatch();
  Sources.Userdata = &Userdata;
  Sources.Classes = &Classes;
  Sources.LazyValues = &LazyValues;
  Sources.FrozenCaches = FrozenCaches.get();
  Sources.Identities = &Identities;
  Sources.Frozen = Lifecycle.IsFrozen();

  Sources.DynamicLifecycleEnabled = false;
  return Detail::DescribeLifecycleSubject(Sources);
}

Detail::LifecycleAnalysis State::Impl::AnalyzeLifecycleRequest(
    const Detail::LifecycleRequest &Request) const {
  return Detail::AnalyzeLifecycleRequest(Request, DescribeLifecycleSubject());
}

Detail::LifecycleAttemptObservation
State::Impl::PrepareLifecycleAttempt(const Detail::LifecycleAttempt &Attempt) {
  Detail::LifecycleAttemptObservation Observed;

  Detail::RegistrationTransaction Transaction(CaptureTransactionEntry());
  const Detail::ActiveTransactionScope Outer(ActiveTransaction, Transaction);
  Detail::RegistrationTransaction &Active = Outer.Active();

  Detail::LifecycleAnalysis Analysis;
  Analysis.Operation = Attempt.Plan.Operation;
  Analysis.Identity = Attempt.Plan.Identity;
  Analysis.Blockers = Attempt.Blockers;

  Detail::LifecycleStagingSources Sources;
  Sources.Machine = &VirtualMachine;
  Sources.Bindings = &Bindings;
  Sources.Faults = &Faults;
  Sources.Modules = &Modules;
  Sources.Caches = FrozenCaches;

  Detail::LifecycleStagingCallback Callback;
  if (Attempt.RunCallback) {
    Callback = [&Attempt]() -> std::optional<ErrorDiagnostic> {
      if (Attempt.CallbackThrows)
        throw std::runtime_error("replacement registration callback");
      if (Attempt.CallbackFails)
        return ErrorDiagnostic::Create(
            ErrorCategory::Internal,
            "The replacement registration callback refused.");
      return std::nullopt;
    };
  }

  const auto PathKind = [this](const std::string &Path) {
    if (!IsReady())
      return std::string("<unavailable>");
    Detail::SavedVmValue Saved;
    if (!VirtualMachine.CaptureVmPath(Path, Saved))
      return std::string("<unavailable>");
    std::string Kind(Detail::VmValueKindText(Saved.Kind));
    VirtualMachine.ReleaseSavedValue(Saved);
    return Kind;
  };

  Detail::PreparedLifecycle Prepared;
  const Detail::LifecycleStageStatus Status = Detail::PrepareLifecycle(
      Active, Attempt.Plan, Analysis, Sources, Callback, Prepared);

  Observed.GenerationWhileStaged = CurrentGenerations()->Generation();
  Observed.SymbolCountWhileStaged = CurrentGenerations()->Symbols().Size();
  Observed.ReflectionGenerationWhileStaged = Reflection.Generation();
  Observed.DispatchGenerationWhileStaged = Bindings.Dispatch().Generation();
  Observed.ModuleCountWhileStaged = Modules.Count();
  Observed.StackDepthWhileStaged =
      IsReady() ? VirtualMachine.StackDepth() : Active.EntryStackDepth();
  for (const std::string &Path : Prepared.Observed().JournalledPathNames)
    Observed.PathKindsWhileStaged.push_back(PathKind(Path));

  Observed.TransactionPoisoned = Active.IsPoisoned();
  if (Active.Failure())
    Observed.TransactionFailure = Active.Failure()->Message();

  Prepared.Rollback();
  Observed.Staging = Prepared.Observed();
  Observed.Staging.Status = Status;

  if (Active.IsOpen())
    Active.MarkRolledBack();

  Observed.GenerationAfter = CurrentGenerations()->Generation();
  Observed.SymbolCountAfter = CurrentGenerations()->Symbols().Size();
  Observed.ReflectionGenerationAfter = Reflection.Generation();
  Observed.DispatchGenerationAfter = Bindings.Dispatch().Generation();
  Observed.ModuleCountAfter = Modules.Count();
  Observed.StackDepthAfter =
      IsReady() ? VirtualMachine.StackDepth() : Active.EntryStackDepth();
  for (const std::string &Path : Observed.Staging.JournalledPathNames)
    Observed.PathKindsAfter.push_back(PathKind(Path));
  Observed.ModuleStillLoaded = Modules.IsLoaded(Attempt.Plan.Identity);
  Observed.SupersededDispatchGenerations =
      Bindings.Dispatch().SupersededGenerationCount();
  Observed.RetainedDispatchGenerations =
      Bindings.Dispatch().RetainedGenerationCount();
  Observed.LifecycleJournalRetainersAfter = Bindings.Dispatch().RetainerCount(
      Detail::DispatchRetainer::LifecycleJournal);
  return Observed;
}

Detail::LifecycleCommitObservation State::Impl::PublishLifecycleAttempt(
    const Detail::LifecycleCommitAttempt &Request) {
  Detail::LifecycleCommitObservation Observed;
  const Detail::LifecycleAttempt &Attempt = Request.Staged;

  const auto ReflectionIdentities = [this]() {
    std::vector<std::string> Identities;
    const std::shared_ptr<const Detail::ReflectionStorage> Captured =
        Reflection.Capture();
    if (!Captured)
      return Identities;
    for (std::size_t Index = 0; Index < Captured->RecordCount(); ++Index) {
      const Detail::ReflectionRecordFields *Record = Captured->RecordAt(Index);
      if (Record == nullptr)
        continue;
      Identities.push_back(Record->QualifiedName + "=" + Record->Id.ToString());
    }
    std::sort(Identities.begin(), Identities.end());
    return Identities;
  };

  const auto PathKind = [this](const std::string &Path) {
    if (!IsReady())
      return std::string("<unavailable>");
    Detail::SavedVmValue Saved;
    if (!VirtualMachine.CaptureVmPath(Path, Saved))
      return std::string("<unavailable>");
    std::string Kind(Detail::VmValueKindText(Saved.Kind));
    VirtualMachine.ReleaseSavedValue(Saved);
    return Kind;
  };

  const auto RunSource = [this](const std::string &Source, bool &Succeeded,
                                std::string &Diagnostic) {
    if (Source.empty())
      return;
    const ExecutionResult Result = Execute(Source);
    Succeeded = Result.IsSuccess();
    if (const ErrorDiagnostic *Failure = Result.Diagnostic())
      Diagnostic = Failure->Message();
  };

  Observed.GenerationBefore = CurrentGenerations()->Generation();
  Observed.SymbolCountBefore = CurrentGenerations()->Symbols().Size();
  Observed.ReflectionGenerationBefore = Reflection.Generation();
  Observed.DispatchGenerationBefore = Bindings.Dispatch().Generation();
  Observed.LifecycleGenerationBefore = Lifecycle.Generation();
  Observed.ModuleCountBefore = Modules.Count();
  Observed.OwnershipRecordsBefore = Userdata.RecordCount();
  Observed.NamespaceOwnershipsBefore = Namespaces.Size();
  Observed.ReflectionIdentitiesBefore = ReflectionIdentities();
  Observed.StackDepthBefore = IsReady() ? VirtualMachine.StackDepth() : 0;
  for (const std::string &Path : Request.ProbedPaths)
    Observed.ProbedPathKindsBefore.push_back(PathKind(Path));

  RunSource(Request.SourceBeforePublication, Observed.SourceBeforeSucceeded,
            Observed.SourceBeforeDiagnostic);

  {
    Detail::RegistrationTransaction Transaction(CaptureTransactionEntry());
    const Detail::ActiveTransactionScope Outer(ActiveTransaction, Transaction);
    Detail::RegistrationTransaction &Active = Outer.Active();

    Detail::LifecycleAnalysis Analysis;
    Analysis.Operation = Attempt.Plan.Operation;
    Analysis.Identity = Attempt.Plan.Identity;
    Analysis.Blockers = Attempt.Blockers;

    Detail::LifecycleStagingSources Sources;
    Sources.Machine = &VirtualMachine;
    Sources.Bindings = &Bindings;
    Sources.Faults = &Faults;
    Sources.Modules = &Modules;
    Sources.Caches = FrozenCaches;

    Detail::LifecycleStagingCallback Callback;
    if (Attempt.RunCallback) {
      Callback = [&Attempt]() -> std::optional<ErrorDiagnostic> {
        if (Attempt.CallbackThrows)
          throw std::runtime_error("replacement registration callback");
        if (Attempt.CallbackFails)
          return ErrorDiagnostic::Create(
              ErrorCategory::Internal,
              "The replacement registration callback refused.");
        return std::nullopt;
      };
    }

    Detail::PreparedLifecycle Prepared;
    const Detail::LifecycleStageStatus Staged = Detail::PrepareLifecycle(
        Active, Attempt.Plan, Analysis, Sources, Callback, Prepared);

    Detail::DispatchRetention Held;
    Detail::DispatchSlotId Probe;
    if (Request.RetainInvocationGeneration) {
      Held = Bindings.Dispatch().Retain(Detail::DispatchRetainer::Invocation);
      for (const std::string &Path : Prepared.RemovedPaths) {
        const std::optional<Detail::DispatchSlotId> Slot =
            Bindings.Dispatch().FindSlot(Path);
        if (!Slot)
          continue;
        Probe = *Slot;
        Observed.RetainedProbe = Path;
        break;
      }
    }

    Detail::LifecyclePlan Plan = Attempt.Plan;
    if (Request.PublishWithoutDynamicLifecycle)
      Plan.DynamicLifecycleEnabled = false;
    if (Request.PublishWithoutStaging)
      Prepared.Rollback();

    Detail::LifecyclePublicationTargets Targets;
    Targets.Machine = &VirtualMachine;
    Targets.Bindings = &Bindings;
    Targets.Faults = &Faults;
    Targets.Modules = &Modules;
    Targets.Reflection = &Reflection;
    Targets.LazyValues = &LazyValues;
    Targets.Identities = &Identities;
    Targets.Generations = &Generations;
    Targets.Caches = &FrozenCaches;

    static_cast<void>(Detail::PublishLifecycle(Active, Plan, Prepared, Targets,
                                               Observed.Publication));

    Observed.Staging = Prepared.Observed();
    Observed.Staging.Status = Staged;

    Observed.TransactionPoisoned = Active.IsPoisoned();
    Observed.TransactionCommitted =
        Active.Status() == Detail::TransactionStatus::Committed;
    if (Active.Failure())
      Observed.TransactionFailure = Active.Failure()->Message();
    if (Active.IsOpen())
      Active.MarkRolledBack();

    Observed.SupersededDispatchGenerations =
        Bindings.Dispatch().SupersededGenerationCount();
    Observed.RetainedDispatchGenerations =
        Bindings.Dispatch().RetainedGenerationCount();

    if (Held.IsHeld()) {
      Observed.RetainedGenerationNumber = Held.GenerationNumber();
      const Detail::DispatchEntry *Entry =
          Probe.IsValid() ? Held.Find(Probe) : nullptr;
      Observed.RetainedGenerationResolvesOldTarget =
          Entry != nullptr && Entry->IsAvailable();
    }
  }

  Observed.ReclaimedAfterRelease = Bindings.Dispatch().ReclaimUnretained();

  RunSource(Request.SourceAfterPublication, Observed.SourceAfterSucceeded,
            Observed.SourceAfterDiagnostic);

  Observed.GenerationAfter = CurrentGenerations()->Generation();
  Observed.SymbolCountAfter = CurrentGenerations()->Symbols().Size();
  Observed.ReflectionGenerationAfter = Reflection.Generation();
  Observed.DispatchGenerationAfter = Bindings.Dispatch().Generation();
  Observed.LifecycleGenerationAfter = Lifecycle.Generation();
  Observed.ModuleCountAfter = Modules.Count();
  Observed.OwnershipRecordsAfter = Userdata.RecordCount();
  Observed.NamespaceOwnershipsAfter = Namespaces.Size();
  Observed.ReflectionIdentitiesAfter = ReflectionIdentities();
  Observed.StackDepthAfter = IsReady() ? VirtualMachine.StackDepth() : 0;
  for (const std::string &Path : Request.ProbedPaths)
    Observed.ProbedPathKindsAfter.push_back(PathKind(Path));

  Observed.ModuleStillLoaded = Modules.IsLoaded(Attempt.Plan.Identity);
  if (const ModuleManifest *Loaded = Modules.Find(Attempt.Plan.Identity))
    Observed.LoadedVersionAfter = Loaded->Version().ToString();

  Observed.LifecycleJournalRetainersAfter = Bindings.Dispatch().RetainerCount(
      Detail::DispatchRetainer::LifecycleJournal);
  return Observed;
}

} // namespace Luna
