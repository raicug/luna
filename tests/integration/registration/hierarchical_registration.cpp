// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/enum_builder.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/core/results/execution_result.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/test_hooks.hpp"

#include <cstddef>
#include <cstdint>
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
  std::cerr << "hierarchical registration integration check failed: "
            << Description << '\n';
}

enum class Alignment { Left = 0, Center = 1, Right = 2 };
enum class Access : int { None = 0, Read = 1, Write = 2, Execute = 4 };
enum class Narrow : signed char { Small = 1 };
enum class Unit { Metre = 0, Second = 1 };
enum Legacy { LegacyFirst = 1, LegacySecond = 2 };

[[nodiscard]] Luna::StableTypeKey AlignmentKey() {
  return Luna::StableTypeKey("Studio.Alignment");
}

[[nodiscard]] Luna::StableTypeKey AccessKey() {
  return Luna::StableTypeKey("Studio.Access");
}

[[nodiscard]] int StackDepth(const Luna::State &Owner) {
  const auto Depth = Hooks::ObserveRootStackDepth(Owner);
  return Depth ? *Depth : -1;
}

[[nodiscard]] std::string PathKind(Luna::State &Owner,
                                   const std::string &Path) {
  const auto Kind = Hooks::ObserveVmPathValueKind(Owner, Path);
  return Kind ? *Kind : std::string("<unavailable>");
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

[[nodiscard]] Luna::RegistrationResult
CommitRepresentativeHierarchy(Luna::BindingRegistry &Registry) {
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  static_cast<void>(Studio.RegisterConstant("Name", "Luna"));

  Luna::NamespaceBuilder Ui = Studio.RegisterNamespace("Ui");
  static_cast<void>(Ui.RegisterConstant("Scale", 1.5));

  Luna::NamespaceBuilder Layout = Ui.RegisterNamespace("Layout");
  static_cast<void>(Layout.RegisterConstant("Columns", 12));
  static_cast<void>(Layout.RegisterConstant("Fluid", true));

  Luna::EnumBuilder<Alignment> Alignments =
      Ui.RegisterEnum<Alignment>("Alignment", AlignmentKey());
  Luna::EnumBuilder<Alignment> &StagedAlignments =
      Alignments.Value("Left", Alignment::Left)
          .Value("Center", Alignment::Center)
          .Value("Right", Alignment::Right)
          .Alias("Start", "Left")
          .Documentation("Horizontal alignment.")
          .Documentation("Center", "Centered content.")
          .Attribute("Center", "Default", "true");
  static_cast<void>(StagedAlignments.QualifiedName());

  Luna::EnumBuilder<Access> Permissions =
      Ui.RegisterEnum<Access>("Access", AccessKey());
  Luna::EnumBuilder<Access> &StagedPermissions =
      Permissions.Value("None", Access::None)
          .Value("Read", Access::Read)
          .Value("Write", Access::Write)
          .Bitflags(static_cast<std::int64_t>(3));
  static_cast<void>(StagedPermissions.QualifiedName());

  Luna::EnumBuilder<Legacy> Legacies = Layout.RegisterEnum<Legacy>(
      "Legacy", Luna::StableTypeKey("Studio.Legacy"));
  Luna::EnumBuilder<Legacy> &StagedLegacies = Legacies.AllowUnscoped()
                                                  .Value("First", LegacyFirst)
                                                  .Value("Second", LegacySecond)
                                                  .Alias("Primary", "First");
  static_cast<void>(StagedLegacies.QualifiedName());

  return Studio.Commit();
}

void ConfigureUnits(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Units = Builder.RegisterNamespace("Units");
  static_cast<void>(Units.RegisterConstant("Metre", 1));
  Luna::NamespaceBuilder Scale = Units.RegisterNamespace("Scale");
  static_cast<void>(Scale.RegisterConstant("Factor", 2.5));
  Luna::EnumBuilder<Unit> Kinds =
      Units.RegisterEnum<Unit>("Kind", Luna::StableTypeKey("Studio.Unit"));
  static_cast<void>(
      Kinds.Value("Metre", Unit::Metre).Value("Second", Unit::Second));
}

void ConfigurePhysics(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Physics = Builder.RegisterNamespace("Physics");
  static_cast<void>(Physics.RegisterConstant("Gravity", 10));
  Luna::NamespaceBuilder Solver = Physics.RegisterNamespace("Solver");
  static_cast<void>(Solver.RegisterConstant("Iterations", 4));
}

void CheckRepresentativeHierarchyRunsThroughTheVirtualMachine() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  int Invocations = 0;
  Check(Registry
            .Register("Combine",
                      [&Invocations](int Left, int Right) {
                        ++Invocations;
                        return Left + Right;
                      })
            .IsSuccess(),
        "a root callable registers alongside the hierarchy");

  Check(CommitRepresentativeHierarchy(Registry).IsSuccess(),
        "one plan publishes the whole representative hierarchy");
  Check(StackDepth(Owner) == EntryDepth,
        "publishing the hierarchy restores the exact entry stack depth");

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::NamespaceBuilder Ui = Studio.RegisterNamespace("Ui");
  static_cast<void>(Ui.RegisterConstant("DefaultAlignment", Alignment::Center,
                                        AlignmentKey()));
  static_cast<void>(
      Ui.RegisterConstant("ReadWrite", static_cast<Access>(3), AccessKey()));
  Check(Studio.Commit().IsSuccess(),
        "reopening the published namespaces adds enumeration-typed constants");

  const Luna::ExecutionResult Script = Owner.Execute(
      "assert(Studio.Name == 'Luna')\n"
      "assert(Studio.Ui.Scale == 1.5)\n"
      "assert(Studio.Ui.Layout.Columns == 12)\n"
      "assert(Studio.Ui.Layout.Fluid == true)\n"
      "assert(Studio.Ui.Alignment.Center == 1)\n"
      "assert(Studio.Ui.Alignment.Start == Studio.Ui.Alignment.Left)\n"
      "assert(Studio.Ui.Layout.Legacy.Primary == 1)\n"
      "assert(Studio.Ui.Layout.Legacy.Second == 2)\n"
      "assert(Studio.Ui.Access.Read == 1 and Studio.Ui.Access.Write == 2)\n"
      "assert(Studio.Ui.ReadWrite ==\n"
      "       Studio.Ui.Access.Read + Studio.Ui.Access.Write)\n"
      "assert(Studio.Ui.DefaultAlignment == Studio.Ui.Alignment.Center)\n"
      "assert(Combine(Studio.Ui.Layout.Columns,\n"
      "               Studio.Ui.Alignment.Right) == 14)\n");
  Check(Script.IsSuccess(),
        "every published constant, enumerator, and alias is readable and the "
        "registered callable is invocable from real Luau source");
  Check(Invocations == 1,
        "the native callable ran exactly once for the one script call");
  Check(StackDepth(Owner) == EntryDepth,
        "executing against the hierarchy leaves the stack exactly balanced");

  Check(!Owner.Execute("Studio.Ui.Alignment.Left = 5").IsSuccess() &&
            !Owner.Execute("Studio.Ui.Layout.Legacy.First = 5").IsSuccess(),
        "a script write to any published enumeration table fails");
  Check(Owner.Execute("assert(Studio.Ui.Alignment.Left == 0)").IsSuccess(),
        "a refused write leaves the enumeration table unchanged");
  Check(StackDepth(Owner) == EntryDepth,
        "a refused script write restores the exact entry stack depth");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  Check(Snapshot.Find("Studio").Kind() == Luna::SymbolKind::Namespace &&
            Snapshot.Find("Studio.Ui").Kind() == Luna::SymbolKind::Namespace &&
            Snapshot.Find("Studio.Ui.Layout").Kind() ==
                Luna::SymbolKind::Namespace,
        "every nested namespace of the hierarchy is reflected");
  Check(Snapshot.Find("Studio.Ui.Layout").Scope().Owner() ==
            Snapshot.Find("Studio.Ui").Id(),
        "a nested namespace is reflected inside its parent scope");
  Check(Snapshot.Find("Studio.Ui.Layout.Columns").Kind() ==
                Luna::SymbolKind::Constant &&
            Snapshot.Find("Studio.Ui.Layout.Columns").ValueText() == "12",
        "a deeply nested constant reflects its scope and canonical value");
  Check(Snapshot.Find("Studio.Ui.Alignment").Documentation() ==
            "Horizontal alignment.",
        "the scoped enumeration reflects its documentation");
  Check(Snapshot.Find("Studio.Ui.Alignment.Start").Kind() ==
                Luna::SymbolKind::EnumeratorAlias &&
            Snapshot.Find("Studio.Ui.Alignment.Start").Declaration() ==
                Snapshot.Find("Studio.Ui.Alignment.Left").Id(),
        "the alias reflects its canonical enumerator");
  Check(Snapshot.Find("Studio.Ui.DefaultAlignment").Type() ==
            Snapshot.Find("Studio.Ui.Alignment").Type(),
        "an enumeration-typed constant keeps the enumeration's type identity");
  Check(Snapshot.Find("Studio.Ui.Layout.Legacy").Kind() ==
            Luna::SymbolKind::Enumeration,
        "the opted-in unscoped enumeration is reflected as an enumeration");

  const Luna::ReflectionRecordRange Members =
      Snapshot.Symbols(Luna::ScopeId(Snapshot.Find("Studio.Ui").Id()));
  Check(Members.Size() == 6, "the namespace scope holds all six declarations");
  Check(Members.At(0).Name() == "Access" &&
            Members.At(1).Name() == "Alignment" &&
            Members.At(2).Name() == "DefaultAlignment" &&
            Members.At(3).Name() == "Layout" &&
            Members.At(4).Name() == "ReadWrite" &&
            Members.At(5).Name() == "Scale",
        "namespace members are enumerated in canonical name order");
}

