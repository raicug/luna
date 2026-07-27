// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/detail/callable_adapter.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/state/state.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/identity/symbol_descriptor.hpp"
#include "state/registration/plan.hpp"
#include "state/registration/submission.hpp"
#include "state/registration/validation.hpp"
#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/transaction/capture.hpp"
#include "state/transaction/generation_set.hpp"
#include "state/transaction/lifecycle.hpp"
#include "state/transaction/preparation.hpp"
#include "state/transaction/transaction.hpp"

#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Luna::Detail::CommittedSymbol;
using Luna::Detail::CommittedSymbolTable;
using Luna::Detail::DescriptorPlan;
using Luna::Detail::DescriptorPlanEntry;
using Luna::Detail::GenerationSet;
using Luna::Detail::JoinedFunctionDeclaration;
using Luna::Detail::LifecyclePhase;
using Luna::Detail::MakeCommittedSymbol;
using Luna::Detail::MakeFunctionPlanEntry;
using Luna::Detail::PlanEntryKind;
using Luna::Detail::PreparationStatus;
using Luna::Detail::PreparedGenerations;
using Luna::Detail::RegistrationPrecedence;
using Luna::Detail::RegistrationTransaction;
using Luna::Detail::RegistrationValidationRequest;
using Luna::Detail::SymbolView;
using Luna::Detail::TransactionCapture;
using Luna::Detail::TransactionStatus;
using Luna::Detail::ValidateRegistration;
using FaultPoint = Luna::Detail::StateFaultPoint;
using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "transaction capture check failed: " << Description << '\n';
}

[[nodiscard]] int AddIntegers(int Left, int Right) { return Left + Right; }

[[nodiscard]] Luna::ErasedCallableDescriptor IntegerAdder() {
  return Luna::Detail::MakeErasedCallableDescriptor(&AddIntegers);
}

[[nodiscard]] bool HasFailure(const Luna::RegistrationResult &Result,
                              Luna::ErrorCategory Category,
                              std::string_view Fragment) {
  return !Result.IsSuccess() && Result.Diagnostic() != nullptr &&
         Result.Diagnostic()->Category() == Category &&
         Result.Diagnostic()->Message().find(Fragment) != std::string::npos;
}

[[nodiscard]] bool HasFailure(const Luna::ExecutionResult &Result,
                              Luna::ErrorCategory Category,
                              std::string_view Fragment) {
  return !Result.IsSuccess() && Result.Diagnostic() != nullptr &&
         Result.Diagnostic()->Category() == Category &&
         Result.Diagnostic()->Message().find(Fragment) != std::string::npos;
}

[[nodiscard]] bool
HasDiagnostic(const std::optional<Luna::ErrorDiagnostic> &Value,
              Luna::ErrorCategory Category, std::string_view Fragment) {
  return Value.has_value() && Value->Category() == Category &&
         Value->Message().find(Fragment) != std::string::npos;
}

[[nodiscard]] TransactionCapture
ReadyCapture(std::shared_ptr<const GenerationSet> Generations = nullptr) {
  TransactionCapture Capture;
  Capture.OwnerThread = std::this_thread::get_id();
  Capture.VirtualMachineIsReady = true;
  Capture.Phase = LifecyclePhase::Ready;
  Capture.Generations =
      Generations ? std::move(Generations) : GenerationSet::Initial();
  return Capture;
}

[[nodiscard]] DescriptorPlanEntry ScopeEntry(std::string QualifiedName) {
  DescriptorPlanEntry Entry;
  Entry.Category = PlanEntryKind::Scope;
  Entry.VmPath = QualifiedName;
  Entry.Symbol = Luna::Detail::MakeScopeSymbol(
      Luna::SymbolKind::Namespace, std::move(QualifiedName), Luna::SymbolId());
  if (const auto Identity =
          Luna::Detail::SymbolIdentityRegistry::ComputeIdentity(Entry.Symbol))
    Entry.Identity = *Identity;
  return Entry;
}

