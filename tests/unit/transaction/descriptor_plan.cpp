// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/value.hpp>
#include <luna/detail/callable_adapter.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/state/state.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/identity/symbol_descriptor.hpp"
#include "state/registration/plan.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/transaction/generation_set.hpp"
#include "state/transaction/lifecycle.hpp"
#include "state/transaction/transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Luna::Detail::CommittedSymbol;
using Luna::Detail::CommittedSymbolTable;
using Luna::Detail::DescriptorPlan;
using Luna::Detail::DescriptorPlanEntry;
using Luna::Detail::GenerationSet;
using Luna::Detail::MakeCommittedSymbol;
using Luna::Detail::MakeFunctionPlanEntry;
using Luna::Detail::PlanEntryKind;
using Luna::Detail::RegistrationTransaction;
using Luna::Detail::StateLifecycle;
using Luna::Detail::SymbolView;
using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "descriptor plan check failed: " << Description << '\n';
}

[[nodiscard]] int AddIntegers(int Left, int Right) { return Left + Right; }

[[nodiscard]] Luna::ErasedCallableDescriptor IntegerAdder() {
  return Luna::Detail::MakeErasedCallableDescriptor(&AddIntegers);
}

[[nodiscard]] Luna::ErasedCallableDescriptor TextLength() {
  return Luna::Detail::MakeErasedCallableDescriptor(
      [](std::string Text) { return static_cast<int>(Text.size()); });
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

void CheckFoundationPlanEntry() {
  DescriptorPlanEntry Entry = MakeFunctionPlanEntry("Add", IntegerAdder());

  Check(Entry.Category == PlanEntryKind::Function,
        "a foundation request plans one function candidate");
  Check(Entry.VmPath == "Add", "the planned VM path is the global name");
  Check(Entry.Symbol.QualifiedName == "Add",
        "the canonical qualified name is the global name");
  Check(Entry.Symbol.Kind == Luna::SymbolKind::FunctionCandidate,
        "a foundation request plans a callable candidate symbol");
  Check(Entry.Symbol.Parent == Luna::SymbolId(),
        "a foundation request is planned in the root scope");
  Check(Entry.Identity.IsValid(), "a planned entry resolves its identity");
  Check(Entry.IsValid(), "a complete function entry is valid");

  const auto &Signature = Entry.Symbol.Signature;
  Check(Signature.has_value(), "a callable candidate carries its signature");
  if (Signature) {
    Check(Signature->ParameterTypes.size() == 2,
          "the canonical signature keeps both parameters");
    Check(Signature->RequiredParameterCount == 2,
          "foundation parameters are all required");
    Check(!Signature->IsVariadic, "foundation callables are never variadic");
    Check(!Signature->ReceiverType.has_value(),
          "a root function has no receiver");
    Check(Signature->ReturnType ==
              Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Int32),
          "an int return maps to the canonical int32 type");
    if (Signature->ParameterTypes.size() == 2)
      Check(Signature->ParameterTypes[0] ==
                Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Int32),
            "an int parameter maps to the canonical int32 type");
  }

  // Equal requests describe one identity; a different signature does not.
  const DescriptorPlanEntry Same = MakeFunctionPlanEntry("Add", IntegerAdder());
  const DescriptorPlanEntry Other = MakeFunctionPlanEntry("Add", TextLength());
  Check(Same.Identity == Entry.Identity,
        "equal canonical requests plan one identity");
  Check(Other.Identity != Entry.Identity,
        "a different signature plans a different identity");

  // Void returns map to the canonical void type rather than a value type.
  const DescriptorPlanEntry Sink = MakeFunctionPlanEntry(
      "Sink", Luna::Detail::MakeErasedCallableDescriptor([](int) {}));
  Check(Sink.Symbol.Signature.has_value() &&
            Sink.Symbol.Signature->ReturnType ==
                Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Void),
        "a void return maps to the canonical void type");

  // An entry without its required payload is incomplete.
  DescriptorPlanEntry Incomplete = MakeFunctionPlanEntry("Add", IntegerAdder());
  Incomplete.Callable.reset();
  Check(!Incomplete.IsValid(),
        "a function entry without a callable is incomplete");

  DescriptorPlanEntry PlannedType;
  PlannedType.Category = PlanEntryKind::Type;
  PlannedType.Symbol = Luna::Detail::MakeScopeSymbol(
      Luna::SymbolKind::Namespace, "Studio", Luna::SymbolId());
  if (const auto Identity =
          Luna::Detail::SymbolIdentityRegistry::ComputeIdentity(
              PlannedType.Symbol))
    PlannedType.Identity = *Identity;
  Check(!PlannedType.IsValid(),
        "a type entry without canonical type fields is incomplete");
}

