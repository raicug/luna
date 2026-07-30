// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/ownership.hpp"

#include <cstddef>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::ClassAllocator;
using Luna::Detail::ConstAccess;
using Luna::Detail::LifetimeState;
using Luna::Detail::OwnershipModel;
using Luna::Detail::ReleaseCause;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "class userdata integration check failed: " << Description
            << '\n';
}

struct Vector3 final {
  double X = 1.0;
  double Y = 2.0;
  double Z = 3.0;
};

class Actor {
public:
  virtual ~Actor() = default;
  int Health = 100;
};

int ScaleCalls = 0;
int DescribeCalls = 0;
int MarkCalls = 0;

void ResetCallCounters() {
  ScaleCalls = 0;
  DescribeCalls = 0;
  MarkCalls = 0;
}

[[nodiscard]] int Scale(int Value) {
  ++ScaleCalls;
  return Value * 2;
}

[[nodiscard]] int Describe(Luna::ArgumentView Arguments) {
  ++DescribeCalls;
  return static_cast<int>(Arguments.Size());
}

[[nodiscard]] int Mark() {
  ++MarkCalls;
  return MarkCalls;
}

int DestroyCalls = 0;
int DeallocateCalls = 0;

void ResetStorageCounters() {
  DestroyCalls = 0;
  DeallocateCalls = 0;
}

[[nodiscard]] Vector3 *AllocateVector() {
  Vector3 *Storage = static_cast<Vector3 *>(::operator new(sizeof(Vector3)));
  new (Storage) Vector3{};
  return Storage;
}

[[nodiscard]] Luna::ClassAllocator OwnedStorageProtocol() {
  Luna::ClassAllocator::AllocateOperation Allocate =
      [](const Luna::StorageRequest &Wanted) -> void * {
    return ::operator new(Wanted.ByteCount);
  };
  Luna::ClassAllocator::ConstructOperation Construct = [](void *Storage) {
    new (Storage) Vector3{};
    return Luna::AllocatorStepResult::Done();
  };
  Luna::ClassAllocator::DestroyOperation Destroy = [](void *Storage) {
    ++DestroyCalls;
    static_cast<Vector3 *>(Storage)->~Vector3();
    return Luna::AllocatorStepResult::Done();
  };
  Luna::ClassAllocator::DeallocateOperation Deallocate =
      [](void *Storage, const Luna::StorageRequest &) {
        ++DeallocateCalls;
        ::operator delete(Storage);
        return Luna::AllocatorStepResult::Done();
      };
  return Luna::ClassAllocator::FromOperations(
      "Studio.Vector3Storage", Luna::StorageRequest::ForClass<Vector3>(),
      std::move(Allocate), std::move(Construct), std::move(Destroy),
      std::move(Deallocate));
}

std::size_t ParticleLive = 0;
std::size_t ParticleConstructed = 0;
int FactoryCalls = 0;

struct Particle final {
  double Mass = 1.0;

  Particle() {
    ++ParticleLive;
    ++ParticleConstructed;
  }

  explicit Particle(double MassValue) : Mass(MassValue) {
    ++ParticleLive;
    ++ParticleConstructed;
  }

  Particle(const Particle &Other) : Mass(Other.Mass) {
    ++ParticleLive;
    ++ParticleConstructed;
  }

  Particle(Particle &&Other) noexcept : Mass(Other.Mass) {
    ++ParticleLive;
    ++ParticleConstructed;
  }

  Particle &operator=(const Particle &) = default;
  Particle &operator=(Particle &&) noexcept = default;

  ~Particle() { --ParticleLive; }
};

void ResetConstructionCounters() {
  ParticleLive = 0;
  ParticleConstructed = 0;
  FactoryCalls = 0;
}

[[nodiscard]] Particle MakeHeavy(double Mass) {
  ++FactoryCalls;
  return Particle(Mass);
}

[[nodiscard]] std::shared_ptr<Particle> MakeBoxed() {
  ++FactoryCalls;
  return std::make_shared<Particle>(7.0);
}

[[nodiscard]] Particle MakeRefused(double) {
  throw std::runtime_error("no particle today");
}

[[nodiscard]] Particle *EngineParticle() {
  static Particle Engine(11.0);
  return &Engine;
}

[[nodiscard]] Luna::ClassAllocator ParticleStorageProtocol() {
  Luna::ClassAllocator::AllocateOperation Allocate =
      [](const Luna::StorageRequest &Wanted) -> void * {
    return ::operator new(Wanted.ByteCount, std::align_val_t{Wanted.Alignment});
  };
  Luna::ClassAllocator::ConstructOperation Construct = [](void *Storage) {
    static_cast<void>(new (Storage) Particle());
    return Luna::AllocatorStepResult::Done();
  };
  Luna::ClassAllocator::DestroyOperation Destroy = [](void *Storage) {
    ++DestroyCalls;
    static_cast<Particle *>(Storage)->~Particle();
    return Luna::AllocatorStepResult::Done();
  };
  Luna::ClassAllocator::DeallocateOperation Deallocate =
      [](void *Storage, const Luna::StorageRequest &Wanted) {
        ++DeallocateCalls;
        ::operator delete(Storage, std::align_val_t{Wanted.Alignment});
        return Luna::AllocatorStepResult::Done();
      };
  return Luna::ClassAllocator::FromOperations(
      "Studio.ParticleArena", Luna::StorageRequest::ForClass<Particle>(),
      std::move(Allocate), std::move(Construct), std::move(Destroy),
      std::move(Deallocate));
}