[[nodiscard]] RegistrationValidationRequest
FunctionRequest(std::string_view Name, const DescriptorPlanEntry *Entry,
                RegistrationPrecedence Precedence) {
  RegistrationValidationRequest Request;
  Request.Precedence = Precedence;
  Request.Name = Name;
  Request.Entry = Entry;
  Request.Category = PlanEntryKind::Function;
  Request.HasTarget = Entry && Entry->Callable && Entry->Callable->HasTarget();
  return Request;
}

void CheckEntryCapture() {
  Luna::State Owner;
  const auto RootDepth = Hooks::ObserveRootStackDepth(Owner);
  const auto Capture = Hooks::CaptureTransactionEntryOf(Owner);
  Check(Capture.has_value(), "a ready State captures its transaction entry");
  if (!Capture || !RootDepth)
    return;

  Check(Capture->IsOwnerThread(),
        "the construction thread is the captured owner thread");
  Check(Capture->VirtualMachineIsReady,
        "a ready State captures a ready virtual machine");
  Check(Capture->Phase == LifecyclePhase::Ready && !Capture->IsFrozen(),
        "a fresh State captures the ready lifecycle phase");
  Check(Capture->AllowsMutation(),
        "the owner thread of a ready, unfrozen State may mutate");
  Check(Capture->EntryStackDepth == *RootDepth,
        "the capture records the entry stack depth");
  Check(Capture->Identity == Hooks::LogicalIdentityOf(Owner).value_or(
                                 Luna::Detail::StateIdentity()),
        "the capture records the logical State identity");
  Check(Capture->OwnerEpoch == Hooks::OwnerEpochOf(Owner).value_or(0),
        "the capture records the owner-object epoch");
  Check(Capture->LifecycleGeneration ==
            Hooks::LifecycleGenerationOf(Owner).value_or(1),
        "the capture records the lifecycle generation");
  Check(Capture->Generations == Hooks::GenerationsOf(Owner),
        "the capture shares the current committed generation set");

  bool ForeignThreadRejected = false;
  std::thread Reader([&Owner, &ForeignThreadRejected] {
    const auto Foreign = Hooks::CaptureTransactionEntryOf(Owner);
    ForeignThreadRejected = Foreign.has_value() && !Foreign->IsOwnerThread() &&
                            !Foreign->AllowsMutation();
  });
  Reader.join();
  Check(ForeignThreadRejected,
        "a capture read from another thread rejects mutation");

  Luna::State Moved(std::move(Owner));
  const auto MovedCapture = Hooks::CaptureTransactionEntryOf(Moved);
  Check(MovedCapture.has_value() && MovedCapture->IsOwnerThread(),
        "a move preserves the owner-thread affinity");
  Check(MovedCapture.has_value() && MovedCapture->OwnerEpoch == 2,
        "a move advances the captured owner-object epoch");

  Check(Hooks::MarkFrozen(Moved) && Hooks::IsFrozen(Moved),
        "the frozen phase is observable");
  const auto FrozenCapture = Hooks::CaptureTransactionEntryOf(Moved);
  Check(FrozenCapture.has_value() && FrozenCapture->IsFrozen() &&
            !FrozenCapture->AllowsMutation(),
        "a frozen State captures a phase that rejects mutation");
}

