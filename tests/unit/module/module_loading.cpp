// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/enum_builder.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "module loading check failed: " << Description << '\n';
}

[[nodiscard]] Luna::SemanticVersion Version(std::string_view Text) {
  const auto Parsed = Luna::SemanticVersion::TryParse(Text);
  return Parsed ? *Parsed : Luna::SemanticVersion();
}

[[nodiscard]] Luna::ModuleDependency
Dependency(std::string Identity, const std::vector<std::string> &Constraints) {
  Luna::ModuleDependency Declared;
  Declared.Identity = std::move(Identity);
  for (const std::string &Text : Constraints) {
    const auto Parsed = Luna::VersionConstraint::TryParse(Text);
    if (Parsed)
      Declared.Constraints.push_back(*Parsed);
  }
  return Declared;
}

[[nodiscard]] Luna::ModuleExport Exported(Luna::SymbolKind Kind,
                                          std::string Name) {
  Luna::ModuleExport Declared;
  Declared.Kind = Kind;
  Declared.Name = std::move(Name);
  return Declared;
}

[[nodiscard]] Luna::ModuleManifest
Manifest(std::string Identity, std::string_view VersionText,
         std::vector<Luna::ModuleDependency> Dependencies = {},
         std::vector<Luna::ModuleExport> Exports = {},
         std::string Documentation = std::string()) {
  auto Created = Luna::ModuleManifest::TryCreate(
      std::move(Identity), Version(VersionText), std::move(Dependencies),
      std::move(Documentation), std::move(Exports));
  return Created ? std::move(*Created) : Luna::ModuleManifest();
}

[[nodiscard]] std::string PathKind(Luna::State &Owner,
                                   const std::string &Path) {
  const auto Kind = Hooks::ObserveVmPathValueKind(Owner, Path);
  return Kind ? *Kind : std::string("<unavailable>");
}

[[nodiscard]] int StackDepth(const Luna::State &Owner) {
  const auto Depth = Hooks::ObserveRootStackDepth(Owner);
  return Depth ? *Depth : -1;
}

// One scoped enumeration a module declares, so a module's canonical type
// enumeration has something to report.
enum class Alignment { Left = 0, Right = 1 };

// One module that publishes a namespace with one constant inside it.
struct RecordingModule final {
  std::string Namespace;
  std::string Constant;
  int Value = 0;
  std::vector<std::string> *Order = nullptr;
  std::string Identity;

  void operator()(Luna::NamespaceBuilder &Builder) const {
    if (Order)
      Order->push_back(Identity);
    Luna::NamespaceBuilder Scope = Builder.RegisterNamespace(Namespace);
    static_cast<void>(Scope.RegisterConstant(Constant, Value));
  }
};

void CheckOneProvidedModuleLoadsAtomically() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  const Luna::ModuleManifest Physics =
      Manifest("studio.physics", "1.2.0", {},
               {Exported(Luna::SymbolKind::Namespace, "Physics"),
                Exported(Luna::SymbolKind::Constant, "Physics.Gravity")},
               "Rigid body physics.");
  Check(Physics.IsValid(), "the test manifest is valid");

  std::size_t Callbacks = 0;
  const auto Result = Registry.RegisterModule(
      Physics, [&Callbacks](Luna::NamespaceBuilder &Builder) {
        ++Callbacks;
        Luna::NamespaceBuilder Scope = Builder.RegisterNamespace("Physics");
        static_cast<void>(Scope.RegisterConstant("Gravity", 10));
      });

  Check(Result.IsSuccess(), "one requested module loads");
  Check(Callbacks == 1, "the requested module callback runs exactly once");
  Check(Hooks::LoadedModuleCount(Owner) == 1,
        "a published load records exactly one loaded module");
  Check(Hooks::ModuleIsLoaded(Owner, "studio.physics"),
        "the loaded identity is the requested one");
  Check(Hooks::AvailableModuleCount(Owner) == 1,
        "a loaded module becomes available to later dependents");
  Check(PathKind(Owner, "Physics") == "table" &&
            Owner.Execute("assert(Physics.Gravity == 10)").IsSuccess(),
        "the module's namespace and constant publish together");
  Check(StackDepth(Owner) == EntryDepth,
        "publishing a module restores the exact entry stack depth");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ReflectionRecord Record = Snapshot.Find("studio.physics");
  Check(Record.IsValid() && Record.Kind() == Luna::SymbolKind::Module,
        "a loaded module reflects one module record");
  Check(Snapshot.Modules().Size() == 1,
        "a loaded module is enumerated exactly once");

  const Luna::ModuleRecord Module = Snapshot.Modules().At(0);
  Check(Module.Identity() == "studio.physics" && Module.Version() == "1.2.0",
        "module reflection reports the loaded identity and version");
  Check(Module.Symbol() == Record.Id(),
        "module reflection names its own module symbol");
  Check(Module.Documentation() == "Rigid body physics.",
        "module reflection keeps the manifest documentation");
  Check(Module.ExportCount() == 2 && Module.Export(0).Name() == "Physics" &&
            Module.Export(1).Name() == "Physics.Gravity",
        "module exports enumerate in canonical name order");
  Check(Module.NamespaceCount() == 1 && Module.Namespace(0) == "Physics",
        "module reflection enumerates the namespaces the module declared");
  Check(Module.DependencyCount() == 0,
        "a module without dependencies enumerates none");
}