void CheckCanonicalPlanOrdering() {
  const std::vector<std::string> Names{"Zulu", "Alpha", "Mike"};

  DescriptorPlan Forward;
  for (const std::string &Name : Names)
    static_cast<void>(
        Forward.Append(MakeFunctionPlanEntry(Name, IntegerAdder())));

  DescriptorPlan Reversed;
  for (std::size_t Index = Names.size(); Index > 0; --Index)
    static_cast<void>(Reversed.Append(
        MakeFunctionPlanEntry(Names[Index - 1], IntegerAdder())));

  const std::vector<std::size_t> ForwardOrder = Forward.CanonicalOrder();
  const std::vector<std::size_t> ReversedOrder = Reversed.CanonicalOrder();
  Check(ForwardOrder.size() == Names.size() &&
            ReversedOrder.size() == Names.size(),
        "canonical order covers every planned entry");

  std::vector<std::string> ForwardNames;
  for (const std::size_t Index : ForwardOrder)
    ForwardNames.push_back(Forward.At(Index)->Symbol.QualifiedName);
  std::vector<std::string> ReversedNames;
  for (const std::size_t Index : ReversedOrder)
    ReversedNames.push_back(Reversed.At(Index)->Symbol.QualifiedName);

  const std::vector<std::string> Expected{"Alpha", "Mike", "Zulu"};
  Check(ForwardNames == Expected,
        "canonical order sorts planned entries by qualified name");
  Check(ForwardNames == ReversedNames,
        "canonical order is independent of insertion order");

  Check(Forward.Size() == 3, "the plan keeps every appended entry");
  Check(Forward.CountOf(PlanEntryKind::Function) == 3,
        "the plan counts entries by category");
  Check(Forward.CountOf(PlanEntryKind::Module) == 0,
        "an absent category counts zero");
  Check(Forward.Contains("Mike"), "a planned entry is found by name");
  Check(!Forward.Contains("Absent"), "an unplanned name is not found");

  const DescriptorPlanEntry *Found = Forward.Find("Mike");
  Check(Found != nullptr && Found->Symbol.QualifiedName == "Mike",
        "lookup by qualified name returns the planned entry");
  Check(Found != nullptr && Forward.Find(Found->Identity) == Found,
        "lookup by identity returns the same planned entry");
  Check(Forward.At(Forward.Size()) == nullptr,
        "an out-of-range plan index has no entry");

  DescriptorPlan Empty;
  Check(Empty.IsEmpty() && Empty.CanonicalOrder().empty(),
        "an empty plan has no entries and no order");
}

void CheckCommittedSymbolTableAndGenerationSet() {
  const std::shared_ptr<const GenerationSet> Initial = GenerationSet::Initial();
  Check(Initial != nullptr && Initial->Generation() == 0,
        "the initial generation set is generation zero");
  Check(Initial != nullptr && Initial->Symbols().IsEmpty(),
        "the initial generation set commits no symbol");
  Check(Initial != nullptr && Initial->Reflection() != nullptr &&
            Initial->Reflection()->RecordCount() == 0,
        "the initial generation set shares the empty reflection generation");

  std::vector<CommittedSymbol> Symbols;
  Symbols.push_back(
      MakeCommittedSymbol(MakeFunctionPlanEntry("Zulu", IntegerAdder())));
  Symbols.push_back(
      MakeCommittedSymbol(MakeFunctionPlanEntry("Alpha", IntegerAdder())));
  Symbols.push_back(MakeCommittedSymbol(ScopeEntry("Studio")));

  const std::shared_ptr<const CommittedSymbolTable> Table =
      CommittedSymbolTable::Build(Symbols);
  Check(Table != nullptr && Table->Size() == 3,
        "the committed table keeps every published symbol");
  Check(Table != nullptr && Table->At(0) != nullptr &&
            Table->At(0)->Symbol.QualifiedName == "Alpha",
        "the committed table is canonically ordered");
  Check(Table != nullptr && Table->CountOf(PlanEntryKind::Function) == 2,
        "the committed table counts symbols by category");
  Check(Table != nullptr && Table->Contains("Studio"),
        "a committed symbol is found by qualified name");
  Check(Table != nullptr && Table->Find("Absent") == nullptr,
        "an uncommitted name is not found");
  const CommittedSymbol *Committed = Table ? Table->Find("Alpha") : nullptr;
  Check(Committed != nullptr && Committed->IsValid(),
        "a committed symbol keeps a complete canonical descriptor");
  Check(Committed != nullptr && Table->Find(Committed->Identity) == Committed,
        "a committed symbol is found by identity");

  std::vector<CommittedSymbol> Added;
  Added.push_back(
      MakeCommittedSymbol(MakeFunctionPlanEntry("Mike", IntegerAdder())));
  const std::shared_ptr<const CommittedSymbolTable> Extended =
      CommittedSymbolTable::Extend(*Table, std::move(Added));
  Check(Extended != nullptr && Extended->Size() == 4,
        "extending a table keeps the prior symbols");
  Check(Table->Size() == 3,
        "extending a table leaves the prior table unchanged");

  const std::shared_ptr<const GenerationSet> Published =
      GenerationSet::Derive(*Initial, Extended, Initial->Reflection());
  Check(Published != nullptr && Published->Generation() == 1,
        "publication advances the generation number");
  Check(Published != nullptr && Published->Symbols().Size() == 4,
        "publication swaps in the new committed symbol table");
  Check(Initial->Generation() == 0 && Initial->Symbols().IsEmpty(),
        "publication leaves the prior generation set untouched");
}