void CheckFoundationPrecedence() {
  Luna::State Owner;
  int (*NullTarget)() = nullptr;

  Luna::RegistrationResult ForeignInvalid = Luna::RegistrationResult::Success();
  Luna::RegistrationResult ForeignValid = Luna::RegistrationResult::Success();
  std::thread Foreign([&Owner, &ForeignInvalid, &ForeignValid] {
    ForeignInvalid = Owner.Bindings().Register("Bad.Name", &AddIntegers);
    ForeignValid = Owner.Bindings().Register("ForeignName", &AddIntegers);
  });
  Foreign.join();

  Check(HasFailure(ForeignInvalid, Luna::ErrorCategory::InvalidGlobalName,
                   "byte 4"),
        "an invalid identifier is rejected before a wrong thread");
  Check(HasFailure(ForeignValid, Luna::ErrorCategory::StateNotReady,
                   "owner thread"),
        "a foreign thread is rejected deterministically");
  Check(Hooks::BindingCount(Owner) == 0 &&
            Hooks::PendingBindingCount(Owner) == 0,
        "a foreign-thread attempt stages nothing");
  Check(!Hooks::HasActiveTransaction(Owner),
        "a foreign-thread attempt leaves no active transaction");

  const auto Accepted = Owner.Bindings().Register("Add", &AddIntegers);
  Check(Accepted.IsSuccess(),
        "the owner thread still registers after a foreign attempt");

  Check(Hooks::MarkFrozen(Owner), "the State can enter the frozen phase");
  const auto FrozenNullTarget =
      Owner.Bindings().Register("FrozenCandidate", NullTarget);
  Check(HasFailure(FrozenNullTarget, Luna::ErrorCategory::StateNotReady,
                   "frozen"),
        "a frozen lifecycle is rejected before a null target");
  const auto FrozenDuplicate = Owner.Bindings().Register("Add", &AddIntegers);
  Check(
      HasFailure(FrozenDuplicate, Luna::ErrorCategory::StateNotReady, "frozen"),
      "a frozen lifecycle is rejected before a duplicate");
  Check(Hooks::BindingCount(Owner) == 1 &&
            Hooks::PendingBindingCount(Owner) == 0,
        "a frozen State stages nothing");

  const auto Execution = Owner.Execute("ObservedFrozen = Add(2, 3)");
  Check(Execution.IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "ObservedFrozen") == 5,
        "a frozen State keeps its committed behavior");
}

void CheckVmBackedOperationsRequireOwnerThread() {
  Luna::State Owner;
  int NativeCalls = 0;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry
            .Register("Counted",
                      [&NativeCalls] {
                        ++NativeCalls;
                        return NativeCalls;
                      })
            .IsSuccess(),
        "the affinity test callable registers on the owner thread");

  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  Luna::ExecutionResult ForeignExecution = Luna::ExecutionResult::Success();
  Luna::RegistrationResult ForeignConstant =
      Luna::RegistrationResult::Success();
  std::optional<Luna::NamespaceBuilder> ForeignBuilder;
  bool SnapshotWasReadable = false;

  std::thread Foreign([&] {
    ForeignExecution = Owner.Execute("Counted()");
    ForeignConstant = Registry.RegisterConstant("ForeignValue", 7);
    ForeignBuilder.emplace(Registry.RegisterNamespace("ForeignScope"));

    const Luna::ReflectionRecord Reflected = Snapshot.Find("Counted");
    SnapshotWasReadable = Reflected.IsValid() && Reflected.Name() == "Counted";
  });
  Foreign.join();

  Check(HasFailure(ForeignExecution, Luna::ErrorCategory::StateNotReady,
                   "owner thread"),
        "source execution rejects a foreign thread deterministically");
  Check(HasFailure(ForeignConstant, Luna::ErrorCategory::StateNotReady,
                   "owner thread"),
        "immediate value registration rejects a foreign thread");
  Check(ForeignBuilder.has_value(),
        "a builder facade can carry a rejected staged operation safely");
  if (ForeignBuilder) {
    const Luna::RegistrationResult Commit = ForeignBuilder->Commit();
    Check(
        HasFailure(Commit, Luna::ErrorCategory::StateNotReady, "owner thread"),
        "wrong-thread builder staging records one deterministic refusal");
  }
  Check(SnapshotWasReadable,
        "an owning immutable reflection snapshot is readable on any thread");
  Check(NativeCalls == 0,
        "foreign-thread execution reaches no native invocation");
  Check(EntryDepth.has_value() &&
            Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "foreign-thread operations never access or mutate the root stack");
  Check(!Hooks::HasActiveTransaction(Owner),
        "foreign-thread registration never installs an active transaction");
  Check(Registry.Reflection().Find("ForeignValue").IsValid() == false &&
            Registry.Reflection().Find("ForeignScope").IsValid() == false,
        "foreign-thread registration publishes no reflected symbol");

  const Luna::ExecutionResult Accepted = Owner.Execute("Counted()");
  Check(Accepted.IsSuccess() && NativeCalls == 1,
        "the owner thread can invoke normally after every refusal");
}

