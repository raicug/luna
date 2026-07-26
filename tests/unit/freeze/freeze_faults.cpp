// Focused coverage of the freeze and cache refusal surface.
//
// Three things are checked here that no equivalence property observes directly:
// the precedence a wrong-thread refusal takes over every other refusal on every
// virtual-machine-backed public operation, the determinism and recoverability
// of a refused freeze - a metadata contradiction and an injected
// cache-allocation failure - and the complete matrix of registration and module
// operations a frozen State rejects, including builders that were opened before
// freeze.

// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_member.hpp>
#include <luna/binding/enum_builder.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/core/results/execution_result.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using FaultPoint = Luna::Detail::StateFaultPoint;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "freeze fault check failed: " << Description << '\n';
}

[[nodiscard]] bool FailedWith(const Luna::RegistrationResult &Result,
                              Luna::ErrorCategory Category,
                              std::string_view Fragment) {
  return !Result.IsSuccess() && Result.Diagnostic() != nullptr &&
         Result.Diagnostic()->Category() == Category &&
         Result.Diagnostic()->Message().find(Fragment) != std::string::npos;
}

[[nodiscard]] std::string MessageOf(const Luna::RegistrationResult &Result) {
  return Result.Diagnostic() != nullptr
             ? std::string(Result.Diagnostic()->Message())
             : std::string("<no diagnostic>");
}

// One expected success, whose diagnostic is reported when it unexpectedly
// refuses, so a failing check names the refusal instead of only its intent.
[[nodiscard]] bool Succeeded(const Luna::RegistrationResult &Result) {
  if (Result.IsSuccess())
    return true;
  std::cerr << "  unexpected refusal: " << MessageOf(Result) << '\n';
  return false;
}

// -- the representative model ------------------------------------------------

enum class Mode { Off = 0, On = 1 };

struct Widget final {
  int Charge = 3;
  [[nodiscard]] int Level() const { return Charge * 2; }
};

[[nodiscard]] int Increment(int Value) { return Value + 1; }

[[nodiscard]] Luna::StableTypeKey WidgetKey() {
  return Luna::StableTypeKey("tests.freeze.faults.Widget");
}

[[nodiscard]] Luna::StableTypeKey ModeKey() {
  return Luna::StableTypeKey("tests.freeze.faults.Mode");
}

[[nodiscard]] Luna::ModuleManifest ToolsManifest() {
  const std::optional<Luna::SemanticVersion> Version =
      Luna::SemanticVersion::TryParse("1.0.0");
  const std::optional<Luna::ModuleManifest> Manifest =
      Version ? Luna::ModuleManifest::TryCreate("tests.freeze.tools", *Version,
                                                {}, "", {})
              : std::nullopt;
  return Manifest ? *Manifest : Luna::ModuleManifest();
}

void ConfigureTools(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Tools = Builder.RegisterNamespace("Tools");
  static_cast<void>(Tools.RegisterConstant("Slots", 4));
}

// One fully populated Ready State: an overload-free callable, a nested
// namespace with a constant, a scoped enumeration, a class with a field and one
// explicitly lazy property, and one loaded module.
[[nodiscard]] Luna::RegistrationResult RegisterModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  if (auto Function = Registry.Register("Increment", &Increment);
      !Function.IsSuccess())
    return Function;

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  static_cast<void>(Studio.RegisterConstant("Version", 7));
  Luna::EnumBuilder<Mode> Modes = Studio.RegisterEnum<Mode>("Mode", ModeKey());
  static_cast<void>(
      Modes.Value("Off", Mode::Off).Value("On", Mode::On).QualifiedName());
  Luna::ClassBuilder<Widget> Class =
      Studio.RegisterClass<Widget>("Widget", WidgetKey());
  static_cast<void>(
      Class.Constructor<>()
          .Field("Charge", &Widget::Charge)
          .Property("Level", Luna::PropertyPolicy::Lazy(), &Widget::Level)
          .QualifiedName());
  if (auto Committed = Studio.Commit(); !Committed.IsSuccess())
    return Committed;
  return Registry.RegisterModule(ToolsManifest(), &ConfigureTools);
}

