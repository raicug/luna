// Focused coverage for atomic freeze preparation and frozen lifecycle behavior.

// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_member.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/binding/overload.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
  std::cerr << "freeze lifecycle check failed: " << Description << '\n';
}

[[nodiscard]] bool FailedWith(const Luna::RegistrationResult &Result,
                              Luna::ErrorCategory Category,
                              std::string_view Fragment) {
  return !Result.IsSuccess() && Result.Diagnostic() != nullptr &&
         Result.Diagnostic()->Category() == Category &&
         Result.Diagnostic()->Message().find(Fragment) != std::string::npos;
}

[[nodiscard]] int Measure(int Value) { return Value + 1; }
[[nodiscard]] int Measure(int Value, int Scale) { return Value * Scale; }

struct Base {
  virtual ~Base() = default;
  int Value = 4;
};

struct Derived final : Base {
  [[nodiscard]] int Expensive() const { return Value * 3; }
};
[[nodiscard]] Luna::StableTypeKey BaseKey() {
  return Luna::StableTypeKey("tests.freeze.Base");
}

[[nodiscard]] Luna::StableTypeKey DerivedKey() {
  return Luna::StableTypeKey("tests.freeze.Derived");
}

[[nodiscard]] Luna::ModuleManifest UnitsManifest() {
  const auto Version = Luna::SemanticVersion::TryParse("1.0.0");
  const auto Manifest =
      Version
          ? Luna::ModuleManifest::TryCreate("tests.units", *Version, {}, "", {})
          : std::nullopt;
  return Manifest ? *Manifest : Luna::ModuleManifest();
}

void ConfigureUnits(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Units = Builder.RegisterNamespace("Units");
  static_cast<void>(Units.RegisterConstant("Scale", 2));
}

[[nodiscard]] Luna::RegistrationResult RegisterModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  if (auto First = Registry.RegisterFunction(
          "Measure", Luna::Overload<int(int)>(&Measure));
      !First.IsSuccess())
    return First;
  if (auto Second = Registry.RegisterFunction(
          "Measure", Luna::Overload<int(int, int)>(&Measure));
      !Second.IsSuccess())
    return Second;

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Base> BaseClass =
      Studio.RegisterClass<Base>("Base", BaseKey());
  static_cast<void>(BaseClass.Field("Value", &Base::Value).QualifiedName());
  Luna::ClassBuilder<Derived> DerivedClass =
      Studio.RegisterClass<Derived>("Derived", DerivedKey());
  static_cast<void>(DerivedClass.Base<Base>(BaseKey())
                        .Property("Expensive", Luna::PropertyPolicy::Lazy(),
                                  &Derived::Expensive)
                        .QualifiedName());
  if (auto Classes = Studio.Commit(); !Classes.IsSuccess())
    return Classes;
  return Registry.RegisterModule(UnitsManifest(), ConfigureUnits);
}

[[nodiscard]] Luna::Detail::ClassExposureObservation
ExposeDerived(Luna::State &Owner, Derived &Value,
              const std::uint64_t &Lifetime) {
  Luna::Detail::ClassExposureRequest Request;
  Request.QualifiedName = "Studio.Derived";
  Request.Path = "FrozenDerived";
  Request.Storage = &Value;
  Request.Ownership = Luna::Detail::OwnershipModel::Borrowed;
  Request.Access = Luna::Detail::ConstAccess::Mutable;
  Request.LifetimeGeneration = &Lifetime;
  return Hooks::ExposeClassUserdata(Owner, Request);
}