void CheckMovesPreserveConstructionThreadAffinity() {
  Luna::State ConstructionSource;
  int ConstructionCalls = 0;
  Check(ConstructionSource.Bindings()
            .Register("MovedConstructed",
                      [&ConstructionCalls] { ++ConstructionCalls; })
            .IsSuccess(),
        "the move-construction source registers on its owner thread");

  std::optional<Luna::State> Constructed;
  Luna::ExecutionResult ForeignConstructionExecution =
      Luna::ExecutionResult::Success();
  std::thread Constructor([&] {
    Constructed.emplace(std::move(ConstructionSource));
    ForeignConstructionExecution = Constructed->Execute("MovedConstructed()");
  });
  Constructor.join();

  Check(!ConstructionSource.IsReady() && Constructed.has_value() &&
            Constructed->IsReady(),
        "move construction transfers readiness from its source");
  Check(HasFailure(ForeignConstructionExecution,
                   Luna::ErrorCategory::StateNotReady, "owner thread") &&
            ConstructionCalls == 0,
        "move construction does not adopt the moving thread");
  Check(Constructed && Constructed->Execute("MovedConstructed()").IsSuccess() &&
            ConstructionCalls == 1,
        "the original construction thread remains the owner after move "
        "construction");

  Luna::State AssignmentSource;
  int AssignmentCalls = 0;
  Check(
      AssignmentSource.Bindings()
          .Register("MovedAssigned", [&AssignmentCalls] { ++AssignmentCalls; })
          .IsSuccess(),
      "the move-assignment source registers on its owner thread");

  std::optional<Luna::State> Assigned;
  Luna::ExecutionResult ForeignAssignmentExecution =
      Luna::ExecutionResult::Success();
  std::thread Assigner([&] {
    Luna::State WorkerOwnedDestination;
    WorkerOwnedDestination = std::move(AssignmentSource);
    ForeignAssignmentExecution =
        WorkerOwnedDestination.Execute("MovedAssigned()");
    Assigned.emplace(std::move(WorkerOwnedDestination));
  });
  Assigner.join();

  Check(!AssignmentSource.IsReady() && Assigned.has_value() &&
            Assigned->IsReady(),
        "move assignment transfers readiness from its source");
  Check(HasFailure(ForeignAssignmentExecution,
                   Luna::ErrorCategory::StateNotReady, "owner thread") &&
            AssignmentCalls == 0,
        "move assignment does not adopt the assigning thread");
  Check(Assigned && Assigned->Execute("MovedAssigned()").IsSuccess() &&
            AssignmentCalls == 1,
        "the original construction thread remains the owner after move "
        "assignment");
}