void CheckDependencyGraphRunsInOneTransaction() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  // Two versions of the same dependency are available; resolution must select
  // the highest one satisfying the accumulated constraint.
  std::vector<std::string> Order;
  const RecordingModule OldMath{"MathOld", "Version", 1, &Order, "studio.math"};
  const RecordingModule NewMath{"Math", "Version", 2, &Order, "studio.math"};
  const RecordingModule Units{"Units", "Metres", 3, &Order, "studio.units"};

  Check(Registry.ProvideModule(Manifest("studio.math", "1.0.0"), OldMath)
            .IsSuccess(),
        "an older dependency version becomes available");
  Check(Registry.ProvideModule(Manifest("studio.math", "2.1.0"), NewMath)
            .IsSuccess(),
        "a newer dependency version becomes available");
  Check(Registry.ProvideModule(Manifest("studio.units", "1.0.0"), Units)
            .IsSuccess(),
        "a second dependency becomes available");
  Check(Order.empty(), "providing a definition never runs its callback");
  Check(Hooks::LoadedModuleCount(Owner) == 0 && Registry.Reflection().IsEmpty(),
        "providing definitions publishes nothing at all");
  Check(Hooks::AvailableModuleCount(Owner) == 3,
        "every provided definition is available to resolution");

  const Luna::ModuleManifest Engine =
      Manifest("studio.engine", "1.0.0",
               {Dependency("studio.math", {">=2.0.0"}),
                Dependency("studio.units", {">=1.0.0"})});

  const auto Result = Registry.RegisterModule(
      Engine, [&Order](Luna::NamespaceBuilder &Builder) {
        Order.push_back("studio.engine");
        Luna::NamespaceBuilder Scope = Builder.RegisterNamespace("Engine");
        static_cast<void>(Scope.RegisterConstant("Ready", true));
      });

  Check(Result.IsSuccess(), "a resolved dependency graph loads");
  Check(Order.size() == 3 && Order.back() == "studio.engine",
        "the requested callback runs after every dependency callback");
  Check(Order[0] == "studio.math" && Order[1] == "studio.units",
        "dependency callbacks run in canonical dependency-first order");
  Check(Hooks::LoadedModuleCount(Owner) == 3,
        "every module of the graph is recorded as loaded");
  Check(Hooks::LoadedModuleVersion(Owner, "studio.math") &&
            *Hooks::LoadedModuleVersion(Owner, "studio.math") == "2.1.0",
        "resolution selects the highest version satisfying the constraint");
  Check(Owner.Execute("assert(Math.Version == 2)").IsSuccess() &&
            Owner.Execute("assert(Units.Metres == 3)").IsSuccess() &&
            Owner.Execute("assert(Engine.Ready == true)").IsSuccess(),
        "every module of the graph published its declarations together");
  Check(PathKind(Owner, "MathOld") == "absent",
        "an unselected version never runs and never publishes");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  Check(Snapshot.Generation() == 1,
        "one module graph publishes exactly one reflection generation");
  Check(Snapshot.Modules().Size() == 3,
        "module reflection enumerates the whole loaded graph");
  Check(Snapshot.Modules().At(0).Identity() == "studio.engine" &&
            Snapshot.Modules().At(1).Identity() == "studio.math" &&
            Snapshot.Modules().At(2).Identity() == "studio.units",
        "loaded modules enumerate in canonical identity order");

  const Luna::ModuleRecord Requested = Snapshot.Modules().At(0);
  Check(Requested.DependencyCount() == 2,
        "module reflection enumerates every declared dependency");
  Check(Requested.Dependency(0).Identity() == "studio.math" &&
            Requested.Dependency(0).Version() == "2.1.0" &&
            Requested.Dependency(0).Constraints() == ">=2.0.0",
        "a dependency reports its resolved version and declared constraints");
  Check(Requested.Dependency(1).Identity() == "studio.units",
        "dependencies enumerate in canonical identity order");
}