// Everything one foreign thread attempted, so the refusals are compared on the
// owner thread after that thread has joined.
struct ForeignOutcome final {
  Luna::RegistrationResult Duplicate = Luna::RegistrationResult::Success();
  Luna::RegistrationResult Malformed = Luna::RegistrationResult::Success();
  Luna::RegistrationResult Function = Luna::RegistrationResult::Success();
  Luna::RegistrationResult Constant = Luna::RegistrationResult::Success();
  Luna::RegistrationResult Scope = Luna::RegistrationResult::Success();
  Luna::RegistrationResult Enumeration = Luna::RegistrationResult::Success();
  Luna::RegistrationResult Class = Luna::RegistrationResult::Success();
  Luna::RegistrationResult Provided = Luna::RegistrationResult::Success();
  Luna::RegistrationResult Loaded = Luna::RegistrationResult::Success();
  Luna::RegistrationResult Frozen = Luna::RegistrationResult::Success();

  bool ExecutionRefused = false;
  Luna::ErrorCategory ExecutionCategory = Luna::ErrorCategory::Internal;
  std::string ExecutionMessage;

  std::vector<std::string> SnapshotNames;
  std::uint64_t SnapshotGeneration = 0;
};

[[nodiscard]] std::vector<std::string>
OrderedNames(const Luna::ReflectionSnapshot &Snapshot) {
  const Luna::ReflectionRecordRange Symbols = Snapshot.Symbols();
  std::vector<std::string> Names;
  Names.reserve(Symbols.Size());
  for (std::size_t Index = 0; Index < Symbols.Size(); ++Index)
    Names.push_back(std::string(Symbols.At(Index).QualifiedName()));
  return Names;
}

void AttemptEverythingFromForeignThread(Luna::State &Owner,
                                        ForeignOutcome &Observed) {
  Luna::BindingRegistry Registry = Owner.Bindings();

  // A name that is already registered and a name that is malformed: the
  // established root-scope spelling validates the identifier first, so the
  // malformed name reports its own grammar failure, while the well-formed
  // duplicate reports the thread instead of the duplicate.
  Observed.Duplicate = Registry.Register("Increment", &Increment);
  Observed.Malformed = Registry.Register("9Invalid", &Increment);

  Observed.Function = Registry.RegisterFunction("Later", &Increment);
  Observed.Constant = Registry.RegisterConstant("Limit", 3);

  Luna::NamespaceBuilder Scope = Registry.RegisterNamespace("Foreign");
  static_cast<void>(Scope.RegisterConstant("Slots", 1));
  Observed.Scope = Scope.Commit();

  Luna::EnumBuilder<Mode> Modes = Registry.RegisterEnum<Mode>(
      "ForeignMode", Luna::StableTypeKey("tests.freeze.faults.ForeignMode"));
  Observed.Enumeration = Modes.Value("Off", Mode::Off).Commit();

  Luna::ClassBuilder<Widget> Class = Registry.RegisterClass<Widget>(
      "ForeignWidget", Luna::StableTypeKey("tests.freeze.faults.Foreign"));
  Observed.Class = Class.Field("Charge", &Widget::Charge).Commit();

  Observed.Provided = Registry.ProvideModule(ToolsManifest(), &ConfigureTools);
  Observed.Loaded = Registry.RegisterModule(ToolsManifest(), &ConfigureTools);
  Observed.Frozen = Registry.Freeze();

  const Luna::ExecutionResult Executed = Owner.Execute("return Increment(1)");
  Observed.ExecutionRefused = !Executed.IsSuccess();
  if (Executed.Diagnostic() != nullptr) {
    Observed.ExecutionCategory = Executed.Diagnostic()->Category();
    Observed.ExecutionMessage = std::string(Executed.Diagnostic()->Message());
  }

  // Owning immutable reflection is readable from any thread, which is exactly
  // what makes it safe to refuse everything above.
  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  Observed.SnapshotNames = OrderedNames(Snapshot);
  Observed.SnapshotGeneration = Snapshot.Generation();
}

