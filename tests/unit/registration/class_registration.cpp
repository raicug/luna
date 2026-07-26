// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/testing/test_hooks.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/identity.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "class registration check failed: " << Description << '\n';
}

// One ordinary value type, one type with a virtual destructor, and one type
// Luna could never release.
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

class Immortal final {
public:
  int Value = 0;

private:
  ~Immortal() = default;
};

[[nodiscard]] Luna::StableTypeKey Vector3Key() {
  return Luna::StableTypeKey("Studio.Vector3");
}

[[nodiscard]] std::string PathKind(Luna::State &Owner,
                                   const std::string &Path) {
  const auto Kind = Hooks::ObserveVmPathValueKind(Owner, Path);
  return Kind ? *Kind : std::string("<unavailable>");
}

// One registered class inside one namespace, committed as a unit.
[[nodiscard]] Luna::RegistrationResult
RegisterVector3(Luna::BindingRegistry &Registry) {
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Vector3> Class =
      Studio.RegisterClass<Vector3>("Vector3", Vector3Key());
  Luna::ClassBuilder<Vector3> &Declared =
      Class.Documentation("A three-component vector.")
          .Attribute("Category", "Math");
  return Declared.Commit();
}

void CheckClassPublishesTypeSymbolAndMetatableIdentity() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = Hooks::ObserveRootStackDepth(Owner)
                             ? *Hooks::ObserveRootStackDepth(Owner)
                             : -1;

  Check(RegisterVector3(Registry).IsSuccess(),
        "one class commits with its namespace as a unit");
  Check(PathKind(Owner, "Studio.Vector3") == "table",
        "a registered class owns one Luna table at its exact path");
  Check(Hooks::ObserveRootStackDepth(Owner) &&
            *Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "publishing a class restores the exact entry stack depth");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ReflectionRecord Class = Snapshot.Find("Studio.Vector3");
  Check(Class.IsValid() && Class.Kind() == Luna::SymbolKind::Class,
        "a class reflects one class record");
  Check(Class.Name() == "Vector3" && Class.QualifiedName() == "Studio.Vector3",
        "a class record keeps its local and qualified names");
  Check(Class.Scope().Owner() == Snapshot.Find("Studio").Id(),
        "a scoped class is reflected inside its namespace scope");
  Check(Class.Documentation() == "A three-component vector.",
        "a class reflects its documentation");
  Check(Class.AttributeCount() == 1 &&
            Class.Attribute(0).Name() == "Category" &&
            Class.Attribute(0).Value() == "Math",
        "a class reflects its attributes");
  Check(Class.Descriptor().Kind() == Luna::TypeKind::Class &&
            Class.Descriptor().Key() == Vector3Key(),
        "a class reflects its canonical class type");
  Check(Snapshot.FindType(Class.Type()).IsValid(),
        "a class contributes one canonical type to the generation");

  // The per-State half: one registered class with one cached metatable
  // identity, and no second identity for the same class.
  Check(Hooks::RegisteredClassCount(Owner) == 1,
        "one class declaration registers exactly one class");
  Check(Hooks::ClassIsRegistered(Owner, "Studio.Vector3"),
        "the registered class belongs to this logical State and generation");
  Check(Hooks::ClassTypeOf(Owner, "Studio.Vector3") == Class.Type(),
        "the registered class owns exactly the reflected canonical type");
  const auto Metatable =
      Hooks::ClassMetatableIdentityOf(Owner, "Studio.Vector3");
  Check(Metatable && *Metatable != 0,
        "a registered class owns one cached metatable identity");
  Check(Hooks::IssuedMetatableIdentityCount(Owner) == 1,
        "one class registration issues exactly one metatable identity");

  // The metatable identity is Luna's own: it is not reachable as a value at any
  // canonical path.
  Check(PathKind(Owner, "Studio.Vector3.__LunaMetatable") == "absent",
        "the metatable identity installs no virtual-machine value");
  Check(!Snapshot.Find("Studio.Vector3.__LunaMetatable").IsValid(),
        "the metatable identity contributes no reflection record");
}

