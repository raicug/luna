// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>

#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "namespace builder check failed: " << Description << '\n';
}

[[nodiscard]] int AddIntegers(int Left, int Right) { return Left + Right; }

[[nodiscard]] std::string PathKind(Luna::State &Owner,
                                   const std::string &Path) {
  const auto Kind = Hooks::ObserveVmPathValueKind(Owner, Path);
  return Kind ? *Kind : std::string("<unavailable>");
}

[[nodiscard]] int StackDepth(const Luna::State &Owner) {
  const auto Depth = Hooks::ObserveRootStackDepth(Owner);
  return Depth ? *Depth : -1;
}

void CheckRootNamespaceStaysPendingUntilCommit() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Check(Studio.QualifiedName() == "Studio",
        "a root namespace builder carries its canonical qualified name");
  Check(PathKind(Owner, "Studio") == "absent",
        "a staged namespace is invisible to ordinary virtual-machine queries");
  Check(Hooks::NamespaceOwnershipCount(Owner) == 0,
        "a staged namespace owns no table before publication");
  Check(Registry.Reflection().IsEmpty(),
        "a staged namespace contributes no reflection record");

  const auto Result = Studio.Commit();
  Check(Result.IsSuccess(), "committing one staged namespace succeeds");
  Check(PathKind(Owner, "Studio") == "table",
        "a published namespace holds one Luna table at its exact path");
  Check(
      Hooks::NamespaceIsOwned(Owner, "Studio"),
      "a published namespace table carries Luna's private ownership identity");
  Check(StackDepth(Owner) == EntryDepth,
        "publishing a namespace restores the exact entry stack depth");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ReflectionRecord Record = Snapshot.Find("Studio");
  Check(Record.IsValid() && Record.Kind() == Luna::SymbolKind::Namespace,
        "a published namespace reflects one namespace record");
  Check(Record.Name() == "Studio" && Record.QualifiedName() == "Studio",
        "a root namespace record keeps its local and qualified names");
  Check(Record.Scope().IsRoot(), "a root namespace record has the root scope");
}

void CheckNestedChainPublishesOneCanonicalHierarchy() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::NamespaceBuilder Physics = Studio.RegisterNamespace("Physics");
  Luna::NamespaceBuilder Solver = Physics.RegisterNamespace("Solver");
  Check(Solver.QualifiedName() == "Studio.Physics.Solver",
        "nested builders join segments with the canonical separator");

  const auto Result = Solver.Commit();
  Check(Result.IsSuccess(), "one nested namespace plan commits as a unit");
  Check(PathKind(Owner, "Studio") == "table" &&
            PathKind(Owner, "Studio.Physics") == "table" &&
            PathKind(Owner, "Studio.Physics.Solver") == "table",
        "a nested plan creates the exact table path from parent to child");
  Check(Hooks::NamespaceOwnershipCount(Owner) == 3,
        "every published namespace of the plan is Luna-owned");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ReflectionRecord Parent = Snapshot.Find("Studio.Physics");
  const Luna::ReflectionRecord Child = Snapshot.Find("Studio.Physics.Solver");
  Check(Parent.IsValid() && Child.IsValid(),
        "every namespace of the plan reflects one record");
  Check(Child.Name() == "Solver",
        "a nested record's local name is the final canonical segment");
  Check(Child.Scope().Owner() == Parent.Id(),
        "a nested record's scope names its parent namespace");
}

void CheckUncommittedBuilderHasNoEffect() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  {
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    Luna::NamespaceBuilder Physics = Studio.RegisterNamespace("Physics");
    static_cast<void>(Physics.QualifiedName());
  }

  Check(PathKind(Owner, "Studio") == "absent",
        "destroying an uncommitted builder installs nothing");
  Check(Hooks::NamespaceOwnershipCount(Owner) == 0,
        "destroying an uncommitted builder records no ownership");
  Check(Registry.Reflection().IsEmpty(),
        "destroying an uncommitted builder publishes no reflection");
  Check(StackDepth(Owner) == EntryDepth,
        "destroying an uncommitted builder leaves the stack depth unchanged");

  Check(Registry.RegisterNamespace("Studio").Commit().IsSuccess(),
        "a State remains reusable after an abandoned builder");
}

void CheckReopeningOwnedNamespacePreservesPriorSymbols() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  Check(Registry.RegisterNamespace("Studio")
            .RegisterNamespace("Physics")
            .Commit()
            .IsSuccess(),
        "the first namespace plan publishes");
  const Luna::ReflectionSnapshot First = Registry.Reflection();
  const std::size_t FirstCount = First.Size();
  const Luna::SymbolId FirstIdentity = First.Find("Studio").Id();

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Check(Studio.Commit().IsSuccess(),
        "reopening a matching Luna-owned namespace succeeds");

  const Luna::ReflectionSnapshot Second = Registry.Reflection();
  Check(Second.Size() == FirstCount,
        "reopening a namespace adds no duplicate reflection record");
  Check(Second.Find("Studio").Id() == FirstIdentity,
        "reopening a namespace keeps its canonical identity");
  Check(Second.Find("Studio.Physics").IsValid(),
        "reopening a namespace preserves the symbols it already held");

  Check(Registry.RegisterNamespace("Studio")
            .RegisterNamespace("Render")
            .Commit()
            .IsSuccess(),
        "a reopened namespace permits additive registration");
  Check(PathKind(Owner, "Studio.Render") == "table",
        "an additive child of a reopened namespace installs its table");
}