void CheckCanonicalCollisionDetection() {
  std::vector<CommittedSymbol> Committed;
  Committed.push_back(
      MakeCommittedSymbol(MakeFunctionPlanEntry("Committed", IntegerAdder())));
  Committed.push_back(MakeCommittedSymbol(ScopeEntry("Studio")));
  const std::shared_ptr<const CommittedSymbolTable> Table =
      CommittedSymbolTable::Build(std::move(Committed));

  DescriptorPlan Plan;
  static_cast<void>(
      Plan.Append(MakeFunctionPlanEntry("Pending", IntegerAdder())));
  const SymbolView View(*Table, Plan);
  const TransactionCapture Capture = ReadyCapture();

  const DescriptorPlanEntry Fresh =
      MakeFunctionPlanEntry("Fresh", IntegerAdder());
  Check(!ValidateRegistration(
             FunctionRequest("Fresh", &Fresh,
                             RegistrationPrecedence::FoundationRootFunction),
             Capture, View)
             .has_value(),
        "a declaration absent from the canonical model is accepted");

  const DescriptorPlanEntry Duplicate =
      MakeFunctionPlanEntry("Committed", IntegerAdder());
  const auto CommittedCollision = ValidateRegistration(
      FunctionRequest("Committed", &Duplicate,
                      RegistrationPrecedence::FoundationRootFunction),
      Capture, View);
  Check(HasDiagnostic(CommittedCollision,
                      Luna::ErrorCategory::DuplicateGlobalName,
                      "already registered"),
        "a committed symbol of the same category is a duplicate");

  const DescriptorPlanEntry PendingDuplicate =
      MakeFunctionPlanEntry("Pending", IntegerAdder());
  const auto PendingCollision = ValidateRegistration(
      FunctionRequest("Pending", &PendingDuplicate,
                      RegistrationPrecedence::FoundationRootFunction),
      Capture, View);
  Check(HasDiagnostic(PendingCollision,
                      Luna::ErrorCategory::DuplicateGlobalName,
                      "already registered"),
        "a pending symbol of the same category is a duplicate");

  const DescriptorPlanEntry ScopeClash =
      MakeFunctionPlanEntry("Studio", IntegerAdder());
  const auto CategoryCollision = ValidateRegistration(
      FunctionRequest("Studio", &ScopeClash,
                      RegistrationPrecedence::FoundationRootFunction),
      Capture, View);
  Check(HasDiagnostic(CategoryCollision,
                      Luna::ErrorCategory::DuplicateGlobalName, "scope"),
        "a symbol of another category is an incompatible-category collision");

  RegistrationValidationRequest Occupied = FunctionRequest(
      "Fresh", &Fresh, RegistrationPrecedence::FoundationRootFunction);
  Occupied.VmPathIsOccupied = true;
  Check(HasDiagnostic(ValidateRegistration(Occupied, Capture, View),
                      Luna::ErrorCategory::DuplicateGlobalName,
                      "already registered"),
        "an occupied canonical VM path is a duplicate");
}

void CheckGeneralOperationPrecedence() {
  const std::shared_ptr<const CommittedSymbolTable> Table =
      CommittedSymbolTable::Empty();
  DescriptorPlan Plan;
  const SymbolView View(*Table, Plan);

  const DescriptorPlanEntry Nested =
      MakeFunctionPlanEntry("Studio.Physics.Add", IntegerAdder());

  TransactionCapture Foreign = ReadyCapture();
  Foreign.OwnerThread = std::thread::id();
  Foreign.VirtualMachineIsReady = false;

  RegistrationValidationRequest Request = FunctionRequest(
      "Studio..Add", &Nested, RegistrationPrecedence::GeneralOperation);
  Check(HasDiagnostic(ValidateRegistration(Request, Foreign, View),
                      Luna::ErrorCategory::StateNotReady, "owner thread"),
        "a scoped operation rejects a wrong thread before an invalid name");

  TransactionCapture NotReady = ReadyCapture();
  NotReady.VirtualMachineIsReady = false;
  Check(HasDiagnostic(ValidateRegistration(Request, NotReady, View),
                      Luna::ErrorCategory::StateNotReady, "not ready"),
        "a scoped operation rejects a non-ready State before an invalid name");

  const TransactionCapture Capture = ReadyCapture();
  Check(HasDiagnostic(ValidateRegistration(Request, Capture, View),
                      Luna::ErrorCategory::InvalidGlobalName,
                      "segment is empty"),
        "a scoped operation rejects a malformed qualified name");

  RegistrationValidationRequest Foreignscope = FunctionRequest(
      "Studio.Physics.Add", &Nested, RegistrationPrecedence::GeneralOperation);
  Foreignscope.ParentQualifiedName = "Studio.Physics";
  Foreignscope.ScopeIsOwned = false;
  Foreignscope.HasTarget = false;
  Check(HasDiagnostic(ValidateRegistration(Foreignscope, Capture, View),
                      Luna::ErrorCategory::InvalidGlobalName,
                      "not a Luna-owned scope"),
        "a foreign parent scope is rejected before a null target");

  RegistrationValidationRequest StaleScope = FunctionRequest(
      "Studio.Physics.Add", &Nested, RegistrationPrecedence::GeneralOperation);
  StaleScope.ParentQualifiedName = "Studio.Physics";
  StaleScope.ScopeIsCurrent = false;
  StaleScope.HasTarget = false;
  Check(HasDiagnostic(ValidateRegistration(StaleScope, Capture, View),
                      Luna::ErrorCategory::StateNotReady, "stale"),
        "a stale parent scope is rejected before a null target");

  RegistrationValidationRequest NullTarget = FunctionRequest(
      "Studio.Physics.Add", &Nested, RegistrationPrecedence::GeneralOperation);
  NullTarget.ParentQualifiedName = "Studio.Physics";
  NullTarget.HasTarget = false;
  Check(HasDiagnostic(ValidateRegistration(NullTarget, Capture, View),
                      Luna::ErrorCategory::NullCallable, "callable target"),
        "a scoped operation names its kind and qualified name");

  const auto Accepted = ValidateRegistration(
      FunctionRequest("Studio.Physics.Add", &Nested,
                      RegistrationPrecedence::GeneralOperation),
      Capture, View);
  Check(!Accepted.has_value(),
        "a complete scoped declaration passes every validation phase");
}