void CheckRepeatedLoadIsIdempotentAndConflictsAreRejected() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  const Luna::ModuleManifest Physics = Manifest("studio.physics", "1.0.0");
  std::size_t Callbacks = 0;
  const auto Configure = [&Callbacks](Luna::NamespaceBuilder &Builder) {
    ++Callbacks;
    Luna::NamespaceBuilder Scope = Builder.RegisterNamespace("Physics");
    static_cast<void>(Scope.RegisterConstant("Gravity", 10));
  };

  Check(Registry.RegisterModule(Physics, Configure).IsSuccess(),
        "the first load of a module succeeds");
  const std::uint64_t Published = Registry.Reflection().Generation();

  Check(Registry.RegisterModule(Physics, Configure).IsSuccess(),
        "re-registering the same identity, version, and definition succeeds");
  Check(Callbacks == 1, "an idempotent repeat never reruns the callback");
  Check(Registry.Reflection().Generation() == Published,
        "an idempotent repeat publishes no new generation");
  Check(Hooks::LoadedModuleCount(Owner) == 1,
        "an idempotent repeat records no second module");

  // A same-version unequal definition and a different version of a loaded
  // identity are conflicts, not replacements.
  const Luna::ModuleManifest Unequal =
      Manifest("studio.physics", "1.0.0", {},
               {Exported(Luna::SymbolKind::Namespace, "Physics")});
  const auto Conflicting = Registry.RegisterModule(Unequal, Configure);
  Check(!Conflicting.IsSuccess() && Conflicting.Diagnostic() &&
            Conflicting.Diagnostic()->Category() ==
                Luna::ErrorCategory::DuplicateGlobalName,
        "a same-version unequal definition is a deterministic conflict");

  const auto Replacement =
      Registry.RegisterModule(Manifest("studio.physics", "2.0.0"), Configure);
  Check(!Replacement.IsSuccess(),
        "a different version of a loaded identity is a conflict");
  Check(Callbacks == 1 && Registry.Reflection().Generation() == Published &&
            Hooks::LoadedModuleCount(Owner) == 1,
        "a rejected conflict mutates nothing at all");
}