[[nodiscard]] Luna::Detail::ClassMemberAccessObservation
ReadExpensive(Luna::State &Owner) {
  Luna::Detail::ClassMemberAccessRequest Request;
  Request.QualifiedName = "Studio.Derived";
  Request.Member = "Expensive";
  Request.Path = "FrozenDerived";
  return Hooks::ReadClassMemberValue(Owner, Request);
}
void CheckSuccessfulFreezePublishesCompleteCaches() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterModel(Owner).IsSuccess(), "the representative model registers");

  const auto BeforeGenerations = Hooks::GenerationsOf(Owner);
  const Luna::ReflectionSnapshot Before = Registry.Reflection();
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  Check(!Hooks::ObserveFreezeCache(Owner).Published,
        "no freeze cache exists before freeze");

  const Luna::RegistrationResult Frozen = Registry.Freeze();
  Check(Frozen.IsSuccess(), "freeze succeeds after complete validation");
  const Luna::Detail::FreezeCacheObservation Cache =
      Hooks::ObserveFreezeCache(Owner);
  Check(Cache.Published && Hooks::IsFrozen(Owner),
        "cache and frozen phase publish together");
  Check(BeforeGenerations &&
            Cache.Key.Generation == BeforeGenerations->Generation(),
        "the cache key names the committed generation");
  Check(Cache.Key.ReflectionGeneration == Before.Generation() &&
            Cache.Key.State == Hooks::LogicalIdentityOf(Owner).value_or(
                                   Luna::Detail::StateIdentity()),
        "the cache key names reflection and logical State identity");
  Check(Cache.Lookups == BeforeGenerations->Symbols().Size() &&
            Cache.Overloads >= 1 && Cache.Conversions >= 5,
        "sorted lookups, overload indices, and conversions are prepared");
  Check(Cache.CastPaths >= 1 && Cache.Metatables == 2,
        "class cast paths and metatable maps are prepared");
  Check(Cache.Namespaces == 2 && Cache.Modules == 1,
        "namespace and module caches are prepared");
  Check(std::is_sorted(Cache.OrderedLookups.begin(), Cache.OrderedLookups.end(),
                       [](const std::string &Left, const std::string &Right) {
                         return Left.substr(0, Left.find(':')) <
                                Right.substr(0, Right.find(':'));
                       }),
        "the cache lookup array is deterministic and sorted");
  Check(Hooks::GenerationsOf(Owner) == BeforeGenerations &&
            Registry.Reflection().Generation() == Before.Generation() &&
            Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "freeze changes no generation, reflection record, or stack value");

  Check(Owner
            .Execute("assert(Measure(4) == 5)\n"
                     "assert(Measure(4, 3) == 12)\n"
                     "assert(Units.Scale == 2)")
            .IsSuccess(),
        "invocation remains available after freeze");
  Check(Registry.Reflection().Find("Studio.Derived").IsValid(),
        "reflection reads remain available after freeze");

  Check(FailedWith(Registry.Register("Later", [] { return 1; }),
                   Luna::ErrorCategory::StateNotReady, "frozen"),
        "function registration is rejected while frozen");
  Check(FailedWith(Registry.ProvideModule(UnitsManifest(), ConfigureUnits),
                   Luna::ErrorCategory::StateNotReady, "frozen"),
        "module-definition mutation is rejected while frozen");
  Check(FailedWith(Registry.RegisterModule(UnitsManifest(), ConfigureUnits),
                   Luna::ErrorCategory::StateNotReady, "frozen"),
        "module loading is rejected while frozen");

  // Runtime-only owner-thread state remains live. Exposing a value adds a weak
  // identity entry and a successful lazy read adds a property cache entry,
  // without changing any logical metadata or the published freeze cache.
  Derived Value;
  std::uint64_t Lifetime = 1;
  Check(ExposeDerived(Owner, Value, Lifetime).Status == "created" &&
            Hooks::LiveCachedIdentityCount(Owner) == 1,
        "frozen invocation may create documented weak identity state");
  const auto Lazy = ReadExpensive(Owner);
  Check(Lazy.Reached && Lazy.Recorded &&
            Hooks::LiveLazyMemberCacheEntryCount(Owner) == 1,
        "frozen invocation may create a successful lazy property value");
  Check(Hooks::RetireClassUserdata(Owner, &Value) &&
            Hooks::LiveCachedIdentityCount(Owner) == 0,
        "frozen runtime state may follow userdata lifetime");
  const auto AfterRuntime = Hooks::ObserveFreezeCache(Owner);
  Check(AfterRuntime.Address == Cache.Address &&
            AfterRuntime.OrderedLookups == Cache.OrderedLookups &&
            Registry.Reflection().Generation() == Before.Generation(),
        "runtime-only changes never mutate logical metadata or freeze caches");
}