void CheckForeignAndStaleTablesCollide() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  Check(Owner.Execute("Registry = { Marker = 7 }").IsSuccess(),
        "the script creates its own table");
  const auto ScriptTable = Registry.RegisterNamespace("Registry").Commit();
  Check(!ScriptTable.IsSuccess(),
        "a script-created table is never adopted as a namespace");
  Check(ScriptTable.Diagnostic() &&
            ScriptTable.Diagnostic()->Category() ==
                Luna::ErrorCategory::DuplicateGlobalName,
        "a script-created table collides deterministically");
  Check(Hooks::ObserveIntegerGlobal(Owner, "Registry").has_value() == false,
        "the script's table is not replaced by a namespace");
  Check(Owner.Execute("assert(Registry.Marker == 7)").IsSuccess(),
        "the script's table keeps its contents after the collision");
  Check(Hooks::NamespaceOwnershipCount(Owner) == 0,
        "a rejected namespace records no ownership");

  Check(Registry.RegisterNamespace("Studio").Commit().IsSuccess(),
        "a namespace at a free path still publishes");

  Check(Owner.Execute("Studio = {}").IsSuccess(),
        "the script replaces the namespace table");
  const auto Stale = Registry.RegisterNamespace("Studio").Commit();
  Check(!Stale.IsSuccess(),
        "a replaced Luna namespace table is never re-adopted");
  Check(!Hooks::NamespaceIsOwned(Owner, "Studio"),
        "a replaced namespace table is no longer recognized as Luna-owned");
}

void CheckWrongCategoryAndInvalidSegmentsFail() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.Register("Add", &AddIntegers).IsSuccess(),
        "a foundation function registers");

  const auto Category = Registry.RegisterNamespace("Add").Commit();
  Check(!Category.IsSuccess(),
        "a symbol of another category at the path is a collision");
  Check(Category.Diagnostic() && Category.Diagnostic()->Category() ==
                                     Luna::ErrorCategory::DuplicateGlobalName,
        "a wrong-category collision is deterministic");
  Check(Owner.Execute("assert(Add(2, 3) == 5)").IsSuccess(),
        "the existing function still works after the collision");

  const auto Separator = Registry.RegisterNamespace("Studio.Physics").Commit();
  Check(!Separator.IsSuccess(),
        "a namespace accepts exactly one identifier segment");
  Check(Separator.Diagnostic() && Separator.Diagnostic()->Category() ==
                                      Luna::ErrorCategory::InvalidGlobalName,
        "a multi-segment name is an invalid-name failure");

  const auto Empty = Registry.RegisterNamespace("").Commit();
  Check(!Empty.IsSuccess(), "an empty segment is rejected");

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::NamespaceBuilder Invalid = Studio.RegisterNamespace("1Bad");
  Check(!Invalid.Commit().IsSuccess(),
        "an invalid nested segment fails the whole plan");
  Check(PathKind(Owner, "Studio") == "absent",
        "a failed plan installs none of its namespaces");
}

void CheckFailedInstallationRemovesEveryCreatedTable() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);
  const std::uint64_t Generation = Hooks::GenerationsOf(Owner)->Generation();

  Hooks::InjectFault(Owner, Luna::Detail::StateFaultPoint::BindingInstallation,
                     1);
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  const auto Result = Studio.RegisterNamespace("Physics").Commit();
  Check(!Result.IsSuccess(), "an injected installation fault fails the plan");
  Check(PathKind(Owner, "Studio") == "absent" &&
            PathKind(Owner, "Studio.Physics") == "absent",
        "a failed plan removes every table it created");
  Check(Hooks::NamespaceOwnershipCount(Owner) == 0,
        "a failed plan records no namespace ownership");
  Check(Hooks::GenerationsOf(Owner)->Generation() == Generation,
        "a failed plan publishes no generation");
  Check(Registry.Reflection().IsEmpty(),
        "a failed plan publishes no reflection record");
  Check(StackDepth(Owner) == EntryDepth,
        "a failed plan restores the exact entry stack depth");
  Check(Hooks::PendingFaults(
            Owner, Luna::Detail::StateFaultPoint::BindingInstallation) == 0,
        "the injected fault was consumed");

  Check(Registry.RegisterNamespace("Studio")
            .RegisterNamespace("Physics")
            .Commit()
            .IsSuccess(),
        "the State stays reusable after a failed namespace plan");
  Check(PathKind(Owner, "Studio.Physics") == "table",
        "the retried plan installs its tables");
}