[[nodiscard]] bool RegisterModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Vector3> Vector = Studio.RegisterClass<Vector3>(
      "Vector3", Luna::StableTypeKey("Studio.IntegrationVector3"));
  Luna::ClassBuilder<Actor> Character = Studio.RegisterClass<Actor>(
      "Actor", Luna::StableTypeKey("Studio.IntegrationActor"));
  Luna::NamespaceBuilder &Staged = Studio.RegisterFunction("Scale", &Scale)
                                       .RegisterFunction("Describe", &Describe)
                                       .RegisterFunction("Mark", &Mark);
  static_cast<void>(Staged.QualifiedName());
  static_cast<void>(Vector.QualifiedName());
  static_cast<void>(Character.QualifiedName());
  return Studio.Commit().IsSuccess();
}

[[nodiscard]] bool RegisterPermutedModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::NamespaceBuilder &Staged = Studio.RegisterFunction("Mark", &Mark)
                                       .RegisterFunction("Describe", &Describe)
                                       .RegisterFunction("Scale", &Scale);
  static_cast<void>(Staged.QualifiedName());
  Luna::ClassBuilder<Actor> Character = Studio.RegisterClass<Actor>(
      "Actor", Luna::StableTypeKey("Studio.IntegrationActor"));
  Luna::ClassBuilder<Vector3> Vector = Studio.RegisterClass<Vector3>(
      "Vector3", Luna::StableTypeKey("Studio.IntegrationVector3"));
  static_cast<void>(Character.QualifiedName());
  static_cast<void>(Vector.QualifiedName());
  return Studio.Commit().IsSuccess();
}

[[nodiscard]] bool RegisterConstructionModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::NamespaceBuilder &Staged =
      Studio.RegisterFunction("Scale", &Scale).RegisterFunction("Mark", &Mark);
  static_cast<void>(Staged.QualifiedName());

  Luna::ClassBuilder<Particle> Class = Studio.RegisterClass<Particle>(
      "Particle", Luna::StableTypeKey("Studio.IntegrationParticle"));
  Luna::ClassBuilder<Particle> &Documented =
      Class.Documentation("One simulated particle.");
  Luna::ClassBuilder<Particle> &WithDefault = Documented.Constructor<>();
  Luna::ClassBuilder<Particle> &WithMass = WithDefault.Constructor<double>();
  Luna::ClassBuilder<Particle> &WithFactory =
      WithMass.Factory("Heavy", &MakeHeavy);
  Luna::ClassBuilder<Particle> &WithShared =
      WithFactory.Factory("Boxed", &MakeBoxed);
  Luna::ClassBuilder<Particle> &WithRefused =
      WithShared.Factory("Refused", &MakeRefused);
  Luna::ClassBuilder<Particle> &WithSingleton =
      WithRefused.Singleton("Engine", &EngineParticle);
  Luna::ClassBuilder<Particle> &WithStorage =
      WithSingleton.Allocator(ParticleStorageProtocol());
  Luna::ClassBuilder<Particle> &WithDocumentation =
      WithStorage.Documentation("Heavy", "One heavier particle.");
  static_cast<void>(WithDocumentation.QualifiedName());
  return Studio.Commit().IsSuccess();
}

[[nodiscard]] Luna::Detail::ClassValueWriteObservation
ExposeValue(Luna::State &Host, std::string_view QualifiedName,
            const std::string &Path, void *Storage, OwnershipModel Ownership,
            const Luna::LifetimeHandle &Handle, std::shared_ptr<void> Shared,
            const ClassAllocator &Allocator,
            ConstAccess Access = ConstAccess::Mutable) {
  Luna::Detail::ClassValueExposureRequest Request;
  Request.QualifiedName = std::string(QualifiedName);
  Request.Path = Path;
  Request.Storage = Storage;
  Request.Ownership = Ownership;
  Request.Access = Access;
  Request.Handle = Handle;
  Request.SharedOwnership = std::move(Shared);
  Request.Allocator = Allocator;
  return Hooks::ExposeClassValue(Host, Request);
}

[[nodiscard]] Luna::Detail::ClassValueWriteObservation
ExposeBorrowed(Luna::State &Host, const std::string &Path, void *Storage,
               const Luna::LifetimeHandle &Handle) {
  return ExposeValue(Host, "Studio.Vector3", Path, Storage,
                     OwnershipModel::Borrowed, Handle, nullptr,
                     ClassAllocator());
}

[[nodiscard]] Luna::Detail::ClassAccessObservation
ReadValue(Luna::State &Host, const std::string &Path, const void *Expected,
          std::string_view QualifiedName = "Studio.Vector3") {
  Luna::Detail::ClassAccessRequest Request;
  Request.QualifiedName = std::string(QualifiedName);
  Request.Path = Path;
  Request.ExpectedStorage = Expected;
  return Hooks::AccessClassUserdata(Host, Request);
}