void CheckUnresolvableGraphPreservesThePreLoadState() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  Check(Registry.Register("Existing", [] { return 1; }).IsSuccess(),
        "the State has a committed symbol before the failing load");
  const std::uint64_t Published = Registry.Reflection().Generation();

  const Luna::ModuleManifest Engine = Manifest(
      "studio.engine", "1.0.0", {Dependency("studio.missing", {">=1.0.0"})});
  std::size_t Callbacks = 0;
  const auto Missing = Registry.RegisterModule(
      Engine, [&Callbacks](Luna::NamespaceBuilder &Builder) {
        ++Callbacks;
        static_cast<void>(Builder.RegisterNamespace("Engine").Commit());
      });
  Check(!Missing.IsSuccess() && Missing.Diagnostic(),
        "a missing dependency fails the load");
  Check(Missing.Diagnostic()->Message().find("studio.missing") !=
            std::string::npos,
        "the diagnostic names the missing dependency");
  Check(Missing.Diagnostic()->Message().find("dependency path") !=
            std::string::npos,
        "the diagnostic carries the canonical dependency path");
  Check(Callbacks == 0,
        "an unresolvable graph never runs a registration callback");

  // An available dependency whose only version violates the constraint.
  Check(Registry
            .ProvideModule(
                Manifest("studio.math", "1.0.0"),
                RecordingModule{"Math", "Version", 1, nullptr, "studio.math"})
            .IsSuccess(),
        "a dependency definition becomes available");
  const auto Unsatisfied = Registry.RegisterModule(
      Manifest("studio.engine", "1.0.0",
               {Dependency("studio.math", {">=2.0.0"})}),
      [&Callbacks](Luna::NamespaceBuilder &Builder) {
        ++Callbacks;
        static_cast<void>(Builder.RegisterNamespace("Engine").Commit());
      });
  Check(!Unsatisfied.IsSuccess(),
        "an unsatisfied constraint fails the load deterministically");
  Check(Callbacks == 0, "no callback runs for an unsatisfied constraint");

  Check(Hooks::LoadedModuleCount(Owner) == 0,
        "a failed load records no loaded module");
  Check(Registry.Reflection().Generation() == Published,
        "a failed load publishes no reflection generation");
  Check(PathKind(Owner, "Engine") == "absent" &&
            PathKind(Owner, "Math") == "absent",
        "a failed load leaves every module path exactly as it was");
  Check(Owner.Execute("assert(Existing() == 1)").IsSuccess(),
        "the pre-load committed model still works");
  Check(StackDepth(Owner) == EntryDepth,
        "a failed load restores the exact entry stack depth");
}

void CheckNestedFailurePoisonsTheWholeLoad() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  std::vector<std::string> Order;
  Check(Registry
            .ProvideModule(
                Manifest("studio.math", "1.0.0"),
                RecordingModule{"Math", "Version", 1, &Order, "studio.math"})
            .IsSuccess(),
        "the dependency definition becomes available");

  // The requested callback ignores a failing nested registration. An ignored
  // nested failure still poisons the module's outer transaction.
  const auto Result = Registry.RegisterModule(
      Manifest("studio.engine", "1.0.0",
               {Dependency("studio.math", {">=1.0.0"})}),
      [](Luna::NamespaceBuilder &Builder) {
        Luna::NamespaceBuilder Scope = Builder.RegisterNamespace("Engine");
        static_cast<void>(Scope.RegisterConstant("Ready", true));
        // An invalid identifier segment is a deterministic staging failure
        // whose result the callback deliberately drops.
        static_cast<void>(Builder.RegisterNamespace("not a name"));
      });

  Check(!Result.IsSuccess(), "an ignored nested failure fails the whole load");
  Check(Order.size() == 1,
        "the dependency callback still ran inside the poisoned transaction");
  Check(Hooks::LoadedModuleCount(Owner) == 0,
        "a poisoned load records no module, not even a dependency");
  Check(Registry.Reflection().IsEmpty(),
        "a poisoned load publishes no reflection record");
  Check(PathKind(Owner, "Math") == "absent" &&
            PathKind(Owner, "Engine") == "absent",
        "a poisoned load publishes no dependency or requested declaration");
  Check(Hooks::AvailableModuleCount(Owner) == 1,
        "a poisoned load adds no availability of its own");
}