void CheckStaleBuildersFailDeterministically() {
  {
    Luna::State Owner;
    Luna::NamespaceBuilder Studio =
        Owner.Bindings().RegisterNamespace("Studio");
    Luna::State Moved = std::move(Owner);
    const auto Result = Studio.Commit();
    Check(!Result.IsSuccess(), "a builder of a moved-from owner is stale");
    Check(Result.Diagnostic() && Result.Diagnostic()->Category() ==
                                     Luna::ErrorCategory::StateNotReady,
          "a stale builder reports a deterministic stale-builder diagnostic");
    Check(PathKind(Moved, "Studio") == "absent",
          "a stale builder installs nothing into the new owner");
  }

  {
    auto Owner = std::make_unique<Luna::State>();
    std::optional<Luna::NamespaceBuilder> Ghost(
        Owner->Bindings().RegisterNamespace("Ghost"));
    Owner.reset();
    const auto Result = Ghost->Commit();
    Check(!Result.IsSuccess(), "a builder of a destroyed owner is stale");
    Check(Result.Diagnostic() && Result.Diagnostic()->Category() ==
                                     Luna::ErrorCategory::StateNotReady,
          "a destroyed owner yields a deterministic stale-builder diagnostic");
  }

  {
    Luna::State Owner;
    Luna::NamespaceBuilder Studio =
        Owner.Bindings().RegisterNamespace("Studio");
    Check(Hooks::MarkFrozen(Owner), "the State enters the frozen phase");
    const auto Result = Studio.Commit();
    Check(!Result.IsSuccess(), "a frozen State rejects a builder commit");
    Check(Result.Diagnostic() && Result.Diagnostic()->Category() ==
                                     Luna::ErrorCategory::StateNotReady,
          "a frozen commit is a deterministic lifecycle failure");
    Check(PathKind(Owner, "Studio") == "absent",
          "a frozen commit installs nothing");
  }

  {
    Luna::State Owner;
    Luna::NamespaceBuilder Studio =
        Owner.Bindings().RegisterNamespace("Studio");
    Check(Hooks::AdvanceLifecycleGeneration(Owner),
          "the lifecycle generation advances");
    const auto Result = Studio.Commit();
    Check(!Result.IsSuccess(), "a builder of a replaced generation is stale");
    Check(PathKind(Owner, "Studio") == "absent",
          "a replaced-generation commit installs nothing");
  }

  {
    Luna::State Owner;
    Luna::NamespaceBuilder Studio =
        Owner.Bindings().RegisterNamespace("Studio");
    Check(Studio.Commit().IsSuccess(), "the plan commits once");
    Check(!Studio.Commit().IsSuccess(), "the same plan cannot commit twice");
  }

  {
    Luna::State Owner;
    Luna::State Moved = std::move(Owner);
    Luna::NamespaceBuilder Studio =
        Owner.Bindings().RegisterNamespace("Studio");
    Check(!Studio.Commit().IsSuccess(),
          "a builder of a moved-from State fails safely");
    Check(PathKind(Moved, "Studio") == "absent",
          "a builder of a moved-from State installs nothing anywhere");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Check(Registry.RegisterNamespace("Studio").Commit().IsSuccess(),
          "the parent namespace publishes");
    Check(Hooks::AdvanceLifecycleGeneration(Owner),
          "the lifecycle generation advances");
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    const auto Result = Studio.RegisterNamespace("Physics").Commit();
    Check(!Result.IsSuccess(),
          "a namespace whose committed table belongs to a replaced generation "
          "is stale");
    Check(PathKind(Owner, "Studio.Physics") == "absent",
          "a stale scope installs no child table");
  }
}

void CheckForeignThreadIsRejectedFirst() {
  Luna::State Owner;
  Luna::NamespaceBuilder Studio = Owner.Bindings().RegisterNamespace("Studio");

  std::optional<Luna::RegistrationResult> Result;
  std::thread Foreign([&Studio, &Result] { Result = Studio.Commit(); });
  Foreign.join();

  Check(Result && !Result->IsSuccess(),
        "a namespace commit from a foreign thread fails");
  Check(Result && Result->Diagnostic() &&
            Result->Diagnostic()->Category() ==
                Luna::ErrorCategory::StateNotReady,
        "a foreign-thread commit is a deterministic lifecycle failure");
  Check(PathKind(Owner, "Studio") == "absent",
        "a foreign-thread commit installs nothing");
}

} // namespace

int RunNamespaceBuilderTests() {
  FailureCount = 0;
  CheckRootNamespaceStaysPendingUntilCommit();
  CheckNestedChainPublishesOneCanonicalHierarchy();
  CheckUncommittedBuilderHasNoEffect();
  CheckReopeningOwnedNamespacePreservesPriorSymbols();
  CheckForeignAndStaleTablesCollide();
  CheckWrongCategoryAndInvalidSegmentsFail();
  CheckFailedInstallationRemovesEveryCreatedTable();
  CheckStaleBuildersFailDeterministically();
  CheckForeignThreadIsRejectedFirst();
  return FailureCount == 0 ? 0 : 1;
}