void CheckModuleGraphRegistersIntoNestedScopes() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  int UnitCallbacks = 0;
  int PhysicsCallbacks = 0;
  const auto Units = [&UnitCallbacks](Luna::NamespaceBuilder &Builder) {
    ++UnitCallbacks;
    ConfigureUnits(Builder);
  };
  const auto Physics = [&PhysicsCallbacks](Luna::NamespaceBuilder &Builder) {
    ++PhysicsCallbacks;
    ConfigurePhysics(Builder);
  };

  Check(Registry.ProvideModule(Manifest("studio.units", "1.0.0"), Units)
            .IsSuccess(),
        "the dependency definition becomes available");
  Check(Registry.ProvideModule(Manifest("studio.units", "1.2.0-rc.1"), Units)
            .IsSuccess(),
        "a higher-precedence prerelease definition is also available");
  Check(UnitCallbacks == 0,
        "providing a definition runs no callback and publishes nothing");

  Check(Registry
            .RegisterModule(Manifest("studio.physics", "1.4.2",
                                     {Dependency("studio.units", ">=1.0.0")}),
                            Physics)
            .IsSuccess(),
        "the module graph loads through one transaction");
  Check(UnitCallbacks == 1 && PhysicsCallbacks == 1,
        "each module callback of the graph runs exactly once");
  Check(Hooks::LoadedModuleVersion(Owner, "studio.units") == "1.2.0-rc.1",
        "resolution selects the highest satisfying version by prerelease "
        "precedence");

  Check(Owner
            .Execute("assert(Units.Metre == 1)\n"
                     "assert(Units.Scale.Factor == 2.5)\n"
                     "assert(Units.Kind.Second == 1)\n"
                     "assert(Physics.Gravity == 10)\n"
                     "assert(Physics.Solver.Iterations == 4)\n")
            .IsSuccess(),
        "every module-registered namespace, constant, and enumerator is "
        "readable");
  Check(StackDepth(Owner) == EntryDepth,
        "loading and executing a module graph restores the entry stack depth");

  const std::uint64_t Published = Registry.Reflection().Generation();
  Check(Registry
            .RegisterModule(Manifest("studio.physics", "1.4.2",
                                     {Dependency("studio.units", ">=1.0.0")}),
                            Physics)
            .IsSuccess(),
        "reloading the identical definition succeeds");
  Check(PhysicsCallbacks == 1 && UnitCallbacks == 1,
        "an idempotent reload reruns no callback");
  Check(Registry.Reflection().Generation() == Published,
        "an idempotent reload publishes no new generation");
  Check(Owner.Execute("assert(Physics.Solver.Iterations == 4)").IsSuccess(),
        "an idempotent reload leaves every published symbol intact");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  Check(Snapshot.Modules().Size() == 2,
        "both modules of the graph are reflected");
  Check(Snapshot.Find("Units.Kind.Metre").Kind() ==
            Luna::SymbolKind::Enumerator,
        "a module-registered enumerator is reflected under its module scope");
}

