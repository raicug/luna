// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/test_hooks.hpp"
#include "state/userdata/access.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/identity.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::Detail::ConstAccess;
using Luna::Detail::LifetimeState;
using Luna::Detail::MetatableId;
using Luna::Detail::OwnershipModel;
using Luna::Detail::UserdataAccessFailure;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "userdata access check failed: " << Description << '\n';
}

struct Vector3 final {
  double X = 0.0;
  double Y = 0.0;
  double Z = 0.0;
};

class Actor {
public:
  virtual ~Actor() = default;
  int Health = 100;
};

[[nodiscard]] std::uint64_t ProbeGeneration(const void *Record) noexcept {
  return Record != nullptr ? *static_cast<const std::uint64_t *>(Record) : 0;
}

[[nodiscard]] Luna::RegistrationResult
RegisterClasses(Luna::BindingRegistry &Registry) {
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Vector3> Vector = Studio.RegisterClass<Vector3>(
      "Vector3", Luna::StableTypeKey("Studio.Vector3"));
  const Luna::RegistrationResult Vectors = Vector.Commit();
  if (!Vectors.IsSuccess())
    return Vectors;

  Luna::ClassBuilder<Actor> Character = Registry.RegisterClass<Actor>(
      "Actor", Luna::StableTypeKey("Studio.Actor"));
  return Character.Commit();
}

[[nodiscard]] std::string
ExposeBorrowed(Luna::State &Owner, const std::string &Path, void *Storage,
               const std::uint64_t *Generation,
               std::string_view QualifiedName = "Studio.Vector3",
               ConstAccess Access = ConstAccess::Mutable,
               OwnershipModel Ownership = OwnershipModel::Borrowed) {
  Luna::Detail::ClassExposureRequest Request;
  Request.QualifiedName = std::string(QualifiedName);
  Request.Path = Path;
  Request.Storage = Storage;
  Request.Ownership = Ownership;
  Request.Access = Access;
  Request.LifetimeGeneration = Generation;
  const Luna::Detail::ClassExposureObservation Exposed =
      Hooks::ExposeClassUserdata(Owner, Request);
  return Exposed.Status;
}

[[nodiscard]] Luna::Detail::ClassAccessObservation
ReadHandle(Luna::State &Owner, const std::string &Path, const void *Expected,
           std::string_view QualifiedName = "Studio.Vector3") {
  Luna::Detail::ClassAccessRequest Request;
  Request.QualifiedName = std::string(QualifiedName);
  Request.Path = Path;
  Request.ExpectedStorage = Expected;
  return Hooks::AccessClassUserdata(Owner, Request);
}

[[nodiscard]] int ScriptResult(Luna::State &Owner, const std::string &Source) {
  if (!Owner.Execute(Source).IsSuccess())
    return -1;
  const auto Observed = Hooks::ObserveIntegerGlobal(Owner, "Result");
  return Observed ? *Observed : -1;
}