[[nodiscard]] bool Succeeds(Luna::State &Host, std::string_view Source) {
  const Luna::ExecutionResult Result = Host.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "class userdata source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

[[nodiscard]] std::string Failure(Luna::State &Host, std::string_view Source) {
  const Luna::ExecutionResult Result = Host.Execute(Source);
  if (Result.IsSuccess())
    return std::string();
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  return Diagnostic ? Diagnostic->Message() : std::string("<no diagnostic>");
}

[[nodiscard]] bool Contains(std::string_view Text, std::string_view Needle) {
  return Text.find(Needle) != std::string_view::npos;
}

[[nodiscard]] int ScriptResult(Luna::State &Host, const std::string &Source) {
  if (!Succeeds(Host, Source))
    return -1;
  const auto Observed = Hooks::ObserveIntegerGlobal(Host, "Result");
  return Observed ? *Observed : -1;
}

[[nodiscard]] bool RestoredCheckpoint(const Luna::State &Host) {
  const auto Observation = Hooks::ObserveLastCallbackStackRestoration(Host);
  return Observation.has_value() &&
         Observation->EntryDepth == Observation->RestoredDepth &&
         Observation->ErrorDepth == Observation->RestoredDepth + 1;
}

void CheckExposedObjectsAreConsumedFromScript() {
  ResetCallCounters();
  ResetStorageCounters();
  {
    Luna::State Owner;
    Check(Owner.IsReady() && RegisterModel(Owner),
          "one plan publishes the namespace, both classes, and the callables");
    const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

    Vector3 Borrowed;
    Luna::LifetimeHandle Handle;
    Vector3 *Owned = AllocateVector();
    const std::shared_ptr<Vector3> Shared = std::make_shared<Vector3>();
    Actor Character;
    Luna::LifetimeHandle CharacterHandle;

    Check(ExposeBorrowed(Owner, "Studio.Borrowed", &Borrowed, Handle).Published,
          "a borrowed object is exposed inside the registered namespace");
    Check(ExposeValue(Owner, "Studio.Vector3", "Studio.Owned", Owned,
                      OwnershipModel::LuaOwned,
                      Luna::LifetimeHandle::Undeclared(), nullptr,
                      OwnedStorageProtocol())
              .Published,
          "a Lua-owned object is exposed inside the registered namespace");
    Check(ExposeValue(Owner, "Studio.Vector3", "Studio.Shared", Shared.get(),
                      OwnershipModel::Shared,
                      Luna::LifetimeHandle::Undeclared(), Shared,
                      ClassAllocator())
              .Published,
          "a shared object is exposed inside the registered namespace");
    Check(ExposeValue(Owner, "Studio.Actor", "Studio.Character", &Character,
                      OwnershipModel::Borrowed, CharacterHandle, nullptr,
                      ClassAllocator())
              .Published,
          "an object of the second class is exposed as its own value");
    Check(Shared.use_count() == 2,
          "Luna retains exactly one shared ownership reference");
    Check(Hooks::PublishedUserdataCount(Owner) == 4 &&
              Hooks::LiveCachedIdentityCount(Owner) == 4,
          "each exposed object has exactly one published value and one entry");

    Check(ScriptResult(Owner,
                       "Result = 0\n"
                       "local Names = { 'Borrowed', 'Owned', 'Shared' }\n"
                       "for _, Name in ipairs(Names) do\n"
                       "  local Value = Studio[Name]\n"
                       "  if type(Value) == 'userdata' and typeof(Value) == "
                       "'Studio.Vector3' then Result = Result + 1 end\n"
                       "end\n"
                       "if typeof(Studio.Character) == 'Studio.Actor' then "
                       "Result = Result + 1 end\n") == 4,
          "every exposed value carries the metatable of its own class");
    Check(Hooks::ClassMetatableCreationCount(Owner, "Studio.Vector3") == 1 &&
              Hooks::ClassMetatableCreationCount(Owner, "Studio.Actor") == 1,
          "each class created exactly one metatable for all of its values");
    Check(ScriptResult(Owner,
                       "Result = 0\nif typeof(Studio.Borrowed) ~= "
                       "typeof(Studio.Character) then Result = 1 end") == 1,
          "two registered classes never share one metatable");

    Check(Succeeds(Owner, "Alias = Studio.Borrowed\n"
                          "Holder = { Value = Studio.Owned }\n"
                          "Sequence = { Studio.Shared }\n"),
          "the script aliases and stores the exposed values");
    Check(ReadValue(Owner, "Alias", &Borrowed).DeliveredExpectedObject,
          "a script alias delivers exactly the borrowed object to native code");
    Check(ReadValue(Owner, "Holder.Value", Owned).DeliveredExpectedObject,
          "a script table field delivers exactly the Lua-owned object");
    Check(
        ReadValue(Owner, "Studio.Shared", Shared.get()).DeliveredExpectedObject,
        "the shared value delivers exactly the shared object");
    Check(ReadValue(Owner, "Studio.Character", &Character, "Studio.Actor")
              .DeliveredExpectedObject,
          "the second class's value delivers exactly its own object");
    Check(ReadValue(Owner, "Alias", &Borrowed).PermitsMutation,
          "a mutable view reaches native code as a mutable handle");

    Check(ScriptResult(
              Owner, "Result = 0\n"
                     "if Alias == Studio.Borrowed and rawequal(Alias, "
                     "Studio.Borrowed) then Result = Result + 1 end\n"
                     "if Holder.Value == Studio.Owned then Result = Result + "
                     "1 end\n"
                     "if Studio.Borrowed ~= Studio.Owned then Result = Result "
                     "+ 1 end\n") == 3,
          "script identity of an exposed value follows the native object");

    const auto Reused =
        ExposeBorrowed(Owner, "Studio.Again", &Borrowed, Handle);
    Check(Reused.Published, "re-exposing one object publishes a value");
    Check(Hooks::LiveCachedIdentityCount(Owner) == 4 &&
              Hooks::PublishedUserdataCount(Owner) == 4,
          "a reused exposure creates no second entry and no second owner");
    Check(ScriptResult(Owner, "Result = 0\nif Studio.Again == Studio.Borrowed "
                              "then Result = 1 end") == 1,
          "a reused exposure is exactly the value the script already had");

    const auto Conflicting = ExposeValue(
        Owner, "Studio.Vector3", "Studio.Conflict", &Borrowed,
        OwnershipModel::LuaOwned, Luna::LifetimeHandle::Undeclared(), nullptr,
        OwnedStorageProtocol());
    Check(!Conflicting.Published &&
              Conflicting.Failure == "conflicting_ownership",
          "re-exposing one object under another ownership model is refused");
    Check(Conflicting.FinalStackDepth == Conflicting.EntryStackDepth,
          "a refused re-exposure leaves the stack exactly as it found it");
    Check(Hooks::LiveCachedIdentityCount(Owner) == 4 &&
              Hooks::PublishedUserdataCount(Owner) == 4,
          "a refused re-exposure creates no second owner");
    Check(ScriptResult(Owner, "Result = 0\nif Studio.Conflict == nil then "
                              "Result = 1 end") == 1,
          "a refused re-exposure publishes no value the script can see");
    Check(DestroyCalls == 0 && DeallocateCalls == 0,
          "a refused re-exposure releases nothing");

    Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
          "every exposure, refusal, and access restores the root stack depth");
    Check(Succeeds(Owner, "assert(Studio.Mark() == 1, 'reuse')"),
          "the State keeps invoking its callables around every exposure");
  }
  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "State destruction releases the Lua-owned object exactly once");
}