void CheckEveryFailureFamilyLeavesTheStateReusable() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  Check(Registry.Register("Anchor", [] { return 7; }).IsSuccess(),
        "a committed callable anchors every recovery check");
  Check(
      Registry.ProvideModule(Manifest("studio.units", "1.0.0"), ConfigureUnits)
          .IsSuccess(),
      "one dependency definition is available for the module families");

  const auto Refuses = [&](std::string Description,
                           const std::vector<std::string> &AbsentPaths,
                           auto &&Attempt) {
    const std::uint64_t Generation = Registry.Reflection().Generation();
    const std::size_t Owned = Hooks::NamespaceOwnershipCount(Owner);
    const std::size_t Loaded = Hooks::LoadedModuleCount(Owner);

    const Luna::RegistrationResult Result = Attempt();
    Check(!Result.IsSuccess(), Description + " is refused");
    Check(Result.Diagnostic() && !Result.Diagnostic()->Message().empty(),
          Description + " reports a non-empty deterministic diagnostic");
    for (const std::string &Path : AbsentPaths)
      Check(PathKind(Owner, Path) == "absent",
            Description + " installs nothing at " + Path);
    Check(Registry.Reflection().Generation() == Generation,
          Description + " publishes no reflection generation");
    Check(Hooks::NamespaceOwnershipCount(Owner) == Owned,
          Description + " records no namespace ownership");
    Check(Hooks::LoadedModuleCount(Owner) == Loaded,
          Description + " records no loaded module");
    Check(StackDepth(Owner) == EntryDepth,
          Description + " restores the exact entry stack depth");
    Check(Owner.Execute("assert(Anchor() == 7)").IsSuccess(),
          "the State stays usable after " + Description);
    Check(!Hooks::HasActiveTransaction(Owner),
          Description + " leaves no transaction open");
  };

  Check(Owner.Execute("Foreign = { Marker = 3 }").IsSuccess(),
        "the script creates its own table");
  Refuses("a namespace over a script-created table", {}, [&] {
    Luna::NamespaceBuilder Builder = Registry.RegisterNamespace("Foreign");
    return Builder.Commit();
  });
  Check(Owner.Execute("assert(Foreign.Marker == 3)").IsSuccess(),
        "the script's table keeps its contents after the ownership mismatch");

  Refuses("an invalid nested namespace segment", {"Region"}, [&] {
    Luna::NamespaceBuilder Region = Registry.RegisterNamespace("Region");
    Luna::NamespaceBuilder Nested = Region.RegisterNamespace("Not Valid");
    static_cast<void>(Nested.RegisterConstant("Depth", 1));
    return Region.Commit();
  });

  Refuses("one refused constant inside a deep plan",
          {"Zone", "Zone.Inner", "Zone.Inner.Leaf"}, [&] {
            Luna::NamespaceBuilder Zone = Registry.RegisterNamespace("Zone");
            static_cast<void>(Zone.RegisterConstant("Depth", 1));
            Luna::NamespaceBuilder Inner = Zone.RegisterNamespace("Inner");
            Luna::NamespaceBuilder Leaf = Inner.RegisterNamespace("Leaf");
            static_cast<void>(Leaf.RegisterConstant("Columns", 4));
            Luna::EnumBuilder<Alignment> Alignments =
                Inner.RegisterEnum<Alignment>("Alignment", AlignmentKey());
            static_cast<void>(Alignments.Value("Left", Alignment::Left));
            static_cast<void>(Leaf.RegisterConstant("Broken", Alignment::Left));
            return Zone.Commit();
          });

  Refuses("an out-of-range enumerator inside a nested plan",
          {"Bounds", "Bounds.Narrow"}, [&] {
            Luna::NamespaceBuilder Bounds =
                Registry.RegisterNamespace("Bounds");
            Luna::EnumBuilder<Narrow> Narrows = Bounds.RegisterEnum<Narrow>(
                "Narrow", Luna::StableTypeKey("Studio.Narrow"));
            Luna::EnumBuilder<Narrow> &Staged =
                Narrows.Value("Small", Narrow::Small)
                    .Value("TooLarge", static_cast<std::int64_t>(300));
            static_cast<void>(Staged.QualifiedName());
            return Bounds.Commit();
          });

  Refuses("an unscoped enumeration without its opt-in", {"Legacy"}, [&] {
    Luna::EnumBuilder<Legacy> Legacies = Registry.RegisterEnum<Legacy>(
        "Legacy", Luna::StableTypeKey("Studio.Legacy"));
    Luna::EnumBuilder<Legacy> &Staged = Legacies.Value("First", LegacyFirst);
    return Staged.Commit();
  });

  const Luna::StableTypeKey FlagsKey("Studio.Flags");
  Luna::EnumBuilder<Access> Permissions =
      Registry.RegisterEnum<Access>("Flags", FlagsKey);
  Luna::EnumBuilder<Access> &StagedPermissions =
      Permissions.Value("None", Access::None)
          .Value("Read", Access::Read)
          .Value("Write", Access::Write)
          .Bitflags(static_cast<std::int64_t>(3));
  Check(StagedPermissions.Commit().IsSuccess(),
        "the bitflag enumeration publishes before the unsupported-bit family");
  Refuses("a constant carrying an unsupported flag bit", {"All"}, [&] {
    return Registry.RegisterConstant("All", static_cast<Access>(7), FlagsKey);
  });

  Check(
      Registry.ProvideModule(Manifest("studio.left", "1.0.0",
                                      {Dependency("studio.right", ">=1.0.0")}),
                             ConfigurePhysics)
              .IsSuccess() &&
          Registry
              .ProvideModule(Manifest("studio.right", "1.0.0",
                                      {Dependency("studio.left", ">=1.0.0")}),
                             ConfigurePhysics)
              .IsSuccess(),
      "both sides of the cycle are available as definitions");
  Refuses("a cyclic module graph", {"Physics"}, [&] {
    return Registry.RegisterModule(
        Manifest("studio.left", "1.0.0",
                 {Dependency("studio.right", ">=1.0.0")}),
        ConfigurePhysics);
  });

  Refuses("an unsatisfiable dependency constraint", {"Physics"}, [&] {
    return Registry.RegisterModule(
        Manifest("studio.physics", "1.0.0",
                 {Dependency("studio.units", ">=9.0.0")}),
        ConfigurePhysics);
  });

  Refuses("a throwing module callback", {"Physics", "Units"}, [&] {
    return Registry.RegisterModule(
        Manifest("studio.physics", "1.0.0",
                 {Dependency("studio.units", ">=1.0.0")}),
        [](Luna::NamespaceBuilder &Builder) {
          ConfigurePhysics(Builder);
          throw std::runtime_error("the module callback failed halfway");
        });
  });

  Check(Registry
            .RegisterModule(Manifest("studio.physics", "1.0.0",
                                     {Dependency("studio.units", ">=1.0.0")}),
                            ConfigurePhysics)
            .IsSuccess(),
        "the same graph loads once its callback stops throwing");
  Check(Owner.Execute("assert(Physics.Gravity == 10)").IsSuccess(),
        "the recovered load publishes the whole graph");
  Refuses("a different version of a loaded module identity", {}, [&] {
    return Registry.RegisterModule(Manifest("studio.physics", "2.0.0"),
                                   ConfigurePhysics);
  });
  Check(Hooks::LoadedModuleVersion(Owner, "studio.physics") == "1.0.0",
        "a refused replacement leaves the loaded version in place");

  Check(CommitRepresentativeHierarchy(Registry).IsSuccess(),
        "the State still commits the whole hierarchy after every failure");
  Check(Owner
            .Execute("assert(Studio.Ui.Alignment.Start == 0)\n"
                     "assert(Studio.Ui.Layout.Columns == 12)\n"
                     "assert(Anchor() == 7)\n")
            .IsSuccess(),
        "the recovered hierarchy is fully readable and the anchor still runs");
  Check(StackDepth(Owner) == EntryDepth,
        "the recovered State ends at the exact entry stack depth");
}

} // namespace

int RunHierarchicalRegistrationIntegrationTests() {
  FailureCount = 0;
  CheckRepresentativeHierarchyRunsThroughTheVirtualMachine();
  CheckModuleGraphRegistersIntoNestedScopes();
  CheckEveryFailureFamilyLeavesTheStateReusable();
  return FailureCount == 0 ? 0 : 1;
}