void CheckCallbackExceptionsAreContained() {
  for (const bool Standard : {true, false}) {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    const int EntryDepth = StackDepth(Owner);

    std::vector<std::string> Order;
    Check(Registry
              .ProvideModule(
                  Manifest("studio.math", "1.0.0"),
                  RecordingModule{"Math", "Version", 1, &Order, "studio.math"})
              .IsSuccess(),
          "the dependency definition becomes available");

    const auto Result = Registry.RegisterModule(
        Manifest("studio.engine", "1.0.0",
                 {Dependency("studio.math", {">=1.0.0"})}),
        [Standard](Luna::NamespaceBuilder &Builder) {
          Luna::NamespaceBuilder Scope = Builder.RegisterNamespace("Engine");
          static_cast<void>(Scope.RegisterConstant("Ready", true));
          if (Standard)
            throw std::runtime_error("module callback threw");
          throw 17;
        });

    Check(!Result.IsSuccess() && Result.Diagnostic() &&
              Result.Diagnostic()->Category() == Luna::ErrorCategory::Internal,
          "a callback exception is contained as one internal failure");
    Check(Order.size() == 1,
          "the dependency callback ran before the requested one threw");
    Check(Hooks::LoadedModuleCount(Owner) == 0,
          "a thrown callback records no loaded module");
    Check(Registry.Reflection().IsEmpty(),
          "a thrown callback publishes no reflection record");
    Check(PathKind(Owner, "Math") == "absent" &&
              PathKind(Owner, "Engine") == "absent",
          "a thrown callback rolls the whole graph back to the pre-load state");
    Check(StackDepth(Owner) == EntryDepth,
          "a thrown callback restores the exact entry stack depth");

    // The State stays usable: the same graph loads once the callback behaves.
    Check(
        Registry
            .RegisterModule(
                Manifest("studio.engine", "1.0.0",
                         {Dependency("studio.math", {">=1.0.0"})}),
                RecordingModule{"Engine", "Ready", 5, &Order, "studio.engine"})
            .IsSuccess(),
        "the State recovers and loads the same graph afterwards");
    Check(Hooks::LoadedModuleCount(Owner) == 2,
          "the recovered load records the whole graph");
  }
}

void CheckInjectedFailuresLeaveNoModuleBehind() {
  const Luna::Detail::StateFaultPoint Points[] = {
      Luna::Detail::StateFaultPoint::TransactionPublication,
      Luna::Detail::StateFaultPoint::BindingInstallation,
      Luna::Detail::StateFaultPoint::TransactionConsistency};

  for (const Luna::Detail::StateFaultPoint Point : Points) {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    const int EntryDepth = StackDepth(Owner);

    Hooks::InjectFault(Owner, Point);
    const auto Result = Registry.RegisterModule(
        Manifest("studio.physics", "1.0.0"),
        RecordingModule{"Physics", "Gravity", 10, nullptr, "studio.physics"});

    Check(!Result.IsSuccess(),
          "an injected preparation, installation, or consistency failure fails "
          "the load");
    Check(Hooks::LoadedModuleCount(Owner) == 0,
          "an injected failure records no loaded module");
    Check(Registry.Reflection().IsEmpty(),
          "an injected failure publishes no reflection record");
    Check(PathKind(Owner, "Physics") == "absent",
          "an injected failure restores every journalled path");
    Check(StackDepth(Owner) == EntryDepth,
          "an injected failure restores the exact entry stack depth");
    Check(Registry.RegisterConstant("Recovered", 1).IsSuccess(),
          "the State remains usable after an injected module failure");
  }
}

void CheckLifecycleRejectionsNameTheModule() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const Luna::ModuleManifest Physics = Manifest("studio.physics", "1.0.0");
  const RecordingModule Configure{"Physics", "Gravity", 10, nullptr,
                                  "studio.physics"};

  Luna::RegistrationResult Foreign = Luna::RegistrationResult::Success();
  std::thread Other([&Registry, &Foreign, &Physics, &Configure] {
    Foreign = Registry.RegisterModule(Physics, Configure);
  });
  Other.join();
  Check(!Foreign.IsSuccess() && Foreign.Diagnostic() &&
            Foreign.Diagnostic()->Category() ==
                Luna::ErrorCategory::StateNotReady,
        "a foreign thread cannot load a module");
  Check(Foreign.Diagnostic()->Message().find("module 'studio.physics@1.0.0'") !=
            std::string::npos,
        "the lifecycle diagnostic names the attempted module");
  Check(Hooks::LoadedModuleCount(Owner) == 0,
        "a rejected thread mutates no module state");

  Check(Hooks::MarkFrozen(Owner), "the State can be frozen for this check");
  const auto Frozen = Registry.RegisterModule(Physics, Configure);
  Check(!Frozen.IsSuccess() && Frozen.Diagnostic() &&
            Frozen.Diagnostic()->Category() ==
                Luna::ErrorCategory::StateNotReady,
        "a frozen State rejects module loading");
  const auto Provided = Registry.ProvideModule(Physics, Configure);
  Check(!Provided.IsSuccess(),
        "a frozen State rejects module availability as well");
  Check(Hooks::AvailableModuleCount(Owner) == 0 &&
            Hooks::LoadedModuleCount(Owner) == 0,
        "a frozen rejection mutates nothing");
}