void CheckWrongThreadRefusalPrecedesEveryOtherRefusal() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Succeeded(RegisterModel(Owner)), "the representative model registers");

  const auto Generations = Hooks::GenerationsOf(Owner);
  const std::uint64_t Reflected = Hooks::ReflectionGeneration(Owner);
  const std::size_t Bindings = Hooks::BindingCount(Owner);
  const std::size_t Namespaces = Hooks::NamespaceOwnershipCount(Owner);
  const std::size_t Classes = Hooks::RegisteredClassCount(Owner);
  const std::size_t Modules = Hooks::LoadedModuleCount(Owner);
  const std::size_t Definitions = Hooks::AvailableModuleCount(Owner);
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  const Luna::ReflectionSnapshot Owned = Registry.Reflection();

  ForeignOutcome Observed;
  std::thread Foreign([&Owner, &Observed] {
    AttemptEverythingFromForeignThread(Owner, Observed);
  });
  Foreign.join();

  const Luna::ErrorCategory NotReady = Luna::ErrorCategory::StateNotReady;
  Check(FailedWith(Observed.Duplicate, NotReady, "owner thread"),
        "a well-formed duplicate name reports the foreign thread, not the "
        "duplicate");
  Check(FailedWith(Observed.Malformed, Luna::ErrorCategory::InvalidGlobalName,
                   "Invalid global name"),
        "the established root-scope spelling keeps validating its identifier "
        "before the thread");
  Check(FailedWith(Observed.Function, NotReady, "owner thread"),
        "explicit function registration is refused on a foreign thread");
  Check(FailedWith(Observed.Constant, NotReady, "owner thread"),
        "constant registration is refused on a foreign thread");
  Check(FailedWith(Observed.Scope, NotReady, "owner thread"),
        "a namespace plan is refused on a foreign thread");
  Check(FailedWith(Observed.Enumeration, NotReady, "owner thread"),
        "an enumeration plan is refused on a foreign thread");
  Check(FailedWith(Observed.Class, NotReady, "owner thread"),
        "a class plan is refused on a foreign thread");
  Check(FailedWith(Observed.Provided, NotReady, "owner thread"),
        "recording a module definition is refused on a foreign thread");
  Check(FailedWith(Observed.Loaded, NotReady, "owner thread"),
        "loading a module is refused on a foreign thread");
  Check(FailedWith(Observed.Frozen, NotReady, "owner thread"),
        "freeze is refused on a foreign thread");
  Check(Observed.ExecutionRefused && Observed.ExecutionCategory == NotReady &&
            Observed.ExecutionMessage.find("owner thread") != std::string::npos,
        "source execution is refused on a foreign thread");

  Check(Observed.SnapshotNames == OrderedNames(Owned) &&
            Observed.SnapshotGeneration == Owned.Generation(),
        "an owning reflection snapshot reads identically from another thread");

  Check(!Hooks::IsFrozen(Owner) && !Hooks::ObserveFreezeCache(Owner).Published,
        "no foreign attempt froze the State or published a cache");
  Check(Hooks::GenerationsOf(Owner) == Generations &&
            Hooks::ReflectionGeneration(Owner) == Reflected &&
            Hooks::BindingCount(Owner) == Bindings &&
            Hooks::NamespaceOwnershipCount(Owner) == Namespaces &&
            Hooks::RegisteredClassCount(Owner) == Classes &&
            Hooks::LoadedModuleCount(Owner) == Modules &&
            Hooks::AvailableModuleCount(Owner) == Definitions,
        "no foreign attempt changed one committed generation or store");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth &&
            !Hooks::BindingIsCommitted(Owner, "Later") &&
            !Hooks::NamespaceIsOwned(Owner, "Foreign"),
        "every foreign refusal happened before any virtual-machine mutation");

  Check(Registry.RegisterFunction("Later", &Increment).IsSuccess() &&
            Owner.Execute("assert(Later(1) == 2)").IsSuccess() &&
            Registry.Freeze().IsSuccess(),
        "the owner thread registers, executes, and freezes afterwards");
}

// -- refused freeze ----------------------------------------------------------