void CheckAccessFailsBeforeNativeCode() {
  ResetCallCounters();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model registers");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Vector3 Object;
  Luna::LifetimeHandle Handle;
  Check(ExposeBorrowed(Owner, "Studio.Sample", &Object, Handle).Published,
        "one borrowed object is exposed");

  const std::string Scalar =
      Failure(Owner, "return Studio.Scale(Studio.Sample)");
  Check(Contains(Scalar, "Studio.Scale") && Contains(Scalar, "argument 1") &&
            Contains(Scalar, "signed 32-bit integer") &&
            Contains(Scalar, "userdata"),
        "a class value handed to a scalar parameter names the exact position");
  Check(ScaleCalls == 0, "a refused scalar conversion invokes no native code");
  Check(RestoredCheckpoint(Owner),
        "a refused scalar conversion restores the callback checkpoint");

  DescribeCalls = 0;
  Check(ScriptResult(Owner, "Result = Studio.Describe(1, Studio.Sample)") == 2,
        "a class value in the variadic tail is captured directly, not "
        "refused");
  Check(DescribeCalls == 1,
        "a variadic call receiving a class value invokes native code once");

  Check(Failure(Owner, "return Studio.Scale(Studio.Sample)") == Scalar,
        "a class-value scalar refusal reports one identical deterministic "
        "message");

  Luna::State Permuted;
  Check(Permuted.IsReady() && RegisterPermutedModel(Permuted),
        "the permuted model registers");
  Vector3 Other;
  Luna::LifetimeHandle OtherHandle;
  Check(
      ExposeBorrowed(Permuted, "Studio.Sample", &Other, OtherHandle).Published,
      "the permuted State exposes its own object");
  Check(Failure(Permuted, "return Studio.Scale(Studio.Sample)") == Scalar,
        "a class-value refusal never depends on registration order");

  Check(Succeeds(Owner, "assert(Studio.Scale(4) == 8, 'scalar')\n"
                        "assert(Studio.Describe(1, 2, 3) == 3, 'variadic')\n"),
        "the State stays reusable after every refused conversion");
  Check(ScaleCalls == 1 && DescribeCalls == 2,
        "each recovered call invokes its target exactly once");
  Check(ReadValue(Owner, "Studio.Sample", &Object).DeliveredExpectedObject,
        "the exposed value still reaches native code after every refusal");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every refused call restores the exact root stack depth");
}