void CheckTypeAndMetadataValidation() {
  const std::shared_ptr<const CommittedSymbolTable> Table =
      CommittedSymbolTable::Empty();
  DescriptorPlan Plan;
  const SymbolView View(*Table, Plan);
  const TransactionCapture Capture = ReadyCapture();

  DescriptorPlanEntry Unavailable =
      MakeFunctionPlanEntry("Unavailable", IntegerAdder());
  Check(Unavailable.Symbol.Signature.has_value(),
        "a planned callable carries its canonical signature");
  if (Unavailable.Symbol.Signature)
    Unavailable.Symbol.Signature->ParameterTypes[1] =
        Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Float);
  Check(HasDiagnostic(
            ValidateRegistration(
                FunctionRequest("Unavailable", &Unavailable,
                                RegistrationPrecedence::FoundationRootFunction),
                Capture, View),
            Luna::ErrorCategory::Internal, "parameter 2"),
        "an unavailable canonical type names its position");

  DescriptorPlanEntry Malformed =
      MakeFunctionPlanEntry("Malformed", IntegerAdder());
  if (Malformed.Symbol.Signature) {
    Malformed.Symbol.Signature->ParameterTypes.pop_back();
    Malformed.Symbol.Signature->RequiredParameterCount = 1;
  }
  Check(HasDiagnostic(
            ValidateRegistration(
                FunctionRequest("Malformed", &Malformed,
                                RegistrationPrecedence::FoundationRootFunction),
                Capture, View),
            Luna::ErrorCategory::Internal, "parameter count"),
        "callable metadata and the canonical signature must agree");

  const auto Missing = ValidateRegistration(
      FunctionRequest("Missing", nullptr,
                      RegistrationPrecedence::GeneralOperation),
      Capture, View);
  Check(HasDiagnostic(Missing, Luna::ErrorCategory::NullCallable,
                      "callable target"),
        "an absent descriptor still fails its target check first");

  RegistrationValidationRequest Described = FunctionRequest(
      "Missing", nullptr, RegistrationPrecedence::GeneralOperation);
  Described.HasTarget = true;
  Check(HasDiagnostic(ValidateRegistration(Described, Capture, View),
                      Luna::ErrorCategory::Internal, "canonically"),
        "a declaration Luna cannot describe fails as an internal failure");
}