void CheckSymbolViewAndTransaction() {
  std::vector<CommittedSymbol> Symbols;
  Symbols.push_back(
      MakeCommittedSymbol(MakeFunctionPlanEntry("Committed", IntegerAdder())));
  const std::shared_ptr<const GenerationSet> Current =
      GenerationSet::Derive(*GenerationSet::Initial(),
                            CommittedSymbolTable::Build(std::move(Symbols)),
                            GenerationSet::Initial()->Reflection());

  RegistrationTransaction Transaction(Current);
  Check(Transaction.Status() == Luna::Detail::TransactionStatus::Open,
        "a new transaction is open");
  Check(Transaction.Captured().Generation() == Current->Generation(),
        "a transaction validates against the generation set it captured");

  static_cast<void>(
      Transaction.Append(MakeFunctionPlanEntry("Pending", IntegerAdder())));
  const SymbolView View = Transaction.Symbols();
  Check(View.CommittedCount() == 1 && View.PendingCount() == 1,
        "one view exposes committed and pending symbols");
  const auto Pending = View.Find("Pending");
  Check(Pending.has_value() && Pending->IsPending,
        "a pending symbol is visible inside its transaction");
  const auto Existing = View.Find("Committed");
  Check(Existing.has_value() && !Existing->IsPending,
        "a committed symbol is visible through the same schema");
  Check(Existing.has_value() && Existing->Symbol != nullptr &&
            Existing->Symbol->QualifiedName == "Committed",
        "the view reports canonical descriptors");
  Check(Pending.has_value() && View.Find(Pending->Identity).has_value(),
        "the view resolves pending symbols by identity");
  Check(!View.Contains("Absent"), "an unknown name is absent from the view");

  Transaction.Poison(Luna::ErrorDiagnostic::Create(
      Luna::ErrorCategory::Internal, "first failure"));
  Transaction.Poison(Luna::ErrorDiagnostic::Create(
      Luna::ErrorCategory::Internal, "second failure"));
  Check(Transaction.IsPoisoned(), "a failure poisons the transaction");
  Check(Transaction.Failure().has_value() &&
            Transaction.Failure()->Message() == "first failure",
        "the transaction keeps the first deterministic diagnostic");
  Transaction.MarkCommitted();
  Check(Transaction.IsPoisoned(), "a poisoned transaction cannot commit");
  Transaction.MarkRolledBack();
  Check(Transaction.Status() == Luna::Detail::TransactionStatus::RolledBack,
        "a poisoned transaction rolls back");

  // Nested submissions join the active outer transaction.
  RegistrationTransaction Outer(GenerationSet::Initial());
  RegistrationTransaction Nested(GenerationSet::Initial());
  RegistrationTransaction *Active = nullptr;
  {
    const Luna::Detail::ActiveTransactionScope OuterScope(Active, Outer);
    Check(OuterScope.IsOuter() && Active == &Outer,
          "the first scope opens the outer transaction");
    {
      const Luna::Detail::ActiveTransactionScope NestedScope(Active, Nested);
      Check(!NestedScope.IsOuter(),
            "a nested scope never becomes the outer one");
      Check(&NestedScope.Active() == &Outer,
            "a nested scope appends to the outer transaction");
      static_cast<void>(NestedScope.Active().Append(
          MakeFunctionPlanEntry("Nested", IntegerAdder())));
    }
    Check(Active == &Outer,
          "leaving a nested scope keeps the outer transaction");
    Check(Outer.Plan().Size() == 1,
          "nested declarations land in the outer plan");
    Check(Nested.Plan().IsEmpty(),
          "a joined transaction stages nothing of its own");
  }
  Check(Active == nullptr, "leaving the outer scope clears the active slot");
}