void CheckInvalidScriptVisibleValuesNeverReachNativeCode() {
  ResetCallCounters();
  ResetStorageCounters();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model registers");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Vector3 Expiring;
  Luna::LifetimeHandle Expires;
  Vector3 *Owned = AllocateVector();
  Actor Character;
  Luna::LifetimeHandle CharacterHandle;
  Check(
      ExposeBorrowed(Owner, "Studio.Expiring", &Expiring, Expires).Published &&
          ExposeValue(Owner, "Studio.Vector3", "Studio.Owned", Owned,
                      OwnershipModel::LuaOwned,
                      Luna::LifetimeHandle::Undeclared(), nullptr,
                      OwnedStorageProtocol())
              .Published &&
          ExposeValue(Owner, "Studio.Actor", "Studio.Character", &Character,
                      OwnershipModel::Borrowed, CharacterHandle, nullptr,
                      ClassAllocator())
              .Published,
      "the representative objects are exposed");
  Check(Succeeds(Owner, "Aliased = Studio.Expiring\n"
                        "Held = { Value = Studio.Owned }\n"
                        "Foreign = { X = 1 }\n"
                        "Proxy = newproxy(true)\n"
                        "Bare = newproxy(false)\n"),
        "the script aliases the values and builds its own look-alikes");

  const auto WrongClass = ReadValue(Owner, "Studio.Character", &Character);
  Check(!WrongClass.ReachedNativeCode &&
            WrongClass.Failure == "userdata_type_mismatch",
        "a value of another registered class never reaches native code");
  Check(Contains(WrongClass.Diagnostic, "Studio.Vector3"),
        "the refusal names the class that was expected");

  const auto ForeignTable = ReadValue(Owner, "Foreign", &Expiring);
  Check(!ForeignTable.ReachedNativeCode &&
            ForeignTable.Failure == "foreign_userdata" &&
            Contains(ForeignTable.Diagnostic, "table"),
        "a script-created table never reaches native code");
  const auto Proxy = ReadValue(Owner, "Proxy", &Expiring);
  Check(!Proxy.ReachedNativeCode && Proxy.Failure == "foreign_userdata",
        "a forged userdata with its own metatable never reaches native code");
  const auto Bare = ReadValue(Owner, "Bare", &Expiring);
  Check(!Bare.ReachedNativeCode && Bare.Failure == "foreign_userdata",
        "a forged userdata without a metatable never reaches native code");
  const auto Absent = ReadValue(Owner, "Missing", &Expiring);
  Check(!Absent.ReachedNativeCode && Absent.Failure == "foreign_userdata",
        "an absent value never reaches native code");

  Check(ReadValue(Owner, "Aliased", &Expiring).ReachedNativeCode,
        "the alias reaches native code while the lifetime is live");
  Expires.Invalidate();
  const auto Expired = ReadValue(Owner, "Studio.Expiring", &Expiring);
  const auto ExpiredAlias = ReadValue(Owner, "Aliased", &Expiring);
  Check(!Expired.ReachedNativeCode && Expired.Failure == "expired_userdata" &&
            Contains(Expired.Diagnostic, "expired_lifetime_handle"),
        "an invalidated lifetime rejects the value before native code");
  Check(!ExpiredAlias.ReachedNativeCode &&
            ExpiredAlias.Diagnostic == Expired.Diagnostic,
        "every script-visible copy of the value is rejected identically");
  Check(Expiring.X == 1.0,
        "invalidating a borrowed lifetime never touches the object");
  Check(ScriptResult(Owner, "Result = 0\nif Studio.Expiring ~= nil and "
                            "typeof(Studio.Expiring) == 'Studio.Vector3' then "
                            "Result = 1 end") == 1,
        "an expired value is still a typed value in script; only access ends");

  Check(Hooks::ReleaseClassValue(Owner, Owned, ReleaseCause::LifecycleAction),
        "a lifecycle action releases the Lua-owned value once");
  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "the released value is destroyed and deallocated exactly once");
  const auto Released = ReadValue(Owner, "Studio.Owned", Owned);
  const auto ReleasedAlias = ReadValue(Owner, "Held.Value", Owned);
  Check(!Released.ReachedNativeCode && Released.Failure == "expired_userdata",
        "a released value never reaches native code again");
  Check(!ReleasedAlias.ReachedNativeCode,
        "a script-held copy of a released value never reaches native code");
  const auto Header = Hooks::ObserveClassUserdata(Owner, "Studio.Owned");
  Check(Header && Header->Lifetime != LifetimeState::Published,
        "the released value's own header no longer permits access");

  Vector3 *Collected = AllocateVector();
  Check(ExposeValue(Owner, "Studio.Vector3", "Collected", Collected,
                    OwnershipModel::LuaOwned,
                    Luna::LifetimeHandle::Undeclared(), nullptr,
                    OwnedStorageProtocol())
            .Published,
        "another Lua-owned value is exposed");
  Check(Succeeds(Owner, "Collected = nil"),
        "the script drops its reference to the value");
  Check(Hooks::CollectGarbage(Owner), "the collector runs to completion");
  Check(DestroyCalls == 2 && DeallocateCalls == 2,
        "the collected value is destroyed and deallocated exactly once");
  Check(!ReadValue(Owner, "Collected", Collected).ReachedNativeCode,
        "a collected value never reaches native code again");
  Check(Hooks::ObserveUserdataCollections().ContainedException == 0,
        "nothing was thrown at the collection boundary");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every refused access restores the exact root stack depth");

  Vector3 Fresh;
  Luna::LifetimeHandle FreshHandle;
  Check(ExposeBorrowed(Owner, "Studio.Fresh", &Fresh, FreshHandle).Published,
        "the State exposes another value after every refusal");
  Check(ReadValue(Owner, "Studio.Fresh", &Fresh).DeliveredExpectedObject,
        "the value exposed after every refusal reaches native code");
  Check(Succeeds(Owner, "assert(Studio.Mark() >= 1, 'reuse')"),
        "the State keeps invoking its callables after every refusal");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "the recovered exposure and access restore the root stack depth");
  Hooks::ResetUserdataCollections();
}