void CheckPreparationFailureIsAtomicAndRecoverable() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.Register("Existing", [] { return 7; }).IsSuccess(),
        "one callable exists before the failed freeze");
  const auto Generations = Hooks::GenerationsOf(Owner);
  const std::uint64_t ReflectionGeneration = Registry.Reflection().Generation();

  Hooks::InjectFault(Owner, FaultPoint::FreezePreparation);
  const auto Failed = Registry.Freeze();
  Check(FailedWith(Failed, Luna::ErrorCategory::Internal, "allocation_failure"),
        "a preparation fault reports one deterministic failure");
  Check(!Hooks::IsFrozen(Owner) && !Hooks::ObserveFreezeCache(Owner).Published,
        "a failed preparation publishes neither caches nor frozen flag");
  Check(Hooks::GenerationsOf(Owner) == Generations &&
            Registry.Reflection().Generation() == ReflectionGeneration,
        "a failed preparation leaves Ready metadata unchanged");
  Check(Registry.Register("Recovered", [] { return 9; }).IsSuccess() &&
            Owner
                .Execute("assert(Existing() == 7)\n"
                         "assert(Recovered() == 9)")
                .IsSuccess(),
        "the Ready State remains reusable after failure");
  Check(Registry.Freeze().IsSuccess(),
        "a later freeze succeeds after the transient failure");
}
void CheckRepeatedFreezeAndThreadAffinityAreDeterministic() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.Freeze().IsSuccess(), "an empty Ready State can freeze");
  const auto Published = Hooks::ObserveFreezeCache(Owner);

  const auto Repeated = Registry.Freeze();
  const auto RepeatedAgain = Registry.Freeze();
  Check(FailedWith(Repeated, Luna::ErrorCategory::StateNotReady,
                   "already frozen") &&
            Repeated.Diagnostic() != nullptr &&
            RepeatedAgain.Diagnostic() != nullptr &&
            Repeated.Diagnostic()->Message() ==
                RepeatedAgain.Diagnostic()->Message(),
        "every repeated freeze returns the same already-frozen result");
  Check(Hooks::ObserveFreezeCache(Owner).Address == Published.Address,
        "a repeated freeze does not republish caches");

  Luna::State ThreadBound;
  Luna::RegistrationResult Foreign = Luna::RegistrationResult::Success();
  std::thread Other(
      [&ThreadBound, &Foreign] { Foreign = ThreadBound.Bindings().Freeze(); });
  Other.join();
  Check(
      FailedWith(Foreign, Luna::ErrorCategory::StateNotReady, "owner thread") &&
          !Hooks::IsFrozen(ThreadBound) &&
          !Hooks::ObserveFreezeCache(ThreadBound).Published,
      "a foreign-thread freeze is rejected before mutation");
  Check(ThreadBound.Bindings().Freeze().IsSuccess(),
        "the owner thread can freeze after a foreign refusal");

  Luna::State MovedFrom;
  Luna::BindingRegistry StaleRegistry = MovedFrom.Bindings();
  Luna::State Moved(std::move(MovedFrom));
  Check(FailedWith(StaleRegistry.Freeze(), Luna::ErrorCategory::StateNotReady,
                   "not ready"),
        "a registry attached to the moved-from owner cannot freeze");
  Check(Moved.Bindings().Freeze().IsSuccess(),
        "the moved implementation preserves affinity and can freeze");
}

void CheckCrossRecordMismatchPublishesNothing() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::Detail::ReflectionDatabase *Database =
      Hooks::ReflectionDatabaseOf(Owner);
  Luna::Detail::ReflectionGenerationBuilder Divergent;
  Check(Database != nullptr &&
            Database->PublishGeneration(Divergent) ==
                Luna::Detail::ReflectionGenerationStatus::Valid,
        "the test creates a divergent private reflection generation");
  const auto Failed = Registry.Freeze();
  Check(FailedWith(Failed, Luna::ErrorCategory::Internal, "inconsistent") &&
            !Hooks::IsFrozen(Owner) &&
            !Hooks::ObserveFreezeCache(Owner).Published,
        "cross-record inconsistency publishes neither cache nor frozen flag");
}

} // namespace

int RunFreezeLifecycleTests() {
  FailureCount = 0;
  CheckSuccessfulFreezePublishesCompleteCaches();
  CheckPreparationFailureIsAtomicAndRecoverable();
  CheckRepeatedFreezeAndThreadAffinityAreDeterministic();
  CheckCrossRecordMismatchPublishesNothing();
  return FailureCount == 0 ? 0 : 1;
}