void CheckAccessValidationOrderIsDeterministic() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterClasses(Registry).IsSuccess(), "both classes register");

  const auto Identity = Hooks::LogicalIdentityOf(Owner);
  const auto VectorType = Hooks::ClassTypeOf(Owner, "Studio.Vector3");
  const auto ActorType = Hooks::ClassTypeOf(Owner, "Actor");
  const auto VectorMetatable =
      Hooks::ClassMetatableIdentityOf(Owner, "Studio.Vector3");
  const auto ActorMetatable = Hooks::ClassMetatableIdentityOf(Owner, "Actor");
  Check(Identity && VectorType && ActorType && VectorMetatable &&
            ActorMetatable,
        "both registered classes own a complete per-State identity");
  if (!Identity || !VectorType || !ActorType || !VectorMetatable ||
      !ActorMetatable)
    return;

  Vector3 Object;
  std::uint64_t Generation = 7;

  Luna::Detail::UserdataAccessRequest Request;
  Request.Origin = *Identity;
  Request.Metatable = MetatableId::FromValue(*VectorMetatable);
  Request.RequestedType = *VectorType;
  Request.HandleProbe = &ProbeGeneration;

  Luna::Detail::UserdataAccessRequest Incomplete;
  const auto Unavailable =
      Luna::Detail::InspectUserdataAccess(&Object, sizeof(Object), Incomplete);
  Check(Unavailable.Failure == UserdataAccessFailure::UnavailableRequest &&
            Unavailable.Storage == nullptr,
        "an incomplete access request is refused before any value is read");

  Check(Luna::Detail::InspectUserdataAccess(nullptr, 0, Request).Failure ==
            UserdataAccessFailure::MissingValue,
        "an absent value fails as a missing value");

  auto Header = Hooks::DescribeClassUserdata(
      Owner, "Studio.Vector3", OwnershipModel::Borrowed, ConstAccess::Mutable);
  Check(Header.has_value(), "the registered class describes one header");
  if (!Header)
    return;

  Check(Luna::Detail::InspectUserdataAccess(&*Header, sizeof(*Header) - 1,
                                            Request)
                .Failure == UserdataAccessFailure::ForeignLayout,
        "a block too small to hold the header is foreign");

  Luna::Detail::UserdataHeader Broken = *Header;
  Broken.Magic = 0;
  Broken.Origin = Luna::Detail::StateIdentity();
  Broken.Metatable = MetatableId::FromValue(*ActorMetatable);
  Broken.DynamicType = *ActorType;
  Broken.DeclaredViewType = *ActorType;
  Broken.Access = ConstAccess::Const;
  Request.RequiresMutation = true;
  Check(Luna::Detail::ValidateUserdataAccess(Broken, Request).Failure ==
            UserdataAccessFailure::ForeignLayout,
        "the layout check runs before every other check");

  Broken.Magic = Luna::Detail::UserdataMagic;
  Check(Luna::Detail::ValidateUserdataAccess(Broken, Request).Failure ==
            UserdataAccessFailure::ForeignState,
        "the origin check runs before the metatable check");

  Broken.Origin = Header->Origin;
  Check(Luna::Detail::ValidateUserdataAccess(Broken, Request).Failure ==
            UserdataAccessFailure::MetatableMismatch,
        "a wrong metatable identity is refused before the lifetime check");

  Broken.Metatable = Header->Metatable;
  Check(Luna::Detail::ValidateUserdataAccess(Broken, Request).Failure ==
            UserdataAccessFailure::NullPayload,
        "a null payload is refused before the lifetime handle check");

  Broken.Payload.Storage = &Object;
  Check(Luna::Detail::ValidateUserdataAccess(Broken, Request).Failure ==
            UserdataAccessFailure::MissingLifetimeHandle,
        "a borrowed value without its explicit handle is refused");

  Broken.Handle.Record = &Generation;
  Broken.Handle.Generation = Generation - 1;
  Check(Luna::Detail::ValidateUserdataAccess(Broken, Request).Failure ==
            UserdataAccessFailure::ExpiredLifetimeHandle,
        "an invalidated lifetime handle expires every later access");

  Broken.Handle.Generation = Generation;
  Check(Luna::Detail::ValidateUserdataAccess(Broken, Request).Failure ==
            UserdataAccessFailure::Unpublished,
        "a value that was never published permits no access");

  Broken.Lifetime = LifetimeState::Invalid;
  Check(Luna::Detail::ValidateUserdataAccess(Broken, Request).Failure ==
            UserdataAccessFailure::Invalidated,
        "an invalidated value permits no access");

  Broken.Lifetime = LifetimeState::Destroyed;
  Check(Luna::Detail::ValidateUserdataAccess(Broken, Request).Failure ==
            UserdataAccessFailure::Destroyed,
        "a destroyed value permits no access");

  Broken.Lifetime = LifetimeState::SharedReleased;
  Check(Luna::Detail::ValidateUserdataAccess(Broken, Request).Failure ==
            UserdataAccessFailure::Released,
        "a released shared reference permits no access");

  Broken.Lifetime = LifetimeState::Published;
  Check(
      Luna::Detail::ValidateUserdataAccess(Broken, Request).Failure ==
          UserdataAccessFailure::TypeMismatch,
      "a value of another registered class is refused before the const check");

  Broken.DynamicType = *VectorType;
  Broken.DeclaredViewType = *VectorType;
  Check(Luna::Detail::ValidateUserdataAccess(Broken, Request).Failure ==
            UserdataAccessFailure::ConstViolation,
        "a mutating access is refused at a const view");

  const auto Permitted = Luna::Detail::ValidateUserdataAccess(Broken, Request);
  Check(!Permitted.IsPermitted() && Permitted.Storage == nullptr,
        "no refused access ever yields a native pointer");

  Broken.Access = ConstAccess::Mutable;
  const auto Granted = Luna::Detail::ValidateUserdataAccess(Broken, Request);
  Check(Granted.Failure == UserdataAccessFailure::None && Granted.IsPermitted(),
        "a complete, live, correctly typed value is permitted");
  Check(Granted.Storage == &Object && Granted.PermitsMutation,
        "a permitted access hands out exactly the exposed object");

  Request.RequiresMutation = false;
  Broken.Access = ConstAccess::Const;
  const auto Reading = Luna::Detail::ValidateUserdataAccess(Broken, Request);
  Check(Reading.IsPermitted() && !Reading.PermitsMutation,
        "a const view permits a non-mutating access without granting mutation");
}