void CheckModuleRequestedInsideANamespacePlan() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  std::vector<std::string> Order;
  Check(Registry
            .ProvideModule(
                Manifest("studio.math", "1.0.0"),
                RecordingModule{"Math", "Version", 1, &Order, "studio.math"})
            .IsSuccess(),
        "the dependency definition becomes available");

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  static_cast<void>(Studio.RegisterConstant("Name", "Luna"));
  static_cast<void>(Studio.RegisterModule(
      Manifest("studio.engine", "1.0.0",
               {Dependency("studio.math", {">=1.0.0"})}),
      [](Luna::NamespaceBuilder &Builder) {
        Luna::NamespaceBuilder Scope = Builder.RegisterNamespace("Engine");
        static_cast<void>(Scope.RegisterConstant("Ready", true));
      }));

  Check(Order.empty(), "a staged module runs no callback before commit");
  Check(Hooks::LoadedModuleCount(Owner) == 0,
        "a staged module loads nothing before commit");

  const auto Result = Studio.Commit();
  Check(Result.IsSuccess(), "one plan commits its namespace and its module");
  Check(Order.size() == 1, "committing the plan runs the dependency callback");
  Check(Hooks::LoadedModuleCount(Owner) == 2,
        "the whole graph loads with the builder plan");
  Check(Owner.Execute("assert(Studio.Name == 'Luna')").IsSuccess() &&
            Owner.Execute("assert(Studio.Engine.Ready == true)").IsSuccess() &&
            Owner.Execute("assert(Math.Version == 1)").IsSuccess(),
        "a module requested inside a namespace registers in that scope");
  Check(Registry.Reflection().Generation() == 1,
        "the plan and its module publish exactly one generation");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ModuleRecord Engine = Snapshot.Modules().At(0);
  Check(Engine.Identity() == "studio.engine" && Engine.NamespaceCount() == 1 &&
            Engine.Namespace(0) == "Studio.Engine",
        "module reflection enumerates the scoped namespace it declared");
}

void CheckModuleTypesAreEnumerated() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  const auto Result = Registry.RegisterModule(
      Manifest("studio.ui", "1.0.0"), [](Luna::NamespaceBuilder &Builder) {
        Luna::NamespaceBuilder Interface = Builder.RegisterNamespace("Ui");
        Luna::EnumBuilder<Alignment> Enumeration =
            Interface.RegisterEnum<Alignment>(
                "Alignment", Luna::StableTypeKey("Studio.Ui.Alignment"));
        static_cast<void>(Enumeration.Value("Left", Alignment::Left)
                              .Value("Right", Alignment::Right));
      });
  Check(Result.IsSuccess(), "a module that declares an enumeration loads");
  Check(Owner.Execute("assert(Ui.Alignment.Right == 1)").IsSuccess(),
        "the module's enumeration table publishes with the module");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ModuleRecord Module = Snapshot.Modules().At(0);
  Check(Module.TypeCount() == 1,
        "module reflection enumerates the canonical types the module declared");
  Check(Module.TypeCount() == 1 && Module.TypeName(0) == "Ui.Alignment",
        "a declared enumeration type is named by its canonical reflected name");
  Check(Module.NamespaceCount() == 1 && Module.Namespace(0) == "Ui",
        "the enumeration scope is not counted as a namespace of the module");
}