void CheckClassesAreOnePerStateAndStableAcrossStates() {
  Luna::State First;
  Luna::State Second;
  Luna::BindingRegistry FirstRegistry = First.Bindings();
  Luna::BindingRegistry SecondRegistry = Second.Bindings();

  Check(RegisterVector3(FirstRegistry).IsSuccess() &&
            RegisterVector3(SecondRegistry).IsSuccess(),
        "the same class registers independently in two States");

  const auto FirstType = Hooks::ClassTypeOf(First, "Studio.Vector3");
  const auto SecondType = Hooks::ClassTypeOf(Second, "Studio.Vector3");
  Check(FirstType && SecondType && *FirstType == *SecondType,
        "the canonical class type is the same value in both States");

  // Metatable identity is state-local, so it identifies one class of one State
  // and never travels between them.
  Check(Hooks::ClassMetatableIdentityOf(First, "Studio.Vector3") &&
            Hooks::ClassMetatableIdentityOf(Second, "Studio.Vector3"),
        "each State caches its own metatable identity for the class");
  Check(Hooks::RegisteredClassCount(First) == 1 &&
            Hooks::RegisteredClassCount(Second) == 1,
        "neither State observes the other's registered class");
}

void CheckSeveralClassesGetDistinctIdentities() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Vector3> Vector =
      Studio.RegisterClass<Vector3>("Vector3", Vector3Key());
  Check(Vector.Commit().IsSuccess(), "the first class commits");

  Luna::ClassBuilder<Actor> Character = Registry.RegisterClass<Actor>(
      "Actor", Luna::StableTypeKey("Studio.Actor"));
  Check(Character.Commit().IsSuccess(),
        "a root-scope class commits immediately as its own transaction");

  Check(Hooks::RegisteredClassCount(Owner) == 2,
        "each class declaration registers exactly one class");
  const auto VectorMetatable =
      Hooks::ClassMetatableIdentityOf(Owner, "Studio.Vector3");
  const auto ActorMetatable = Hooks::ClassMetatableIdentityOf(Owner, "Actor");
  Check(VectorMetatable && ActorMetatable &&
            *VectorMetatable != *ActorMetatable,
        "two registered classes never share one metatable identity");
  Check(Hooks::ClassTypeOf(Owner, "Studio.Vector3") !=
            Hooks::ClassTypeOf(Owner, "Actor"),
        "two registered classes never share one canonical type");
}

void CheckRefusedClassesLeaveNothingBehind() {
  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Vector3> Class = Registry.RegisterClass<Vector3>(
        "Vector3", Luna::StableTypeKey("not a key"));
    const auto Result = Class.Commit();
    Check(!Result.IsSuccess(), "a class with an invalid stable key is refused");
    Check(PathKind(Owner, "Vector3") == "absent",
          "a refused class installs no table");
    Check(Hooks::RegisteredClassCount(Owner) == 0,
          "a refused class registers nothing");
    Check(Hooks::IssuedMetatableIdentityCount(Owner) == 0,
          "a refused class issues no metatable identity");
    Check(Registry.Reflection().IsEmpty(),
          "a refused class contributes no reflection record");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Immortal> Class = Registry.RegisterClass<Immortal>(
        "Immortal", Luna::StableTypeKey("Studio.Immortal"));
    const auto Result = Class.Commit();
    Check(!Result.IsSuccess(), "a class Luna could never destroy is refused");
    Check(Result.Diagnostic() && Result.Diagnostic()->Message().find(
                                     "destructible") != std::string::npos,
          "the refusal explains that Luna could never release such a value");
    Check(Hooks::RegisteredClassCount(Owner) == 0,
          "a refused class registers nothing");
  }

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Vector3> First =
        Registry.RegisterClass<Vector3>("Vector3", Vector3Key());
    Check(First.Commit().IsSuccess(), "the first class registration publishes");

    Luna::ClassBuilder<Vector3> Second =
        Registry.RegisterClass<Vector3>("Vector3", Vector3Key());
    const auto Result = Second.Commit();
    Check(!Result.IsSuccess(), "a duplicate class name is refused");
    Check(Result.Diagnostic() && Result.Diagnostic()->Category() ==
                                     Luna::ErrorCategory::DuplicateGlobalName,
          "a duplicate class name is a deterministic collision");
    Check(Hooks::RegisteredClassCount(Owner) == 1,
          "a refused duplicate leaves exactly the published class registered");
    Check(Hooks::IssuedMetatableIdentityCount(Owner) == 1,
          "a refused duplicate issues no second metatable identity");
  }

  {
    // An uncommitted builder has no effect at all.
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    {
      Luna::ClassBuilder<Vector3> Class =
          Registry.RegisterClass<Vector3>("Vector3", Vector3Key());
      static_cast<void>(Class.QualifiedName());
    }
    Check(PathKind(Owner, "Vector3") == "absent",
          "destroying an uncommitted class builder installs nothing");
    Check(Hooks::RegisteredClassCount(Owner) == 0,
          "destroying an uncommitted class builder registers nothing");
  }
}