void CheckExposedValuesReachNativeCode() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterClasses(Registry).IsSuccess(), "both classes register");

  Vector3 Object;
  std::uint64_t Generation = 1;
  Check(ExposeBorrowed(Owner, "Sample", &Object, &Generation) == "created",
        "one borrowed object is exposed as one new value");

  const auto Exposed = ReadHandle(Owner, "Sample", &Object);
  Check(Exposed.ReachedNativeCode && Exposed.DeliveredExpectedObject,
        "a validated access hands native code exactly the exposed object");
  Check(Exposed.PermitsMutation,
        "a mutable view reaches native code as a mutable handle");

  const auto Header = Hooks::ObserveClassUserdata(Owner, "Sample");
  Check(Header && Header->HasCanonicalLayout() && Header->IdentifiesClass(),
        "the exposed value carries one complete Luna header");
  Check(
      Header && Header->Lifetime == LifetimeState::Published,
      "an exposed value is published only once its metatable, path, and cache "
      "entry are all in place");
  Check(Header && Header->Ownership == OwnershipModel::Borrowed &&
            Header->Handle.IsDeclared(),
        "a borrowed value records its explicit lifetime handle");
  Check(Header && Header->Identity.IsValid(),
        "an exposed value records one native identity");

  Check(ScriptResult(Owner, "Result = 0\nif typeof(Sample) == 'Studio.Vector3' "
                            "then Result = 1 end") == 1,
        "an exposed value carries the class metatable");
  Check(ScriptResult(Owner, "Result = 0\nif getmetatable(Sample) == "
                            "'Studio.Vector3' then Result = 1 end") == 1,
        "the class metatable is protected from script inspection");
  Check(ScriptResult(Owner, "Result = 0\nlocal Ok = pcall(setmetatable, "
                            "Sample, {})\nif not Ok then Result = 1 end") == 1,
        "the class metatable is protected from script replacement");

  Vector3 Second;
  Check(ExposeBorrowed(Owner, "Other", &Second, &Generation) == "created",
        "a second object of the same class is exposed as its own value");
  Check(ScriptResult(Owner, "Result = 0\nif typeof(Other) == typeof(Sample) "
                            "then Result = 1 end") == 1,
        "every value of one class shares one metatable");
  Check(Hooks::LiveCachedIdentityCount(Owner) == 2,
        "two distinct objects record two live cache entries");

  Check(ExposeBorrowed(Owner, "Alias", &Object, &Generation) == "reused",
        "re-exposing one object hands the existing value back");
  Check(ScriptResult(Owner, "Result = 0\nif Alias == Sample then Result = 1 "
                            "end") == 1,
        "a reused exposure is exactly the value that already existed");
  Check(Hooks::LiveCachedIdentityCount(Owner) == 2,
        "reuse records no second entry for one object");
}