void CheckRefusedFreezeIsDeterministicAndRecoverable() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Succeeded(RegisterModel(Owner)), "the representative model registers");

  const auto Generations = Hooks::GenerationsOf(Owner);
  const std::uint64_t Reflected = Hooks::ReflectionGeneration(Owner);
  const std::size_t Bindings = Hooks::BindingCount(Owner);
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  // An injected cache-allocation failure: every attempt reports the same
  // deterministic result, publishes nothing, and leaves the Ready State exactly
  // as it was.
  Hooks::InjectFault(Owner, FaultPoint::FreezePreparation, 2);
  const Luna::RegistrationResult First = Registry.Freeze();
  const Luna::RegistrationResult Second = Registry.Freeze();
  Check(
      FailedWith(First, Luna::ErrorCategory::Internal, "allocation_failure") &&
          MessageOf(First) == MessageOf(Second),
      "every injected cache-allocation failure reports one identical result");
  Check(Hooks::PendingFaults(Owner, FaultPoint::FreezePreparation) == 0,
        "both injected faults were consumed by the two attempts");
  Check(!Hooks::IsFrozen(Owner) && !Hooks::ObserveFreezeCache(Owner).Published,
        "a refused freeze publishes neither caches nor the frozen phase");
  Check(Owner.IsReady() && Hooks::GenerationsOf(Owner) == Generations &&
            Hooks::ReflectionGeneration(Owner) == Reflected &&
            Hooks::BindingCount(Owner) == Bindings &&
            Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "a refused freeze leaves the Ready State, its metadata, and its stack "
        "unchanged");
  Check(Owner.Execute("assert(Increment(1) == 2)").IsSuccess(),
        "the State keeps executing after a refused freeze");

  // The recovered freeze publishes exactly the cache a State that never failed
  // publishes, so a failed attempt leaves no trace in the cache contents.
  Check(Succeeded(Registry.Freeze()),
        "freeze succeeds once the transient failure is gone");
  const Luna::Detail::FreezeCacheObservation Recovered =
      Hooks::ObserveFreezeCache(Owner);

  Luna::State Control;
  Luna::BindingRegistry ControlRegistry = Control.Bindings();
  Check(Succeeded(RegisterModel(Control)), "the control State registers");
  Check(Succeeded(ControlRegistry.Freeze()),
        "the control State freezes on its first attempt");
  const Luna::Detail::FreezeCacheObservation Reference =
      Hooks::ObserveFreezeCache(Control);

  Check(Recovered.Published && Reference.Published &&
            Recovered.LookupDetails == Reference.LookupDetails &&
            Recovered.OrderedLookups == Reference.OrderedLookups &&
            Recovered.OrderedOverloads == Reference.OrderedOverloads &&
            Recovered.OrderedConversions == Reference.OrderedConversions &&
            Recovered.OrderedCastPaths == Reference.OrderedCastPaths &&
            Recovered.OrderedMetatables == Reference.OrderedMetatables &&
            Recovered.OrderedNamespaces == Reference.OrderedNamespaces &&
            Recovered.OrderedModules == Reference.OrderedModules,
        "a recovered freeze publishes exactly the cache a first-attempt freeze "
        "publishes");
}

void CheckMetadataContradictionRefusesDeterministically() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.Register("Increment", &Increment).IsSuccess(),
        "one callable exists before the contradiction");

  Luna::Detail::ReflectionDatabase *Database =
      Hooks::ReflectionDatabaseOf(Owner);
  Luna::Detail::ReflectionGenerationBuilder Divergent;
  Check(Database != nullptr &&
            Database->PublishGeneration(Divergent) ==
                Luna::Detail::ReflectionGenerationStatus::Valid,
        "the test publishes a reflection generation the generation set never "
        "committed");

  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  const Luna::RegistrationResult First = Registry.Freeze();
  const Luna::RegistrationResult Second = Registry.Freeze();
  Check(FailedWith(First, Luna::ErrorCategory::Internal, "inconsistent") &&
            MessageOf(First) == MessageOf(Second),
        "a metadata contradiction is reported identically on every attempt");
  Check(!Hooks::IsFrozen(Owner) && !Hooks::ObserveFreezeCache(Owner).Published,
        "a metadata contradiction publishes nothing at all");
  Check(Owner.IsReady() && Hooks::ObserveRootStackDepth(Owner) == EntryDepth &&
            Owner.Execute("assert(Increment(4) == 5)").IsSuccess(),
        "the Ready State and its stack survive the refused freeze");
}

// -- the frozen rejection matrix --------------------------------------------