void CheckCanonicalEnumerationIsOrderIndependent() {
  const auto Load = [](bool ReversedProvision, std::string &Text) {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();

    const Luna::ModuleManifest Math = Manifest("studio.math", "1.0.0");
    const Luna::ModuleManifest Units = Manifest("studio.units", "1.0.0");
    const RecordingModule MathModule{"Math", "Version", 1, nullptr,
                                     "studio.math"};
    const RecordingModule UnitsModule{"Units", "Metres", 2, nullptr,
                                      "studio.units"};

    if (ReversedProvision) {
      static_cast<void>(Registry.ProvideModule(Units, UnitsModule));
      static_cast<void>(Registry.ProvideModule(Math, MathModule));
    } else {
      static_cast<void>(Registry.ProvideModule(Math, MathModule));
      static_cast<void>(Registry.ProvideModule(Units, UnitsModule));
    }

    std::vector<Luna::ModuleDependency> Dependencies;
    std::vector<Luna::ModuleExport> Exports;
    if (ReversedProvision) {
      Dependencies.push_back(Dependency("studio.units", {">=1.0.0"}));
      Dependencies.push_back(Dependency("studio.math", {">=1.0.0"}));
      Exports.push_back(Exported(Luna::SymbolKind::Constant, "Engine.Ready"));
      Exports.push_back(Exported(Luna::SymbolKind::Namespace, "Engine"));
    } else {
      Dependencies.push_back(Dependency("studio.math", {">=1.0.0"}));
      Dependencies.push_back(Dependency("studio.units", {">=1.0.0"}));
      Exports.push_back(Exported(Luna::SymbolKind::Namespace, "Engine"));
      Exports.push_back(Exported(Luna::SymbolKind::Constant, "Engine.Ready"));
    }

    static_cast<void>(Registry.RegisterModule(
        Manifest("studio.engine", "1.0.0", std::move(Dependencies),
                 std::move(Exports)),
        RecordingModule{"Engine", "Ready", 3, nullptr, "studio.engine"}));

    const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
    const Luna::ModuleRecordRange Modules = Snapshot.Modules();
    for (std::size_t Index = 0; Index < Modules.Size(); ++Index) {
      const Luna::ModuleRecord Module = Modules.At(Index);
      Text.append(Module.Identity()).append("@").append(Module.Version());
      for (std::size_t Position = 0; Position < Module.DependencyCount();
           ++Position) {
        const Luna::ModuleDependencyRecord Declared =
            Module.Dependency(Position);
        Text.append("|dep:").append(Declared.Identity()).append("=");
        Text.append(Declared.Version()).append("(");
        Text.append(Declared.Constraints()).append(")");
      }
      for (std::size_t Position = 0; Position < Module.ExportCount();
           ++Position) {
        const Luna::ModuleExportRecord Declared = Module.Export(Position);
        Text.append("|exp:").append(Declared.Name());
      }
      for (std::size_t Position = 0; Position < Module.NamespaceCount();
           ++Position)
        Text.append("|ns:").append(Module.Namespace(Position));
      Text.append(";");
    }
  };

  std::string Forward;
  std::string Reversed;
  Load(false, Forward);
  Load(true, Reversed);
  Check(!Forward.empty() && Forward == Reversed,
        "module enumeration is identical whatever order the definitions and "
        "declarations were written in");
}

// A failed load must leave Luna's private namespace ownership, its module
// registry, and its available definitions exactly as they were. An ordinary
// query taken while the load is still in flight must observe only the committed
// model, never the declarations the open attempt staged.
void CheckFailedLoadPreservesOwnershipAndQueryIsolation() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Check(Studio.Commit().IsSuccess(),
        "one namespace is committed before the failing load");
  const std::size_t Owned = Hooks::NamespaceOwnershipCount(Owner);
  const std::uint64_t Published = Registry.Reflection().Generation();

  std::uint64_t Observed = 0;
  bool SawPending = false;
  const auto Result = Registry.RegisterModule(
      Manifest("studio.engine", "1.0.0"),
      [&Registry, &Observed, &SawPending](Luna::NamespaceBuilder &Builder) {
        Luna::NamespaceBuilder Reopened = Builder.RegisterNamespace("Studio");
        Luna::NamespaceBuilder Engine = Reopened.RegisterNamespace("Engine");
        static_cast<void>(Engine.RegisterConstant("Ready", true));
        static_cast<void>(Engine.Commit());

        const Luna::ReflectionSnapshot Pending = Registry.Reflection();
        Observed = Pending.Generation();
        SawPending = Pending.Find("Studio.Engine").IsValid() ||
                     Pending.Find("studio.engine").IsValid();
        throw std::runtime_error("the module callback failed after staging");
      });

  Check(!Result.IsSuccess(), "the failing module load reports one failure");
  Check(Observed == Published && !SawPending,
        "an ordinary query never observes the pending declarations of an open "
        "load");
  Check(Hooks::NamespaceOwnershipCount(Owner) == Owned,
        "a failed load records no namespace ownership of its own");
  Check(Hooks::NamespaceIsOwned(Owner, "Studio") &&
            !Hooks::NamespaceIsOwned(Owner, "Studio.Engine"),
        "the committed namespace survives and the staged one never appears");
  Check(Registry.Reflection().Generation() == Published,
        "a failed load publishes no reflection generation");
  Check(Hooks::LoadedModuleCount(Owner) == 0 &&
            Hooks::AvailableModuleCount(Owner) == 0,
        "a failed load leaves the module registry and availability untouched");
  Check(PathKind(Owner, "Studio") == "table" &&
            Owner.Execute("assert(Studio.Engine == nil)").IsSuccess(),
        "the staged nested namespace never reached the virtual machine");
  Check(StackDepth(Owner) == EntryDepth,
        "a failed load restores the exact entry stack depth");

  // The same module loads afterwards and reopens the committed namespace.
  const auto Recovered = Registry.RegisterModule(
      Manifest("studio.engine", "1.0.0"), [](Luna::NamespaceBuilder &Builder) {
        Luna::NamespaceBuilder Reopened = Builder.RegisterNamespace("Studio");
        Luna::NamespaceBuilder Engine = Reopened.RegisterNamespace("Engine");
        static_cast<void>(Engine.RegisterConstant("Ready", true));
      });
  Check(Recovered.IsSuccess(),
        "the State recovers and the module reopens the committed namespace");
  Check(Owner.Execute("assert(Studio.Engine.Ready == true)").IsSuccess(),
        "the recovered load publishes into the reopened namespace");
  Check(Hooks::NamespaceOwnershipCount(Owner) == Owned + 1,
        "reopening a committed namespace records only the new nested one");
  Check(Hooks::LoadedModuleCount(Owner) == 1,
        "the recovered load records exactly one loaded module");
}