void CheckClassMetatableCannotBeReplacedForgedOrReachedThrough() {
  ResetCallCounters();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model registers");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Vector3 Object;
  Luna::LifetimeHandle Handle;
  Actor Character;
  Luna::LifetimeHandle CharacterHandle;
  Check(ExposeBorrowed(Owner, "Studio.Sample", &Object, Handle).Published &&
            ExposeValue(Owner, "Studio.Actor", "Studio.Character", &Character,
                        OwnershipModel::Borrowed, CharacterHandle, nullptr,
                        ClassAllocator())
                .Published,
        "one value of each class is exposed");

  Check(ScriptResult(Owner, "Result = 0\n"
                            "local Observed = getmetatable(Studio.Sample)\n"
                            "if type(Observed) == 'string' and Observed == "
                            "'Studio.Vector3' then Result = 1 end\n") == 1,
        "the class metatable is protected from script inspection");

  Check(ScriptResult(
            Owner, "Result = 0\n"
                   "if not pcall(setmetatable, Studio.Sample, {}) then Result "
                   "= Result + 1 end\n"
                   "if not pcall(setmetatable, Studio.Character, {}) then "
                   "Result = Result + 1 end\n"
                   "if not pcall(setmetatable, Studio.Sample, nil) then "
                   "Result = Result + 1 end\n") == 3,
        "the class metatable is protected from script replacement");

  Check(ScriptResult(
            Owner, "Result = 0\n"
                   "local Stolen = getmetatable(Studio.Sample)\n"
                   "local Forged = newproxy(true)\n"
                   "if not pcall(setmetatable, Forged, Stolen) then Result = "
                   "Result + 1 end\n"
                   "local Table = setmetatable({}, { __type = "
                   "'Studio.Vector3', __metatable = 'Studio.Vector3' })\n"
                   "if typeof(Table) == 'table' then Result = Result + 1 end\n"
                   "local Claimed = getmetatable(Forged)\n"
                   "Claimed.__type = 'Studio.Vector3'\n"
                   "Claimed.__metatable = 'Studio.Vector3'\n"
                   "Forgery = Table\n"
                   "Impostor = Forged\n") == 2,
        "a script can name the class but never carry its metatable");

  Check(!ReadValue(Owner, "Forgery", &Object).ReachedNativeCode,
        "a table that claims the class name never reaches native code");
  Check(!ReadValue(Owner, "Impostor", &Object).ReachedNativeCode,
        "a proxy that claims the class name never reaches native code");

  Check(ScriptResult(
            Owner, "Result = 0\n"
                   "if not pcall(function() return Studio.Sample.X end) then "
                   "Result = Result + 1 end\n"
                   "if not pcall(function() Studio.Sample.X = 9 end) then "
                   "Result = Result + 1 end\n"
                   "if not pcall(function() return #Studio.Sample end) then "
                   "Result = Result + 1 end\n"
                   "if not pcall(function() return Studio.Sample + 1 end) "
                   "then Result = Result + 1 end\n"
                   "if not pcall(function() return tostring(Studio.Sample) .. "
                   "Studio.Sample end) then Result = Result + 1 end\n") == 5,
        "no script operation reaches through the class metatable");
  Check(Object.X == 1.0 && Object.Y == 2.0 && Object.Z == 3.0,
        "no refused script operation ever touched the native object");

  Check(Hooks::ClassMetatableCreationCount(Owner, "Studio.Vector3") == 1 &&
            Hooks::ClassMetatableCreationCount(Owner, "Studio.Actor") == 1,
        "no script attempt created or replaced a class metatable");
  Check(ReadValue(Owner, "Studio.Sample", &Object).DeliveredExpectedObject,
        "the protected value still reaches native code afterwards");
  Check(ScriptResult(Owner, "Result = 0\nif typeof(Studio.Sample) == "
                            "'Studio.Vector3' then Result = 1 end") == 1,
        "the protected value still carries its class metatable");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every protection attempt restores the exact root stack depth");
  Check(Succeeds(Owner, "assert(Studio.Mark() == 1, 'reuse')"),
        "the State stays reusable after every protection attempt");
}