void CheckClassIdentityIsPreservedAcrossStateMoves() {
  Luna::State Original;
  Luna::BindingRegistry Registry = Original.Bindings();
  Check(RegisterVector3(Registry).IsSuccess(), "the class publishes");

  const auto OriginalIdentity = Hooks::LogicalIdentityOf(Original);
  const auto OriginalType = Hooks::ClassTypeOf(Original, "Studio.Vector3");
  const auto OriginalMetatable =
      Hooks::ClassMetatableIdentityOf(Original, "Studio.Vector3");
  const auto OriginalHeader = Hooks::DescribeClassUserdata(
      Original, "Studio.Vector3", Luna::Detail::OwnershipModel::Borrowed,
      Luna::Detail::ConstAccess::Mutable);
  Check(OriginalHeader && OriginalHeader->HasCanonicalLayout() &&
            OriginalHeader->IdentifiesClass(),
        "a userdata header of a registered class names one complete identity");
  Check(OriginalHeader &&
            OriginalHeader->Lifetime == Luna::Detail::LifetimeState::Allocated,
        "a fresh userdata header is never published");

  Luna::State Moved(std::move(Original));
  Check(Moved.IsReady(), "the destination State is ready after the move");

  const auto MovedIdentity = Hooks::LogicalIdentityOf(Moved);
  Check(OriginalIdentity && MovedIdentity &&
            *OriginalIdentity == *MovedIdentity,
        "a State move preserves the logical State identity");
  Check(Hooks::RegisteredClassCount(Moved) == 1 &&
            Hooks::ClassIsRegistered(Moved, "Studio.Vector3"),
        "a State move preserves every registered class");
  Check(Hooks::ClassTypeOf(Moved, "Studio.Vector3") == OriginalType,
        "a State move preserves the canonical class type");
  Check(Hooks::ClassMetatableIdentityOf(Moved, "Studio.Vector3") ==
            OriginalMetatable,
        "a State move preserves the cached metatable identity");

  const auto MovedHeader = Hooks::DescribeClassUserdata(
      Moved, "Studio.Vector3", Luna::Detail::OwnershipModel::Borrowed,
      Luna::Detail::ConstAccess::Mutable);
  Check(MovedHeader && OriginalHeader &&
            MovedHeader->Origin == OriginalHeader->Origin,
        "a userdata origin identity survives a State move unchanged");
  Check(MovedHeader && OriginalIdentity &&
            MovedHeader->BelongsTo(*OriginalIdentity),
        "a value exposed before the move still belongs to its origin State");
  Check(MovedHeader && OriginalHeader &&
            MovedHeader->DynamicType == OriginalHeader->DynamicType &&
            MovedHeader->ClassSymbol == OriginalHeader->ClassSymbol &&
            MovedHeader->Metatable == OriginalHeader->Metatable,
        "a userdata header keeps its type, symbol, and metatable identity "
        "across a State move");
}

