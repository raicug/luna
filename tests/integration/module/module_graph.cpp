// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/core/results/execution_result.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>

#include "state/testing/test_hooks.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
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
  std::cerr << "module graph integration check failed: " << Description << '\n';
}

[[nodiscard]] Luna::SemanticVersion Version(std::string_view Text) {
  const auto Parsed = Luna::SemanticVersion::TryParse(Text);
  return Parsed ? *Parsed : Luna::SemanticVersion();
}

[[nodiscard]] Luna::ModuleDependency Dependency(std::string Identity,
                                                std::string_view Constraint) {
  Luna::ModuleDependency Declared;
  Declared.Identity = std::move(Identity);
  if (const auto Parsed = Luna::VersionConstraint::TryParse(Constraint))
    Declared.Constraints.push_back(*Parsed);
  return Declared;
}

[[nodiscard]] Luna::ModuleManifest
Manifest(std::string Identity, std::string_view VersionText,
         std::vector<Luna::ModuleDependency> Dependencies = {}) {
  auto Created = Luna::ModuleManifest::TryCreate(
      std::move(Identity), Version(VersionText), std::move(Dependencies),
      std::string(), {});
  return Created ? std::move(*Created) : Luna::ModuleManifest();
}

[[nodiscard]] int StackDepth(const Luna::State &Owner) {
  const auto Depth = Hooks::ObserveRootStackDepth(Owner);
  return Depth ? *Depth : -1;
}

void ConfigureUnits(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Units = Builder.RegisterNamespace("Units");
  static_cast<void>(Units.RegisterConstant("Metre", 1));
}

void ConfigurePhysics(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Physics = Builder.RegisterNamespace("Physics");
  static_cast<void>(Physics.RegisterConstant("Gravity", 10));
  Luna::NamespaceBuilder Solver = Physics.RegisterNamespace("Solver");
  static_cast<void>(Solver.RegisterConstant("Iterations", 4));
}

void CheckLoadedGraphRunsThroughTheVirtualMachine() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  Check(
      Registry.ProvideModule(Manifest("studio.units", "1.0.0"), ConfigureUnits)
          .IsSuccess(),
      "the dependency definition becomes available");

  const auto Result =
      Registry.RegisterModule(Manifest("studio.physics", "1.4.2",
                                       {Dependency("studio.units", ">=1.0.0")}),
                              ConfigurePhysics);
  Check(Result.IsSuccess(), "the module graph loads");

  const Luna::ExecutionResult Script =
      Owner.Execute("assert(Units.Metre == 1)\n"
                    "assert(Physics.Gravity == 10)\n"
                    "assert(Physics.Solver.Iterations == 4)\n"
                    "return Physics.Gravity + Units.Metre");
  Check(Script.IsSuccess(), "every published module symbol is readable");
  Check(StackDepth(Owner) == EntryDepth,
        "executing after a module load leaves the stack exactly balanced");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  Check(Snapshot.Modules().Size() == 2,
        "both modules of the graph are reflected");
  const Luna::ModuleRecord Physics = Snapshot.Find("studio.physics").Module();
  Check(Physics.Identity() == "studio.physics" && Physics.Version() == "1.4.2",
        "the module symbol names its own identity and version as provenance");
  Check(Snapshot.Find("Physics.Solver").Module().Identity() ==
                "studio.physics" &&
            Snapshot.Find("Units.Metre").Module().Version() == "1.0.0",
        "every declaration a module load contributed names the module and "
        "version it came from");
  const Luna::ModuleRecord Loaded = Snapshot.Modules().At(0);
  Check(Loaded.Identity() == "studio.physics" && Loaded.Version() == "1.4.2",
        "module enumeration reports the requested module first");
  Check(Loaded.NamespaceCount() == 2 && Loaded.Namespace(0) == "Physics" &&
            Loaded.Namespace(1) == "Physics.Solver",
        "module reflection enumerates nested namespaces canonically");
  Check(Loaded.DependencyCount() == 1 &&
            Loaded.Dependency(0).Version() == "1.0.0",
        "module reflection reports the resolved dependency version");
}