void CheckStateOwnsTheGenerationSet() {
  Luna::State Owner;
  Check(Owner.IsReady(), "a new State is ready");

  const std::shared_ptr<const GenerationSet> Generations =
      Hooks::GenerationsOf(Owner);
  Check(Generations != nullptr && Generations->Generation() == 0,
        "a State starts on the initial generation set");
  Check(Generations != nullptr && Generations->Symbols().IsEmpty(),
        "a State starts with no committed symbol");
  Check(!Hooks::HasActiveTransaction(Owner),
        "a State has no active transaction outside a submission");

  const auto Identity = Hooks::LogicalIdentityOf(Owner);
  Check(Identity.has_value() && Identity->IsValid(),
        "a State owns a logical identity");
  Check(Hooks::OwnerEpochOf(Owner) == std::optional<std::uint64_t>(1),
        "a fresh State starts at owner epoch one");
  Check(Hooks::LifecycleGenerationOf(Owner) == std::optional<std::uint64_t>(0),
        "a fresh State starts at lifecycle generation zero");

  Luna::State Other;
  const auto OtherIdentity = Hooks::LogicalIdentityOf(Other);
  Check(OtherIdentity.has_value() && *OtherIdentity != *Identity,
        "two States never share one logical identity");

  // Registration keeps its observable behavior while routing through the plan.
  const auto Registration = Owner.Bindings().Register("Add", &AddIntegers);
  Check(Registration.IsSuccess(), "registration still succeeds");
  Check(!Hooks::HasActiveTransaction(Owner),
        "a completed submission leaves no active transaction");
  Check(Hooks::BindingCount(Owner) == 1 &&
            Hooks::BindingIsCommitted(Owner, "Add"),
        "a successful registration commits its binding");

  const auto Duplicate = Owner.Bindings().Register("Add", &AddIntegers);
  Check(!Duplicate.IsSuccess() && Duplicate.Diagnostic() != nullptr &&
            Duplicate.Diagnostic()->Category() ==
                Luna::ErrorCategory::DuplicateGlobalName,
        "duplicate detection is unchanged");
  Check(Hooks::PendingBindingCount(Owner) == 0,
        "a rejected duplicate stages nothing");

  Luna::State Moved(std::move(Owner));
  const auto MovedIdentity = Hooks::LogicalIdentityOf(Moved);
  Check(MovedIdentity.has_value() && *MovedIdentity == *Identity,
        "a move preserves the logical State identity");
  Check(Hooks::OwnerEpochOf(Moved) == std::optional<std::uint64_t>(2),
        "a move advances the owner-object epoch");
  Check(!Hooks::LogicalIdentityOf(Owner).has_value(),
        "a moved-from State owns no implementation");

  const auto Execution = Moved.Execute("return Add(2, 3)");
  Check(Execution.IsSuccess(), "the moved State keeps its registered binding");
}

void CheckLifecycleCounters() {
  StateLifecycle Lifecycle;
  Check(Lifecycle.Identity().IsValid(),
        "a lifecycle assigns a logical identity");
  Check(Lifecycle.OwnerEpoch() == 1, "an owner epoch starts at one");
  Check(Lifecycle.Generation() == 0, "a lifecycle generation starts at zero");

  Lifecycle.AdvanceOwnerEpoch();
  Lifecycle.AdvanceGeneration();
  Lifecycle.AdvanceGeneration();
  Check(Lifecycle.OwnerEpoch() == 2, "advancing the owner epoch counts once");
  Check(Lifecycle.Generation() == 2,
        "advancing the lifecycle generation counts each replacement");

  const StateLifecycle Later;
  Check(Later.Identity() != Lifecycle.Identity(),
        "logical identities are never reused");
}

} // namespace

int RunDescriptorPlanTests() {
  FailureCount = 0;
  CheckFoundationPlanEntry();
  CheckCanonicalPlanOrdering();
  CheckCommittedSymbolTableAndGenerationSet();
  CheckSymbolViewAndTransaction();
  CheckStateOwnsTheGenerationSet();
  CheckLifecycleCounters();
  return FailureCount == 0 ? 0 : 1;
}