void CheckProvidedDefinitionsAreValidated() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const RecordingModule Configure{"Math", "Version", 1, nullptr, "studio.math"};

  const auto Invalid =
      Registry.ProvideModule(Luna::ModuleManifest(), Configure);
  Check(!Invalid.IsSuccess() && Invalid.Diagnostic() &&
            Invalid.Diagnostic()->Category() == Luna::ErrorCategory::Internal,
        "an invalid manifest cannot become available");
  Check(Hooks::AvailableModuleCount(Owner) == 0,
        "a rejected definition adds no availability");

  const Luna::ModuleManifest Math = Manifest("studio.math", "1.0.0");
  Check(Registry.ProvideModule(Math, Configure).IsSuccess(),
        "one definition becomes available");
  Check(Registry.ProvideModule(Math, Configure).IsSuccess(),
        "providing an identical definition again is idempotent");
  Check(Hooks::AvailableModuleCount(Owner) == 1,
        "an idempotent provision adds no second definition");

  const auto Conflicting = Registry.ProvideModule(
      Manifest("studio.math", "1.0.0", {},
               {Exported(Luna::SymbolKind::Namespace, "Math")}),
      Configure);
  Check(!Conflicting.IsSuccess() && Conflicting.Diagnostic() &&
            Conflicting.Diagnostic()->Category() ==
                Luna::ErrorCategory::DuplicateGlobalName,
        "an unequal definition of the same identity and version conflicts");
  Check(Hooks::AvailableModuleCount(Owner) == 1,
        "a conflicting provision replaces nothing");
}

} // namespace

int RunModuleLoadingTests() {
  FailureCount = 0;
  CheckOneProvidedModuleLoadsAtomically();
  CheckDependencyGraphRunsInOneTransaction();
  CheckRepeatedLoadIsIdempotentAndConflictsAreRejected();
  CheckUnresolvableGraphPreservesThePreLoadState();
  CheckNestedFailurePoisonsTheWholeLoad();
  CheckCallbackExceptionsAreContained();
  CheckInjectedFailuresLeaveNoModuleBehind();
  CheckLifecycleRejectionsNameTheModule();
  CheckModuleRequestedInsideANamespacePlan();
  CheckModuleTypesAreEnumerated();
  CheckCanonicalEnumerationIsOrderIndependent();
  CheckFailedLoadPreservesOwnershipAndQueryIsolation();
  CheckProvidedDefinitionsAreValidated();
  return FailureCount == 0 ? 0 : 1;
}