void CheckIdentityCacheReusesAndRefusesConflicts() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterClasses(Registry).IsSuccess(), "both classes register");

  Vector3 Object;
  std::uint64_t Generation = 1;
  Luna::Detail::ClassExposureRequest First;
  First.QualifiedName = "Studio.Vector3";
  First.Path = "Sample";
  First.Storage = &Object;
  First.LifetimeGeneration = &Generation;
  const auto Created = Hooks::ExposeClassUserdata(Owner, First);
  Check(Created.Created && Created.Nonce != 0,
        "the first exposure creates one value with one state-local nonce");

  const auto Reused = Hooks::ExposeClassUserdata(Owner, First);
  Check(Reused.Reused && Reused.Nonce == Created.Nonce,
        "a reused exposure keeps the identity it was first exposed with");

  Check(ExposeBorrowed(Owner, "Owned", &Object, &Generation, "Studio.Vector3",
                       ConstAccess::Mutable,
                       OwnershipModel::LuaOwned) == "conflicting_ownership",
        "re-exposing one object under another ownership model is refused");
  Check(ExposeBorrowed(Owner, "Frozen", &Object, &Generation, "Studio.Vector3",
                       ConstAccess::Const) == "incompatible_access",
        "re-exposing one object through another view permission is refused");
  Check(ExposeBorrowed(Owner, "Wrong", &Object, &Generation, "Actor") ==
            "incompatible_type",
        "re-exposing one object as another registered class is refused");
  Check(Hooks::LiveCachedIdentityCount(Owner) == 1 &&
            Hooks::CachedIdentityCount(Owner) == 1,
        "no refused re-exposure records a second entry for one object");
  Check(ScriptResult(Owner, "Result = 0\nif Owned == nil and Frozen == nil and "
                            "Wrong == nil then Result = 1 end") == 1,
        "no refused re-exposure publishes a value");

  Vector3 Unhandled;
  Check(ExposeBorrowed(Owner, "Unhandled", &Unhandled, nullptr) ==
            "missing_lifetime_handle",
        "a borrowed exposure without its lifetime handle is refused");
  Check(Hooks::LiveCachedIdentityCount(Owner) == 1,
        "a refused exposure records nothing");

  Vector3 Owned;
  Check(ExposeBorrowed(Owner, "Value", &Owned, nullptr, "Studio.Vector3",
                       ConstAccess::Mutable,
                       OwnershipModel::LuaOwned) == "created",
        "a Lua-owned value needs no lifetime handle");
  Check(Hooks::LiveCachedIdentityCount(Owner) == 2,
        "a distinct object records its own entry");
}