// The header is the only thing Luna trusts inside a userdata block, so its
// layout and origin checks are exact.
void CheckUserdataHeaderLayoutAndAccessState() {
  Luna::Detail::UserdataHeader Header;
  Check(Header.HasCanonicalLayout(),
        "a default header carries Luna's marker and current layout version");
  Check(!Header.IdentifiesClass(),
        "a header without identities names no registered class");
  Check(!Header.HasLiveLifetime(),
        "a header that was never published permits no native access");
  Check(Header.PermitsMutation(), "a mutable view permits mutation by default");
  Check(!Header.HasRequiredLifetimeHandle(),
        "a borrowed value without its explicit lifetime handle is incomplete");

  Header.Ownership = Luna::Detail::OwnershipModel::LuaOwned;
  Check(Header.HasRequiredLifetimeHandle(),
        "a Lua-owned value needs no lifetime handle");
  Header.Access = Luna::Detail::ConstAccess::Const;
  Check(!Header.PermitsMutation(), "a const view rejects mutation");
  Header.Lifetime = Luna::Detail::LifetimeState::Published;
  Check(Header.HasLiveLifetime(), "a published value has a live lifetime");
  Header.Lifetime = Luna::Detail::LifetimeState::Invalid;
  Check(!Header.HasLiveLifetime(),
        "an invalidated value permits no native access");

  // A block that does not carry Luna's marker and layout version is never read
  // as a Luna userdata.
  Luna::Detail::UserdataHeader Foreign;
  Foreign.Magic = 0;
  Check(!Foreign.HasCanonicalLayout(),
        "a block without Luna's marker is rejected");
  Check(Luna::Detail::InspectUserdataHeader(&Foreign, sizeof(Foreign)) ==
            nullptr,
        "inspection refuses a block that is not a Luna userdata");
  Check(Luna::Detail::InspectUserdataHeader(nullptr, 0) == nullptr,
        "inspection refuses an absent block");

  Luna::Detail::UserdataHeader Canonical;
  Check(Luna::Detail::InspectUserdataHeader(&Canonical,
                                            sizeof(Canonical) - 1) == nullptr,
        "inspection refuses a block too small to hold the header");
  Check(Luna::Detail::InspectUserdataHeader(&Canonical, sizeof(Canonical)) ==
            &Canonical,
        "inspection accepts exactly a canonical Luna userdata block");
}

void CheckClassScopesRejectForeignAndStaleUse() {
  {
    // A script-created table at the class path is a collision, never an
    // adoption.
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Check(Owner.Execute("Vector3 = {}").IsSuccess(),
          "the script creates its own table first");

    Luna::ClassBuilder<Vector3> Class =
        Registry.RegisterClass<Vector3>("Vector3", Vector3Key());
    const auto Result = Class.Commit();
    Check(!Result.IsSuccess(),
          "a class never adopts a value Luna does not own");
    Check(Hooks::RegisteredClassCount(Owner) == 0,
          "a refused class registers nothing");
  }

  {
    // A builder whose State was destroyed fails deterministically instead of
    // touching anything.
    Luna::Detail::ClassStaging Staged;
    {
      Luna::State Owner;
      Luna::BindingRegistry Registry = Owner.Bindings();
      Luna::ClassBuilder<Vector3> Class =
          Registry.RegisterClass<Vector3>("Vector3", Vector3Key());
      Staged = Luna::Detail::StageRootClassDeclaration(
          Owner, "Other", Luna::StableTypeKey("Studio.Other"),
          Luna::Detail::ClassPolicyFor<Vector3>());
      static_cast<void>(Class.QualifiedName());
    }
    const auto Result = Staged.Commit();
    Check(!Result.IsSuccess(),
          "a class builder whose State was destroyed cannot commit");
  }
}

} // namespace

int RunClassRegistrationTests() {
  FailureCount = 0;
  CheckClassPublishesTypeSymbolAndMetatableIdentity();
  CheckClassesAreOnePerStateAndStableAcrossStates();
  CheckSeveralClassesGetDistinctIdentities();
  CheckRefusedClassesLeaveNothingBehind();
  CheckClassIdentityIsPreservedAcrossStateMoves();
  CheckUserdataHeaderLayoutAndAccessState();
  CheckClassScopesRejectForeignAndStaleUse();
  return FailureCount == 0 ? 0 : 1;
}