void CheckFrozenStateRejectsEveryRegistrationAndModuleOperation() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Succeeded(RegisterModel(Owner)), "the representative model registers");

  // Two builders opened while the State was still Ready: neither may commit
  // after freeze, and neither may stage anything either.
  Luna::NamespaceBuilder Opened = Registry.RegisterNamespace("Opened");
  static_cast<void>(Opened.RegisterConstant("Slots", 2));
  Luna::ClassBuilder<Widget> OpenedClass = Registry.RegisterClass<Widget>(
      "OpenedWidget", Luna::StableTypeKey("tests.freeze.faults.Opened"));
  static_cast<void>(
      OpenedClass.Field("Charge", &Widget::Charge).QualifiedName());

  Check(Succeeded(Registry.Freeze()), "the populated State freezes");
  const Luna::Detail::FreezeCacheObservation Published =
      Hooks::ObserveFreezeCache(Owner);
  const auto Generations = Hooks::GenerationsOf(Owner);
  const std::uint64_t Reflected = Hooks::ReflectionGeneration(Owner);
  const std::size_t Bindings = Hooks::BindingCount(Owner);
  const std::size_t Namespaces = Hooks::NamespaceOwnershipCount(Owner);
  const std::size_t Classes = Hooks::RegisteredClassCount(Owner);
  const std::size_t Modules = Hooks::LoadedModuleCount(Owner);
  const std::size_t Definitions = Hooks::AvailableModuleCount(Owner);
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  const Luna::ErrorCategory NotReady = Luna::ErrorCategory::StateNotReady;
  Check(FailedWith(Registry.Register("Established", &Increment), NotReady,
                   "frozen"),
        "the established root-scope spelling is rejected while frozen");
  Check(FailedWith(Registry.RegisterFunction("Explicit", &Increment), NotReady,
                   "frozen"),
        "explicit function registration is rejected while frozen");
  Check(FailedWith(Registry.RegisterConstant("Limit", 3), NotReady, "frozen"),
        "constant registration is rejected while frozen");

  Luna::NamespaceBuilder Late = Registry.RegisterNamespace("Late");
  static_cast<void>(Late.RegisterConstant("Slots", 1));
  Check(FailedWith(Late.Commit(), NotReady, "frozen"),
        "a namespace plan opened while frozen is rejected");
  Check(FailedWith(Opened.Commit(), NotReady, "frozen"),
        "a namespace plan opened before freeze is rejected afterwards");

  Luna::EnumBuilder<Mode> LateModes = Registry.RegisterEnum<Mode>(
      "LateMode", Luna::StableTypeKey("tests.freeze.faults.LateMode"));
  Check(FailedWith(LateModes.Value("Off", Mode::Off).Commit(), NotReady,
                   "frozen"),
        "an enumeration plan is rejected while frozen");

  Luna::ClassBuilder<Widget> LateClass = Registry.RegisterClass<Widget>(
      "LateWidget", Luna::StableTypeKey("tests.freeze.faults.Late"));
  Check(FailedWith(LateClass.Field("Charge", &Widget::Charge).Commit(),
                   NotReady, "frozen"),
        "a class plan is rejected while frozen");
  Check(FailedWith(OpenedClass.Commit(), NotReady, "frozen"),
        "a class plan opened before freeze is rejected afterwards");

  Check(FailedWith(Registry.ProvideModule(ToolsManifest(), &ConfigureTools),
                   NotReady, "frozen"),
        "recording a module definition is rejected while frozen");
  Check(FailedWith(Registry.RegisterModule(ToolsManifest(), &ConfigureTools),
                   NotReady, "frozen"),
        "loading a module is rejected while frozen");

  // Every repeated freeze returns one identical already-frozen result and
  // republishes nothing.
  const Luna::RegistrationResult Repeated = Registry.Freeze();
  const Luna::RegistrationResult Again = Registry.Freeze();
  Check(FailedWith(Repeated, NotReady, "already frozen") &&
            MessageOf(Repeated) == MessageOf(Again),
        "every repeated freeze returns one identical already-frozen result");

  const Luna::Detail::FreezeCacheObservation After =
      Hooks::ObserveFreezeCache(Owner);
  Check(After.Address == Published.Address &&
            After.LookupDetails == Published.LookupDetails &&
            After.MetatableIdentities == Published.MetatableIdentities,
        "no rejection and no repeated freeze republished the caches");
  Check(Hooks::GenerationsOf(Owner) == Generations &&
            Hooks::ReflectionGeneration(Owner) == Reflected &&
            Hooks::BindingCount(Owner) == Bindings &&
            Hooks::NamespaceOwnershipCount(Owner) == Namespaces &&
            Hooks::RegisteredClassCount(Owner) == Classes &&
            Hooks::LoadedModuleCount(Owner) == Modules &&
            Hooks::AvailableModuleCount(Owner) == Definitions,
        "no rejected mutation changed one committed generation or store");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth &&
            !Hooks::BindingIsCommitted(Owner, "Explicit") &&
            !Hooks::NamespaceIsOwned(Owner, "Late") &&
            !Hooks::NamespaceIsOwned(Owner, "Opened"),
        "every rejection happened before any virtual-machine mutation");
  Check(Owner
            .Execute("assert(Increment(1) == 2)\n"
                     "assert(Studio.Version == 7)\n"
                     "assert(Tools.Slots == 4)")
            .IsSuccess(),
        "invocation keeps working after every rejection");
}

} // namespace

int RunFreezeAndCacheFaultTests() {
  FailureCount = 0;
  CheckWrongThreadRefusalPrecedesEveryOtherRefusal();
  CheckRefusedFreezeIsDeterministicAndRecoverable();
  CheckMetadataContradictionRefusesDeterministically();
  CheckFrozenStateRejectsEveryRegistrationAndModuleOperation();
  return FailureCount == 0 ? 0 : 1;
}