void CheckPreparationStopsShortOfPublication() {
  Luna::State Owner;
  const auto Entry = Hooks::GenerationsOf(Owner);
  Check(Entry != nullptr && Entry->Generation() == 0,
        "a fresh State starts on the initial generation set");

  RegistrationTransaction Transaction(
      Hooks::CaptureTransactionEntryOf(Owner).value_or(TransactionCapture()));
  static_cast<void>(
      Transaction.Append(MakeFunctionPlanEntry("Alpha", IntegerAdder())));
  static_cast<void>(Transaction.Append(ScopeEntry("Studio")));

  PreparedGenerations Prepared;
  auto *Database = Hooks::ReflectionDatabaseOf(Owner);
  Check(Database != nullptr, "a State owns one reflection database");
  if (!Database || !Entry)
    return;

  const PreparationStatus Status =
      Luna::Detail::PrepareGenerations(Transaction, *Database, Prepared);
  Check(Status == PreparationStatus::Prepared && Prepared.IsPrepared(),
        "a complete plan prepares its replacement immutable stores");
  if (Prepared.IsPrepared()) {
    Check(Prepared.Candidate->Generation() == Entry->Generation() + 1,
          "the candidate generation set succeeds the captured one");
    Check(Prepared.Candidate->Symbols().Size() == 2,
          "the candidate committed table describes every planned symbol");
    Check(Prepared.Candidate->Symbols().Contains("Alpha") &&
              Prepared.Candidate->Symbols().Contains("Studio"),
          "the candidate committed table is canonical");
  }

  Check(Hooks::GenerationsOf(Owner) == Entry &&
            Hooks::GenerationsOf(Owner)->Symbols().IsEmpty(),
        "preparation leaves the committed generation set untouched");
  Check(Hooks::ReflectionGeneration(Owner) == 0,
        "preparation leaves the committed reflection generation untouched");

  RegistrationTransaction Incomplete(GenerationSet::Initial());
  DescriptorPlanEntry Broken = ScopeEntry("Broken");
  Broken.Symbol.QualifiedName.clear();
  static_cast<void>(Incomplete.Append(std::move(Broken)));
  PreparedGenerations Rejected;
  Check(Luna::Detail::PrepareGenerations(Incomplete, *Database, Rejected) ==
            PreparationStatus::IncompletePlan,
        "an incomplete canonical identity is never prepared");
  Check(!Rejected.IsPrepared(),
        "a rejected preparation returns no candidate generation");
}

void CheckPreparationFaultLeavesStateReusable() {
  Luna::State Owner;
  Hooks::InjectFault(Owner, FaultPoint::TransactionPreparation);
  const auto Failed = Owner.Bindings().Register("Prepared", &AddIntegers);
  Check(HasFailure(Failed, Luna::ErrorCategory::Internal, "preparation"),
        "a preparation fault fails the attempt deterministically");
  Check(Hooks::BindingCount(Owner) == 0 &&
            Hooks::PendingBindingCount(Owner) == 0,
        "a preparation fault stages nothing");
  Check(Hooks::GenerationsOf(Owner)->Generation() == 0,
        "a preparation fault publishes no generation");
  Check(!Hooks::HasActiveTransaction(Owner),
        "a failed attempt leaves no active transaction");

  const auto Retried = Owner.Bindings().Register("Prepared", &AddIntegers);
  const auto Execution = Owner.Execute("ObservedPrepared = Prepared(4, 5)");
  Check(Retried.IsSuccess() && Execution.IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "ObservedPrepared") == 9,
        "the State stays reusable after a preparation fault");
}