void CheckConstructedObjectsAreObtainedFromScript() {
  ResetCallCounters();
  ResetStorageCounters();
  ResetConstructionCounters();
  {
    Luna::State Owner;
    Check(Owner.IsReady() && RegisterConstructionModel(Owner),
          "one plan publishes the class, its construction surface, and the "
          "callables");
    const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

    const Luna::ReflectionSnapshot Snapshot = Owner.Bindings().Reflection();
    const Luna::ReflectionRecord Class = Snapshot.Find("Studio.Particle");
    Check(Class.IsValid() && Class.Kind() == Luna::SymbolKind::Class &&
              Class.Documentation() == "One simulated particle.",
          "the class is reflected inside the namespace that declared it");
    const Luna::ReflectionRecord Constructors =
        Snapshot.Find("Studio.Particle.New");
    Check(Constructors.IsValid() &&
              Constructors.Kind() == Luna::SymbolKind::OverloadSet,
          "both constructors form one reflected overload set");
    Check(Hooks::OverloadCandidateCount(Owner, "Studio.Particle.New") == 2,
          "the overload set publishes exactly two candidates");
    Check(
        ParticleConstructed == 0 && Hooks::PublishedUserdataCount(Owner) == 0,
        "registering a construction surface constructs and publishes nothing");

    const Luna::ReflectionRecordRange Candidates =
        Snapshot.Symbols(Luna::SymbolKind::Constructor);
    bool EveryConstructorIsOwned = Candidates.Size() == 2;
    for (std::size_t Index = 0; Index < Candidates.Size(); ++Index) {
      const Luna::ReflectionRecord Candidate = Candidates.At(Index);
      if (Candidate.OwnershipResult() != "lua-owned" ||
          Candidate.AllocatorPolicy() != "Studio.ParticleArena" ||
          Candidate.Returns() != Luna::ReturnShape::Scalar)
        EveryConstructorIsOwned = false;
    }
    Check(EveryConstructorIsOwned,
          "every constructor reflects one Lua-owned value from the selected "
          "storage protocol");

    Check(Succeeds(Owner, "Made = Studio.Particle.New(3)\n"
                          "Plain = Studio.Particle.New()\n"
                          "Heavy = Studio.Particle.Heavy(5)\n"
                          "Boxed = Studio.Particle.Boxed()\n"
                          "Engine = Studio.Particle.Engine()\n"
                          "Holder = { Value = Made }\n"
                          "Alias = Engine\n"),
          "every construction candidate is callable from Luau");
    Check(ParticleLive == 5 && FactoryCalls == 2,
          "each call produced exactly one live object");
    Check(Hooks::PublishedUserdataCount(Owner) == 5 &&
              Hooks::LiveCachedIdentityCount(Owner) == 5,
          "each obtained object is exactly one published value and one entry");

    const Luna::Detail::ConstructionCounters Built =
        Hooks::UserdataConstructionCounters(Owner);
    Check(
        Built.Allocate == 3 && Built.Construct == 3 &&
            Built.AllocationFailure == 0 && Built.ConstructionFailure == 0,
        "exactly the created values were allocated and constructed once each");

    Check(
        ScriptResult(Owner,
                     "Result = 0\n"
                     "if Holder.Value == Made then Result = Result + 1 end\n"
                     "if Alias == Engine then Result = Result + 1 end\n"
                     "if Made ~= Plain and Made ~= Heavy then Result = "
                     "Result + 1 end\n"
                     "if typeof(Made) == 'Studio.Particle' and "
                     "typeof(Boxed) == 'Studio.Particle' then Result = "
                     "Result + 1 end\n") == 4,
        "obtained values are one script value per object, typed by the class");
    Check(Hooks::ClassMetatableCreationCount(Owner, "Studio.Particle") == 1,
          "every obtained value reuses the one metatable of its class");

    Check(ReadValue(Owner, "Engine", EngineParticle(), "Studio.Particle")
              .DeliveredExpectedObject,
          "the borrowed object reaches native code through its value");
    Check(ReadValue(Owner, "Holder.Value", nullptr, "Studio.Particle")
              .ReachedNativeCode,
          "a constructed object stored in a script table reaches native code");

    const std::size_t ConstructedBeforeRefusals = ParticleConstructed;
    const std::string Unconvertible =
        Failure(Owner, "return Studio.Particle.New('heavy')");
    Check(Contains(Unconvertible, "Studio.Particle.New") &&
              Contains(Unconvertible, "argument 1"),
          "a construction whose argument cannot convert names the position");
    const std::string Thrown =
        Failure(Owner, "return Studio.Particle.Refused(1)");
    Check(Contains(Thrown, "no particle today"),
          "a factory that throws is translated into a deterministic failure");
    const std::string Arity =
        Failure(Owner, "return Studio.Particle.New(1, 2, 3)");
    Check(!Arity.empty(),
          "an argument count no constructor accepts is refused");
    Check(ParticleConstructed == ConstructedBeforeRefusals &&
              ParticleLive == 5 && Hooks::PublishedUserdataCount(Owner) == 5,
          "no refused construction constructed or published anything");
    Check(RestoredCheckpoint(Owner),
          "the last refused construction restored the callback checkpoint");
    Check(Failure(Owner, "return Studio.Particle.New('heavy')") ==
              Unconvertible,
          "one construction failure family reports one identical message");
    Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
          "every construction and refusal restores the root stack depth");

    Check(Succeeds(Owner, "Made = nil\nPlain = nil\nHeavy = nil\nBoxed = nil\n"
                          "Holder = nil\nAlias = nil\nEngine = nil\n"),
          "the script drops every reference it held");
    Check(Hooks::CollectGarbage(Owner), "the collector runs to completion");
    Check(DestroyCalls == 3 && DeallocateCalls == 3,
          "each created value is destroyed once and its storage given back "
          "once");
    const Luna::Detail::ReleaseCounters Released =
        Hooks::UserdataReleaseCounters(Owner);
    Check(Released.SharedRelease == 1,
          "exactly one shared ownership reference is released");
    Check(Released.IncompleteMetadata == 0,
          "every cleanup step ran with the metadata it needs");
    Check(Hooks::ObserveUserdataCollections().ContainedException == 0,
          "nothing was thrown at the collection boundary");
    Check(EngineParticle()->Mass == 11.0 && ParticleLive == 1,
          "the engine's own object survives collection of its value");

    Check(Succeeds(Owner,
                   "assert(Studio.Scale(4) == 8, 'scalar')\n"
                   "Again = Studio.Particle.New(2)\n"
                   "assert(typeof(Again) == 'Studio.Particle', 'again')"),
          "the State keeps constructing and invoking after every refusal");
    Check(ParticleConstructed == ConstructedBeforeRefusals + 1 &&
              ParticleLive == 2,
          "the recovered construction produced exactly one more live object");
    Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
          "the recovered construction restores the root stack depth");
  }
  Check(DestroyCalls == 4 && DeallocateCalls == 4,
        "State destruction releases the value it still owned exactly once");
  Check(ParticleLive == 1,
        "only the engine's own object outlives the State that borrowed it");
  Hooks::ResetUserdataCollections();
}