void CheckFailedGraphLeavesTheVirtualMachineUntouched() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  Check(Registry.Register("Existing", [] { return 3; }).IsSuccess(),
        "a committed callable exists before the failing load");
  Check(
      Registry.ProvideModule(Manifest("studio.units", "1.0.0"), ConfigureUnits)
          .IsSuccess(),
      "the dependency definition becomes available");
  const std::uint64_t Published = Registry.Reflection().Generation();

  const auto Result = Registry.RegisterModule(
      Manifest("studio.physics", "1.0.0",
               {Dependency("studio.units", ">=1.0.0")}),
      [](Luna::NamespaceBuilder &Builder) {
        ConfigurePhysics(Builder);
        throw std::runtime_error("physics module failed halfway");
      });
  Check(!Result.IsSuccess(), "a throwing module callback fails the load");

  Check(Owner.Execute("assert(Units == nil)").IsSuccess(),
        "the dependency published nothing into the virtual machine");
  Check(Owner.Execute("assert(Physics == nil)").IsSuccess(),
        "the requested module published nothing into the virtual machine");
  Check(Owner.Execute("assert(Existing() == 3)").IsSuccess(),
        "the pre-load callable still runs");
  Check(Registry.Reflection().Generation() == Published,
        "no reflection generation was published");
  Check(Hooks::LoadedModuleCount(Owner) == 0,
        "no module of the graph is recorded as loaded");
  Check(StackDepth(Owner) == EntryDepth,
        "a failed load restores the exact entry stack depth");

  Check(Registry
            .RegisterModule(Manifest("studio.physics", "1.0.0",
                                     {Dependency("studio.units", ">=1.0.0")}),
                            ConfigurePhysics)
            .IsSuccess(),
        "the same graph loads once its callback stops throwing");
  Check(Owner.Execute("assert(Physics.Solver.Iterations == 4)").IsSuccess() &&
            Owner.Execute("assert(Units.Metre == 1)").IsSuccess(),
        "the recovered load publishes the whole graph");
  Check(Hooks::LoadedModuleCount(Owner) == 2,
        "the recovered load records both modules");
}

void CheckModulesStayIsolatedBetweenStates() {
  Luna::State First;
  Luna::State Second;
  Luna::BindingRegistry FirstRegistry = First.Bindings();
  Luna::BindingRegistry SecondRegistry = Second.Bindings();

  Check(FirstRegistry
            .ProvideModule(Manifest("studio.units", "1.0.0"), ConfigureUnits)
            .IsSuccess(),
        "one State receives the definition");
  Check(FirstRegistry
            .RegisterModule(Manifest("studio.physics", "1.0.0",
                                     {Dependency("studio.units", ">=1.0.0")}),
                            ConfigurePhysics)
            .IsSuccess(),
        "the first State loads the graph");

  Check(Hooks::LoadedModuleCount(Second) == 0 &&
            Hooks::AvailableModuleCount(Second) == 0,
        "module state never leaks into another State");
  Check(Second.Execute("assert(Physics == nil)").IsSuccess(),
        "another State observes none of the loaded symbols");

  const auto Missing = SecondRegistry.RegisterModule(
      Manifest("studio.physics", "1.0.0",
               {Dependency("studio.units", ">=1.0.0")}),
      ConfigurePhysics);
  Check(!Missing.IsSuccess(),
        "each State resolves only its own available definitions");
}

} // namespace

int RunModuleGraphIntegrationTests() {
  FailureCount = 0;
  CheckLoadedGraphRunsThroughTheVirtualMachine();
  CheckFailedGraphLeavesTheVirtualMachineUntouched();
  CheckModulesStayIsolatedBetweenStates();
  return FailureCount == 0 ? 0 : 1;
}