void CheckNestedSubmissionsJoinTheOuterTransaction() {
  Luna::State Owner;
  const auto EntryGenerations = Hooks::GenerationsOf(Owner);
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  std::vector<JoinedFunctionDeclaration> Group;
  Group.emplace_back("Alpha", IntegerAdder());
  Group.emplace_back("Mike", IntegerAdder());
  Group.emplace_back("Zulu", IntegerAdder());

  const auto Report =
      Hooks::SubmitJoinedFunctions(Owner, std::move(Group), true);
  Check(Report.Submitted == 3 && Report.JoinedSubmissions == 3,
        "every declaration of a group joins one outer transaction");
  Check(Report.Planned == 3 && Report.Prepared == 3,
        "a joined group plans and prepares every declaration");
  Check(Report.NestedFailures == 0 && Report.OuterCouldPublish,
        "a group without a nested failure could publish");
  Check(Report.Status == TransactionStatus::RolledBack,
        "a group that stops before publication rolls back");
  Check(Report.PendingSymbolsInView == 3 && Report.CommittedSymbolsInView == 0,
        "pending symbols are visible to validation inside the transaction");
  Check(Report.Preparation == PreparationStatus::Prepared &&
            Report.CandidateGeneration == 1 && Report.CandidateSymbols == 3,
        "one group prepares one candidate generation for every symbol");

  Check(Report.PublishedGenerationWhileOpen == 0 &&
            Report.PublishedSymbolsWhileOpen == 0,
        "an ordinary query sees no pending symbol");
  Check(Report.ReflectionGenerationWhileOpen == 0,
        "an ordinary reflection query sees no pending record");
  Check(Report.VmVisibleDeclarationsWhileOpen == 0,
        "no pending declaration is installed in the virtual machine");
  Check(Report.StagedBindingsWhileOpen == 3,
        "prepared resources are staged privately");
  Check(EntryDepth.has_value() && Report.EntryStackDepth == *EntryDepth &&
            Report.StackDepthWhileOpen == Report.EntryStackDepth,
        "preparation never disturbs the entry stack depth");

  Check(Hooks::GenerationsOf(Owner) == EntryGenerations,
        "a group that does not publish leaves the generation set untouched");
  Check(Hooks::BindingCount(Owner) == 0 &&
            Hooks::PendingBindingCount(Owner) == 0,
        "a group that does not publish discards every staged resource");
  Check(!Hooks::HasActiveTransaction(Owner),
        "a completed group leaves no active transaction");

  const auto Registered = Owner.Bindings().Register("Alpha", &AddIntegers);
  const auto Execution = Owner.Execute("ObservedAlpha = Alpha(6, 7)");
  Check(Registered.IsSuccess() && Execution.IsSuccess() &&
            Hooks::ObserveIntegerGlobal(Owner, "ObservedAlpha") == 13,
        "a discarded group leaves the State fully reusable");
}

void CheckIgnoredNestedFailurePoisonsTheOuterTransaction() {
  Luna::State Owner;

  std::vector<JoinedFunctionDeclaration> Group;
  Group.emplace_back("Alpha", IntegerAdder());
  Group.emplace_back("Alpha", IntegerAdder());
  Group.emplace_back("Zulu", IntegerAdder());

  const auto Report =
      Hooks::SubmitJoinedFunctions(Owner, std::move(Group), true);
  Check(Report.JoinedSubmissions == 3 && Report.Planned == 2,
        "a rejected nested declaration never joins the plan");
  Check(Report.NestedFailures == 1,
        "an ignored nested failure is recorded on the outer transaction");
  Check(Report.Status == TransactionStatus::RolledBack &&
            !Report.OuterCouldPublish,
        "an ignored nested failure prevents the outer transaction from "
        "publishing");
  Check(HasDiagnostic(Report.Failure, Luna::ErrorCategory::DuplicateGlobalName,
                      "already registered"),
        "the outer transaction keeps the first nested diagnostic");
  Check(Hooks::GenerationsOf(Owner)->Generation() == 0 &&
            Hooks::BindingCount(Owner) == 0 &&
            Hooks::PendingBindingCount(Owner) == 0,
        "a poisoned group publishes nothing and stages nothing");

  const auto Execution = Owner.Execute("ObservedPoisoned = Alpha == nil");
  Check(Execution.IsSuccess(),
        "a poisoned group leaves the virtual machine untouched");
}

} // namespace

int RunRegistrationCaptureAndPreparationTests() {
  FailureCount = 0;
  CheckEntryCapture();
  CheckFoundationPrecedence();
  CheckVmBackedOperationsRequireOwnerThread();
  CheckMovesPreserveConstructionThreadAffinity();
  CheckCanonicalCollisionDetection();
  CheckGeneralOperationPrecedence();
  CheckTypeAndMetadataValidation();
  CheckPreparationStopsShortOfPublication();
  CheckPreparationFaultLeavesStateReusable();
  CheckNestedSubmissionsJoinTheOuterTransaction();
  CheckIgnoredNestedFailurePoisonsTheOuterTransaction();
  return FailureCount == 0 ? 0 : 1;
}