void CheckExposedValuesSurviveStateMoves() {
  ResetCallCounters();
  ResetStorageCounters();
  Luna::State First;
  Check(First.IsReady() && RegisterModel(First), "the model registers");

  Vector3 Borrowed;
  Luna::LifetimeHandle Handle;
  Vector3 *Owned = AllocateVector();
  Check(ExposeBorrowed(First, "Studio.Borrowed", &Borrowed, Handle).Published &&
            ExposeValue(First, "Studio.Vector3", "Studio.Owned", Owned,
                        OwnershipModel::LuaOwned,
                        Luna::LifetimeHandle::Undeclared(), nullptr,
                        OwnedStorageProtocol())
                .Published,
        "both values are exposed before the move");
  Check(Succeeds(First, "Alias = Studio.Borrowed"),
        "the script aliases a value before the move");

  Luna::State Moved = std::move(First);
  Check(Moved.IsReady(), "the moved State is ready");
  Check(Hooks::PublishedUserdataCount(Moved) == 2 &&
            Hooks::LiveCachedIdentityCount(Moved) == 2,
        "a State move preserves every exposed value and cache entry");
  Check(ReadValue(Moved, "Studio.Borrowed", &Borrowed).DeliveredExpectedObject,
        "a value exposed before the move still reaches native code after it");
  Check(ReadValue(Moved, "Alias", &Borrowed).DeliveredExpectedObject,
        "the script's alias still delivers exactly the same object");
  Check(ScriptResult(Moved, "Result = 0\nif Alias == Studio.Borrowed then "
                            "Result = 1 end") == 1,
        "script identity of an exposed value survives a State move");

  Check(ExposeBorrowed(Moved, "Studio.Again", &Borrowed, Handle).Published,
        "re-exposing after the move publishes a value");
  Check(Hooks::LiveCachedIdentityCount(Moved) == 2,
        "re-exposing after the move records no second entry");
  Check(ScriptResult(Moved, "Result = 0\nif Studio.Again == Studio.Borrowed "
                            "then Result = 1 end") == 1,
        "a reused exposure after the move is the value that already existed");
  Check(Succeeds(Moved, "assert(Studio.Scale(2) == 4, 'reuse')"),
        "the moved State keeps invoking its callables");

  Handle.Invalidate();
  Check(!ReadValue(Moved, "Studio.Borrowed", &Borrowed).ReachedNativeCode,
        "invalidation still ends access through the moved State");
  Check(DestroyCalls == 0,
        "nothing is released while the moved State is still usable");
  {
    Luna::State Consumed = std::move(Moved);
    Check(Consumed.IsReady(), "the State moves again");
  }
  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "destruction after two moves releases the value exactly once");
}

} // namespace

int RunClassUserdataIntegrationTests() {
  FailureCount = 0;
  Hooks::ResetUserdataCollections();
  CheckExposedObjectsAreConsumedFromScript();
  CheckAccessFailsBeforeNativeCode();
  CheckInvalidScriptVisibleValuesNeverReachNativeCode();
  CheckClassMetatableCannotBeReplacedForgedOrReachedThrough();
  CheckConstructedObjectsAreObtainedFromScript();
  CheckExposedValuesSurviveStateMoves();
  Hooks::ResetUserdataCollections();
  return FailureCount == 0 ? 0 : 1;
}