void CheckStaleAndForeignValuesNeverReachNativeCode() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterClasses(Registry).IsSuccess(), "both classes register");

  Vector3 Object;
  std::uint64_t Generation = 1;
  Check(ExposeBorrowed(Owner, "Sample", &Object, &Generation) == "created",
        "one borrowed object is exposed");
  Check(ReadHandle(Owner, "Sample", &Object).ReachedNativeCode,
        "the exposed value reaches native code while it is live");

  const auto WrongClass = ReadHandle(Owner, "Sample", &Object, "Actor");
  Check(!WrongClass.ReachedNativeCode &&
            WrongClass.Failure == "userdata_type_mismatch",
        "a value of another class never reaches native code");

  ++Generation;
  const auto Expired = ReadHandle(Owner, "Sample", &Object);
  Check(!Expired.ReachedNativeCode && Expired.Failure == "expired_userdata",
        "an invalidated lifetime handle stops every later access");
  Check(Expired.Diagnostic.find("expired_lifetime_handle") != std::string::npos,
        "the refusal names the exact reason the access expired");
  --Generation;
  Check(ReadHandle(Owner, "Sample", &Object).ReachedNativeCode,
        "restoring the handle generation makes the value accessible again");

  Check(Hooks::RetireClassUserdata(Owner, &Object),
        "retiring one exposed value succeeds");
  Check(Hooks::LiveCachedIdentityCount(Owner) == 0 &&
            Hooks::CachedIdentityCount(Owner) == 0,
        "a retired value leaves no cache entry behind");
  const auto Retired = ReadHandle(Owner, "Sample", &Object);
  Check(!Retired.ReachedNativeCode && Retired.Failure == "expired_userdata",
        "a retired value never reaches native code again");
  const auto RetiredHeader = Hooks::ObserveClassUserdata(Owner, "Sample");
  Check(RetiredHeader && RetiredHeader->Lifetime == LifetimeState::Invalid,
        "retirement invalidates access before anything is released");
  Check(Hooks::RetireClassUserdata(Owner, &Object) ||
            Hooks::LiveCachedIdentityCount(Owner) == 0,
        "retiring one value twice is harmless");

  const std::uint64_t Reborn = 1;
  Luna::Detail::ClassExposureRequest Again;
  Again.QualifiedName = "Studio.Vector3";
  Again.Path = "Reborn";
  Again.Storage = &Object;
  Again.LifetimeGeneration = &Reborn;
  const auto Recreated = Hooks::ExposeClassUserdata(Owner, Again);
  Check(Recreated.Created && Recreated.Nonce != 0,
        "recycled storage is exposed as one new identity");
  Check(ReadHandle(Owner, "Reborn", &Object).DeliveredExpectedObject,
        "the new value reaches native code");
  Check(!ReadHandle(Owner, "Sample", &Object).ReachedNativeCode,
        "the retired value stays unreachable after the storage is exposed "
        "again");

  Check(Owner.Execute("Sample = {}").IsSuccess(),
        "the script replaces the path with its own table");
  const auto Foreign = ReadHandle(Owner, "Sample", &Object);
  Check(!Foreign.ReachedNativeCode && Foreign.Failure == "foreign_userdata",
        "a script-created value never reaches native code");
  Check(Foreign.Diagnostic.find("table") != std::string::npos,
        "the refusal names what actually arrived");
  const auto Absent = ReadHandle(Owner, "Missing", &Object);
  Check(!Absent.ReachedNativeCode && Absent.Failure == "foreign_userdata",
        "an absent value never reaches native code");
}

void CheckClassMetatableIsCreatedOnceAndRetained() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterClasses(Registry).IsSuccess(), "both classes register");

  Check(!Hooks::ClassMetatableIsCreated(Owner, "Studio.Vector3") &&
            !Hooks::ClassMetatableIsCreated(Owner, "Actor"),
        "registering a class creates no metatable");
  Check(Hooks::ClassMetatableIdentityOf(Owner, "Studio.Vector3").has_value(),
        "the metatable identity exists before the table does");

  Vector3 First;
  std::uint64_t Generation = 1;
  Check(ExposeBorrowed(Owner, "First", &First, &Generation) == "created",
        "the first value of the class is exposed");
  Check(Hooks::ClassMetatableIsCreated(Owner, "Studio.Vector3") &&
            Hooks::ClassMetatableCreationCount(Owner, "Studio.Vector3") == 1,
        "the first exposure creates exactly one metatable");
  Check(!Hooks::ClassMetatableIsCreated(Owner, "Actor"),
        "exposing one class creates no metatable for another");

  Vector3 Second;
  Check(ExposeBorrowed(Owner, "Second", &Second, &Generation) == "created",
        "a second value of the class is exposed");
  Check(ExposeBorrowed(Owner, "Alias", &First, &Generation) == "reused",
        "the first object is exposed again");
  Check(Hooks::ClassMetatableCreationCount(Owner, "Studio.Vector3") == 1,
        "every later value of one class reuses the retained metatable");

  Check(Hooks::RetireClassUserdata(Owner, &Second),
        "one exposed value is retired");
  Check(Owner.Execute("Second = nil\nAlias = nil").IsSuccess(),
        "the script drops its references to those values");
  Check(Hooks::CollectGarbage(Owner) && Hooks::CollectGarbage(Owner),
        "the collector runs to completion");
  Check(Hooks::ClassMetatableIsCreated(Owner, "Studio.Vector3") &&
            Hooks::ClassMetatableCreationCount(Owner, "Studio.Vector3") == 1,
        "collecting every value of a class keeps its metatable retained");

  Vector3 Third;
  Check(ExposeBorrowed(Owner, "Third", &Third, &Generation) == "created",
        "a value exposed after collection reuses the retained metatable");
  Check(Hooks::ClassMetatableCreationCount(Owner, "Studio.Vector3") == 1,
        "the metatable is created exactly once per class per State");
  Check(ReadHandle(Owner, "Third", &Third).DeliveredExpectedObject,
        "the value exposed after collection reaches native code");
  Check(ScriptResult(Owner, "Result = 0\nif typeof(Third) == typeof(First) "
                            "then Result = 1 end") == 1,
        "the retained metatable still types every value of the class");
}

void CheckAccessAndCacheAreIsolatedByState() {
  Luna::State First;
  Luna::State Second;
  Luna::BindingRegistry FirstBindings = First.Bindings();
  Luna::BindingRegistry SecondBindings = Second.Bindings();
  Check(RegisterClasses(FirstBindings).IsSuccess(),
        "the first State registers both classes");
  Check(RegisterClasses(SecondBindings).IsSuccess(),
        "the second State registers both classes");

  Vector3 Object;
  std::uint64_t Generation = 1;
  Check(ExposeBorrowed(First, "Sample", &Object, &Generation) == "created",
        "the first State exposes the object");
  Check(ExposeBorrowed(Second, "Sample", &Object, &Generation) == "created",
        "the second State exposes the same object as its own value");
  Check(Hooks::LiveCachedIdentityCount(First) == 1 &&
            Hooks::LiveCachedIdentityCount(Second) == 1,
        "each State records the exposure in its own cache");
  Check(ReadHandle(First, "Sample", &Object).DeliveredExpectedObject &&
            ReadHandle(Second, "Sample", &Object).DeliveredExpectedObject,
        "each State reaches native code through its own value");

  const auto Exposed = Hooks::ObserveClassUserdata(First, "Sample");
  const auto FirstIdentity = Hooks::LogicalIdentityOf(First);
  const auto SecondIdentity = Hooks::LogicalIdentityOf(Second);
  const auto SecondType = Hooks::ClassTypeOf(Second, "Studio.Vector3");
  const auto SecondMetatable =
      Hooks::ClassMetatableIdentityOf(Second, "Studio.Vector3");
  Check(Exposed && FirstIdentity && SecondIdentity && SecondType &&
            SecondMetatable,
        "both States describe one complete per-State class identity");
  if (!Exposed || !FirstIdentity || !SecondIdentity || !SecondType ||
      !SecondMetatable)
    return;
  Check(*FirstIdentity != *SecondIdentity,
        "two live States never share one logical identity");

  Luna::Detail::UserdataAccessRequest Foreign;
  Foreign.Origin = *SecondIdentity;
  Foreign.Metatable = MetatableId::FromValue(*SecondMetatable);
  Foreign.RequestedType = *SecondType;
  Foreign.HandleProbe = &ProbeGeneration;
  const auto Refused = Luna::Detail::ValidateUserdataAccess(*Exposed, Foreign);
  Check(Refused.Failure == UserdataAccessFailure::ForeignState &&
            Refused.Storage == nullptr,
        "a value of another State is refused before its metatable or type is "
        "trusted");

  Check(Hooks::RetireClassUserdata(First, &Object),
        "the first State retires its value");
  Check(Hooks::LiveCachedIdentityCount(First) == 0 &&
            Hooks::LiveCachedIdentityCount(Second) == 1,
        "retiring in one State leaves the other State's entry live");
  Check(!ReadHandle(First, "Sample", &Object).ReachedNativeCode,
        "the retired value never reaches native code again");
  Check(ReadHandle(Second, "Sample", &Object).DeliveredExpectedObject,
        "the other State's value stays accessible");
}

} // namespace

int RunUserdataAccessAndCacheTests() {
  FailureCount = 0;
  CheckAccessValidationOrderIsDeterministic();
  CheckExposedValuesReachNativeCode();
  CheckIdentityCacheReusesAndRefusesConflicts();
  CheckStaleAndForeignValuesNeverReachNativeCode();
  CheckClassMetatableIsCreatedOnceAndRetained();
  CheckAccessAndCacheAreIsolatedByState();
  return FailureCount == 0 ? 0 : 1;
}
