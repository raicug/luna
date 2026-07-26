// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_construction.hpp>
#include <luna/binding/lifetime_handle.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/testing/test_hooks.hpp"

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

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "class construction check failed: " << Description << '\n';
}

// One ordinary value type Luna can allocate, construct, and destroy, plus a
// counter so a test can prove a constructor ran exactly once and its object was
// destroyed exactly once.
std::size_t LiveCount = 0;
std::size_t ConstructedCount = 0;

struct Vector3 final {
  double X = 0.0;
  double Y = 0.0;
  double Z = 0.0;

  Vector3() {
    ++LiveCount;
    ++ConstructedCount;
  }

  Vector3(double XValue, double YValue, double ZValue)
      : X(XValue), Y(YValue), Z(ZValue) {
    ++LiveCount;
    ++ConstructedCount;
  }

  Vector3(const Vector3 &Other) : X(Other.X), Y(Other.Y), Z(Other.Z) {
    ++LiveCount;
    ++ConstructedCount;
  }

  Vector3(Vector3 &&Other) noexcept : X(Other.X), Y(Other.Y), Z(Other.Z) {
    ++LiveCount;
    ++ConstructedCount;
  }

  Vector3 &operator=(const Vector3 &) = default;
  Vector3 &operator=(Vector3 &&) noexcept = default;

  ~Vector3() { --LiveCount; }
};

// One engine-owned object a singleton accessor borrows.
Vector3 *EngineOrigin() {
  static Vector3 Origin(0.0, 0.0, 0.0);
  return &Origin;
}

[[nodiscard]] Luna::StableTypeKey Vector3Key() {
  return Luna::StableTypeKey("Studio.Vector3");
}

[[nodiscard]] Vector3 MakeScaled(double Scale) {
  return Vector3(Scale, Scale, Scale);
}

[[nodiscard]] Vector3 MakeComponents(double XValue, double YValue,
                                     double ZValue) {
  return Vector3(XValue, YValue, ZValue);
}

[[nodiscard]] std::shared_ptr<Vector3> MakeShared() {
  return std::make_shared<Vector3>(1.0, 2.0, 3.0);
}

void ResetCounters() {
  LiveCount = 0;
  ConstructedCount = 0;
}

// One registered class carrying every construction form this milestone
// declares.
[[nodiscard]] Luna::RegistrationResult
RegisterVector3(Luna::BindingRegistry &Registry) {
  Luna::ClassBuilder<Vector3> Class =
      Registry.RegisterClass<Vector3>("Vector3", Vector3Key());
  Luna::ClassBuilder<Vector3> &Declared = Class.Documentation("A vector.");
  Luna::ClassBuilder<Vector3> &WithDefault = Declared.Constructor<>();
  Luna::ClassBuilder<Vector3> &WithComponents =
      WithDefault.Constructor<double, double, double>();
  Luna::ClassBuilder<Vector3> &WithFactory =
      WithComponents.Factory("Scaled", &MakeScaled);
  Luna::ClassBuilder<Vector3> &WithShared =
      WithFactory.Factory("Boxed", &MakeShared);
  Luna::ClassBuilder<Vector3> &WithSingleton =
      WithShared.Singleton("Origin", &EngineOrigin);
  Luna::ClassBuilder<Vector3> &Documented =
      WithSingleton.Documentation("Scaled", "One uniformly scaled vector.");
  return Documented.Commit();
}

void CheckConstructionCandidatesAreReflected() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(RegisterVector3(Registry).IsSuccess(),
        "one class commits with every construction candidate as a unit");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "publishing construction candidates restores the entry stack depth");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ReflectionRecord Class = Snapshot.Find("Vector3");
  Check(Class.IsValid() && Class.Kind() == Luna::SymbolKind::Class,
        "the class itself is still reflected as one class record");

  // The two constructors share Luna's default constructor name, so they are one
  // canonical overload set with two candidates.
  const Luna::ReflectionRecord Constructors = Snapshot.Find("Vector3.New");
  Check(Constructors.IsValid() &&
            Constructors.Kind() == Luna::SymbolKind::OverloadSet,
        "constructors of one class form one reflected overload set");
  Check(Hooks::OverloadCandidateCount(Owner, "Vector3.New") == 2,
        "two constructors publish two candidates of one overload set");

  const Luna::ReflectionRecordRange Candidates =
      Snapshot.Symbols(Luna::SymbolKind::Constructor);
  Check(Candidates.Size() == 2, "each constructor reflects its own candidate");

  bool SawDefault = false;
  bool SawComponents = false;
  for (std::size_t Index = 0; Index < Candidates.Size(); ++Index) {
    const Luna::ReflectionRecord Candidate = Candidates.At(Index);
    Check(Candidate.QualifiedName() == "Vector3.New",
          "a constructor candidate keeps the canonical name of its set");
    Check(Candidate.OverloadSet() == Constructors.Id(),
          "a constructor candidate names the overload set it belongs to");
    Check(Candidate.Declaration() == Class.Id(),
          "a constructor candidate names the class that declares it");
    Check(Candidate.Returns() == Luna::ReturnShape::Scalar &&
              Candidate.ReturnCount() == 1,
          "a constructor publishes exactly one value");
    Check(Candidate.Return(0).Descriptor().Kind() == Luna::TypeKind::Class &&
              Candidate.Return(0).Descriptor().Key() == Vector3Key(),
          "a constructor reflects the registered class as its return type");
    Check(Candidate.OwnershipResult() == "lua-owned",
          "a constructor reflects its Lua-owned result");
    Check(Candidate.AllocatorPolicy() == "Luna.ConstructedStorage",
          "a constructor reflects the allocator policy identity behind it");
    if (Candidate.ParameterCount() == 0)
      SawDefault = true;
    if (Candidate.ParameterCount() == 3)
      SawComponents = true;
  }
  Check(SawDefault && SawComponents,
        "both declared constructor signatures are reflected");

  // Factories and singleton accessors are reflected by name, with the ownership
  // result their declared return type states.
  const Luna::ReflectionRecordRange Factories =
      Snapshot.Symbols(Luna::SymbolKind::Factory);
  Check(Factories.Size() == 3,
        "each factory and singleton accessor reflects its own candidate");

  const Luna::ReflectionRecord Scaled = Snapshot.Find("Vector3.Scaled");
  Check(Scaled.IsValid() && Scaled.Kind() == Luna::SymbolKind::OverloadSet,
        "one factory name owns one overload set");

  bool SawScaled = false;
  bool SawBoxed = false;
  bool SawOrigin = false;
  for (std::size_t Index = 0; Index < Factories.Size(); ++Index) {
    const Luna::ReflectionRecord Candidate = Factories.At(Index);
    if (Candidate.QualifiedName() == "Vector3.Scaled") {
      SawScaled = true;
      Check(Candidate.OwnershipResult() == "lua-owned" &&
                Candidate.AllocatorPolicy() == "Luna.ConstructedStorage",
            "a by-value factory reflects Lua ownership of its result");
      Check(Candidate.Documentation() == "One uniformly scaled vector.",
            "a construction candidate reflects its documentation");
      Check(Candidate.ParameterCount() == 1,
            "a factory reflects its ordered parameters");
    }
    if (Candidate.QualifiedName() == "Vector3.Boxed") {
      SawBoxed = true;
      Check(Candidate.OwnershipResult() == "shared" &&
                Candidate.AllocatorPolicy() == "Luna.AdoptedStorage",
            "a shared factory reflects shared ownership of its result");
    }
    if (Candidate.QualifiedName() == "Vector3.Origin") {
      SawOrigin = true;
      Check(Candidate.OwnershipResult() == "borrowed" &&
                Candidate.AllocatorPolicy() == "Luna.AdoptedStorage",
            "a singleton accessor defaults to borrowed ownership");
    }
  }
  Check(SawScaled && SawBoxed && SawOrigin,
        "every declared factory and accessor is reflected by its own name");

  // Nothing was constructed by registration itself.
  Check(ConstructedCount == 0,
        "registering construction candidates constructs no object");
  Check(Hooks::OwnedUserdataCount(Owner) == 0,
        "registering construction candidates owns no value");
}

void CheckConstructorsPublishOwnedValues() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterVector3(Registry).IsSuccess(), "the class publishes");

  const auto Result = Owner.Execute("local V = Vector3.New(1, 2, 3)\n"
                                    "return type(V)");
  Check(Result.IsSuccess(), "a constructor is callable from Luau");
  Check(ConstructedCount == 1, "the selected constructor runs exactly once");
  Check(LiveCount == 1, "exactly one object exists after construction");
  Check(Hooks::PublishedUserdataCount(Owner) == 1,
        "a constructed object is published as exactly one value");
  Check(Hooks::CachedIdentityCount(Owner) == 1,
        "a constructed value is recorded in the native identity cache");
  Check(Hooks::ClassMetatableIsCreated(Owner, "Vector3"),
        "the first constructed value creates the class metatable");

  const Luna::Detail::ConstructionCounters Built =
      Hooks::UserdataConstructionCounters(Owner);
  Check(
      Built.Allocate == 1 && Built.Construct == 1,
      "one construction performs exactly one allocation and one construction");
  Check(Built.AllocationFailure == 0 && Built.ConstructionFailure == 0,
        "a successful construction refuses nothing");

  // Collection releases the value exactly once, and the object it owned is
  // destroyed and its storage deallocated.
  Check(Hooks::CollectGarbage(Owner), "the collector runs");
  const Luna::Detail::ReleaseCounters Released =
      Hooks::UserdataReleaseCounters(Owner);
  Check(Released.Destroy == 1 && Released.Deallocate == 1,
        "a collected Lua-owned value is destroyed once and deallocated once");
  Check(Released.IncompleteMetadata == 0,
        "cleanup never runs without the metadata it needs");
  Check(LiveCount == 0, "the constructed object is destroyed exactly once");
}

void CheckConstructorOverloadsResolveByShape() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterVector3(Registry).IsSuccess(), "the class publishes");

  Check(Owner.Execute("local A = Vector3.New()").IsSuccess(),
        "the default constructor is selected by arity");
  Check(Owner.Execute("local B = Vector3.New(4, 5, 6)").IsSuccess(),
        "the three-argument constructor is selected by arity");
  Check(ConstructedCount == 2, "each call runs exactly one constructor");

  const auto Refused = Owner.Execute("local C = Vector3.New(1, 2)");
  Check(!Refused.IsSuccess(),
        "an argument count no candidate accepts is refused");
  Check(ConstructedCount == 2, "a refused call constructs nothing");
  Check(Hooks::PublishedUserdataCount(Owner) == 2,
        "a refused call publishes no value");
}

void CheckFactoriesAndSingletonsPublishTheirOwnership() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterVector3(Registry).IsSuccess(), "the class publishes");

  Check(Owner.Execute("local V = Vector3.Scaled(2)").IsSuccess(),
        "a by-value factory is callable from Luau");
  Check(Hooks::PublishedUserdataCount(Owner) == 1,
        "a by-value factory publishes exactly one owned value");

  Check(Owner.Execute("local B = Vector3.Boxed()").IsSuccess(),
        "a shared factory is callable from Luau");
  Check(Hooks::PublishedUserdataCount(Owner) == 2,
        "a shared factory publishes exactly one shared value");

  Check(Owner.Execute("local O = Vector3.Origin()").IsSuccess(),
        "a singleton accessor is callable from Luau");
  Check(Hooks::PublishedUserdataCount(Owner) == 3,
        "a singleton accessor publishes exactly one borrowed value");

  // Re-accessing the singleton reuses the one value that already describes the
  // engine-owned object, so there is never a second owner of it.
  Check(Owner.Execute("local P = Vector3.Origin()").IsSuccess(),
        "the singleton accessor may be called again");
  Check(Hooks::PublishedUserdataCount(Owner) == 3,
        "re-accessing one borrowed object creates no second owner");

  Check(Hooks::CollectGarbage(Owner), "the collector runs");
  const Luna::Detail::ReleaseCounters Released =
      Hooks::UserdataReleaseCounters(Owner);
  Check(Released.SharedRelease == 1,
        "exactly one shared ownership reference is released");
  Check(Released.IncompleteMetadata == 0,
        "cleanup never runs without the metadata it needs");
  Check(EngineOrigin()->X == 0.0,
        "a borrowed engine object survives collection of its value");
}

void CheckConflictingConstructionNamesAreRefused() {
  {
    // Two candidates of one name that no call could tell apart.
    ResetCounters();
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Vector3> Class =
        Registry.RegisterClass<Vector3>("Vector3", Vector3Key());
    Luna::ClassBuilder<Vector3> &First =
        Class.Constructor<double, double, double>();
    Luna::ClassBuilder<Vector3> &Second = First.Factory("New", &MakeComponents);
    const auto Result = Second.Commit();
    Check(!Result.IsSuccess(),
          "two indistinguishable construction signatures are refused");
    Check(Result.Diagnostic() && Result.Diagnostic()->Category() ==
                                     Luna::ErrorCategory::DuplicateGlobalName,
          "an indistinguishable candidate is a deterministic duplicate");
    Check(Hooks::RegisteredClassCount(Owner) == 0,
          "a refused construction candidate publishes no class at all");
    Check(Registry.Reflection().IsEmpty(),
          "a refused construction candidate contributes no reflection record");
  }

  {
    // An explicit policy that contradicts the accessor's declared result.
    ResetCounters();
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Vector3> Class =
        Registry.RegisterClass<Vector3>("Vector3", Vector3Key());
    Luna::OwnershipPolicy Shared = Luna::OwnershipPolicy::Shared();
    Luna::ClassBuilder<Vector3> &Declared =
        Class.Singleton("Origin", &EngineOrigin, std::move(Shared));
    const auto Result = Declared.Commit();
    Check(!Result.IsSuccess(),
          "an ownership policy that contradicts the declared result is "
          "refused");
    Check(Result.Diagnostic() && Result.Diagnostic()->Message().find(
                                     "ownership policy") != std::string::npos,
          "the refusal names the contradictory ownership policy");
    Check(Hooks::RegisteredClassCount(Owner) == 0,
          "a refused policy publishes no class at all");
  }

  {
    // A borrowed singleton whose explicit policy declares no lifetime at all.
    ResetCounters();
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Vector3> Class =
        Registry.RegisterClass<Vector3>("Vector3", Vector3Key());
    Luna::OwnershipPolicy Undeclared =
        Luna::OwnershipPolicy::Borrowed(Luna::LifetimeHandle::Undeclared());
    Luna::ClassBuilder<Vector3> &Declared =
        Class.Singleton("Origin", &EngineOrigin, std::move(Undeclared));
    const auto Result = Declared.Commit();
    Check(!Result.IsSuccess(),
          "a borrowed result without a declared lifetime is refused");
  }

  {
    // Documentation names an already declared member.
    ResetCounters();
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Vector3> Class =
        Registry.RegisterClass<Vector3>("Vector3", Vector3Key());
    Luna::ClassBuilder<Vector3> &Documented =
        Class.Documentation("Missing", "nothing declares this.");
    const auto Result = Documented.Commit();
    Check(!Result.IsSuccess(),
          "documenting an undeclared class member is refused");
  }
}

void CheckRefusedConstructionLeavesNothingBehind() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterVector3(Registry).IsSuccess(), "the class publishes");

  // A refused conversion of one argument stops before any object exists.
  const auto Refused = Owner.Execute("local V = Vector3.New('a', 'b', 'c')");
  Check(!Refused.IsSuccess(),
        "a call whose arguments cannot convert is refused");
  Check(ConstructedCount == 0, "a refused call never allocates or constructs");
  Check(Hooks::OwnedUserdataCount(Owner) == 0,
        "a refused call leaves no ownership record behind");

  const Luna::Detail::ConstructionCounters Built =
      Hooks::UserdataConstructionCounters(Owner);
  Check(Built.Allocate == 0 && Built.Construct == 0,
        "a refused call performs no allocation and no construction");
  Check(Owner.IsReady(), "the State remains usable after a refused call");
  Check(Owner.Execute("local V = Vector3.New(1, 2, 3)").IsSuccess(),
        "a later construction still succeeds");
}

void CheckConstructorExceptionsAreTranslated() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  Luna::ClassBuilder<Vector3> Class =
      Registry.RegisterClass<Vector3>("Vector3", Vector3Key());
  Luna::ClassBuilder<Vector3> &WithFactory =
      Class.Factory("Failing", [](double) -> Vector3 {
        throw std::runtime_error("no vector today");
      });
  Check(WithFactory.Commit().IsSuccess(), "the failing factory registers");

  const auto Result = Owner.Execute("local V = Vector3.Failing(1)");
  Check(!Result.IsSuccess(), "a throwing factory refuses the call");
  Check(LiveCount == 0, "a throwing factory leaves no object alive");
  Check(Hooks::OwnedUserdataCount(Owner) == 0,
        "a throwing factory leaves no ownership record behind");

  const Luna::Detail::ConstructionCounters Built =
      Hooks::UserdataConstructionCounters(Owner);
  Check(Built.Allocate == 0,
        "a factory that never produced an object allocates nothing");
  Check(Owner.IsReady(), "the State remains usable after a throwing factory");
}

// -- one class selecting its own storage protocol ----------------------------

// One consumer arena the class's values are allocated from. It is ordinary
// consumer state, held by the protocol's own operations, which is exactly how a
// real allocator's state travels with it.
class Arena final {
public:
  [[nodiscard]] void *Reserve(std::size_t ByteCount, std::size_t Alignment) {
    ++Reservations;
    return ::operator new(ByteCount, std::align_val_t{Alignment});
  }

  void Return(void *Storage, std::size_t Alignment) {
    ++Returns;
    ::operator delete(Storage, std::align_val_t{Alignment});
  }

  [[nodiscard]] std::size_t ReservationCount() const noexcept {
    return Reservations;
  }
  [[nodiscard]] std::size_t ReturnCount() const noexcept { return Returns; }

private:
  std::size_t Reservations = 0;
  std::size_t Returns = 0;
};

[[nodiscard]] Luna::ClassAllocator ArenaProtocol(std::shared_ptr<Arena> Held) {
  Luna::ClassAllocator::AllocateOperation Allocate =
      [Held](const Luna::StorageRequest &Wanted) -> void * {
    return Held->Reserve(Wanted.ByteCount, Wanted.Alignment);
  };
  Luna::ClassAllocator::ConstructOperation Construct = [](void *Storage) {
    static_cast<void>(new (Storage) Vector3());
    return Luna::AllocatorStepResult::Done();
  };
  Luna::ClassAllocator::DestroyOperation Destroy = [](void *Storage) {
    static_cast<Vector3 *>(Storage)->~Vector3();
    return Luna::AllocatorStepResult::Done();
  };
  Luna::ClassAllocator::DeallocateOperation Deallocate =
      [Held](void *Storage, const Luna::StorageRequest &Wanted) {
        Held->Return(Storage, Wanted.Alignment);
        return Luna::AllocatorStepResult::Done();
      };
  return Luna::ClassAllocator::FromOperations(
      "Studio.Vector3Arena", Luna::StorageRequest::ForClass<Vector3>(),
      std::move(Allocate), std::move(Construct), std::move(Destroy),
      std::move(Deallocate));
}

// The reflected allocator policy identity of one construction candidate.
[[nodiscard]] std::string ReflectedPolicy(const Luna::ReflectionSnapshot &Taken,
                                          Luna::SymbolKind Kind,
                                          std::string_view QualifiedName) {
  const Luna::ReflectionRecordRange Candidates = Taken.Symbols(Kind);
  for (std::size_t Index = 0; Index < Candidates.Size(); ++Index) {
    const Luna::ReflectionRecord Candidate = Candidates.At(Index);
    if (Candidate.QualifiedName() == QualifiedName)
      return std::string(Candidate.AllocatorPolicy());
  }
  return std::string();
}

// A class states its storage protocol once, and every value Luna creates of it
// comes from that protocol - whether the statement came before or after the
// candidates that use it.
void CheckSelectedAllocatorAppliesInEitherOrder() {
  for (int Order = 0; Order < 2; ++Order) {
    ResetCounters();
    const std::shared_ptr<Arena> Storage = std::make_shared<Arena>();
    {
      Luna::State Owner;
      Luna::BindingRegistry Registry = Owner.Bindings();
      Luna::ClassBuilder<Vector3> Class =
          Registry.RegisterClass<Vector3>("Vector3", Vector3Key());

      Luna::RegistrationResult Published = Luna::RegistrationResult::Success();
      if (Order == 0) {
        // Stated first, then the candidates that use it.
        Luna::ClassBuilder<Vector3> &WithStorage =
            Class.Allocator(ArenaProtocol(Storage));
        Luna::ClassBuilder<Vector3> &WithDefault = WithStorage.Constructor<>();
        Luna::ClassBuilder<Vector3> &WithComponents =
            WithDefault.Constructor<double, double, double>();
        Luna::ClassBuilder<Vector3> &WithFactory =
            WithComponents.Factory("Scaled", &MakeScaled);
        Luna::ClassBuilder<Vector3> &WithShared =
            WithFactory.Factory("Boxed", &MakeShared);
        Published = WithShared.Commit();
      } else {
        // The candidates first, and the statement afterwards.
        Luna::ClassBuilder<Vector3> &WithDefault = Class.Constructor<>();
        Luna::ClassBuilder<Vector3> &WithComponents =
            WithDefault.Constructor<double, double, double>();
        Luna::ClassBuilder<Vector3> &WithFactory =
            WithComponents.Factory("Scaled", &MakeScaled);
        Luna::ClassBuilder<Vector3> &WithShared =
            WithFactory.Factory("Boxed", &MakeShared);
        Luna::ClassBuilder<Vector3> &WithStorage =
            WithShared.Allocator(ArenaProtocol(Storage));
        Published = WithStorage.Commit();
      }
      Check(Published.IsSuccess(),
            "a class that selects its own storage protocol publishes");

      // Every candidate Luna creates a value for reflects the selected policy;
      // a candidate whose object Luna never allocated still reflects that it
      // adopted one.
      const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
      const Luna::ReflectionRecordRange Constructors =
          Snapshot.Symbols(Luna::SymbolKind::Constructor);
      Check(Constructors.Size() == 2,
            "both constructors are reflected as their own candidates");
      bool EveryConstructorNamesTheArena = Constructors.Size() == 2;
      for (std::size_t Index = 0; Index < Constructors.Size(); ++Index) {
        if (Constructors.At(Index).AllocatorPolicy() != "Studio.Vector3Arena")
          EveryConstructorNamesTheArena = false;
      }
      Check(EveryConstructorNamesTheArena,
            "every constructor reflects the storage protocol its class "
            "selected");
      Check(ReflectedPolicy(Snapshot, Luna::SymbolKind::Factory,
                            "Vector3.Scaled") == "Studio.Vector3Arena",
            "a by-value factory reflects the selected storage protocol too");
      Check(ReflectedPolicy(Snapshot, Luna::SymbolKind::Factory,
                            "Vector3.Boxed") == "Luna.AdoptedStorage",
            "a shared factory still reflects the object it adopted");

      // The real Luau calls allocate from the consumer's arena and nowhere
      // else.
      Check(Owner.Execute("A = Vector3.New()").IsSuccess() &&
                Owner.Execute("B = Vector3.New(1, 2, 3)").IsSuccess() &&
                Owner.Execute("C = Vector3.Scaled(2)").IsSuccess(),
            "every creating candidate is callable from Luau");
      Check(Storage->ReservationCount() == 3 && Storage->ReturnCount() == 0,
            "each created value took exactly one reservation from the arena");
      Check(ConstructedCount >= 3 && LiveCount == 3,
            "exactly the three created objects are alive");

      Check(Owner.Execute("D = Vector3.Boxed()").IsSuccess(),
            "the shared factory is callable from Luau");
      Check(Storage->ReservationCount() == 3,
            "an adopted object takes no reservation from the arena");

      // Collection gives every reservation back to the arena it came from.
      Check(Owner.Execute("A = nil\nB = nil\nC = nil\nD = nil").IsSuccess(),
            "the script drops its references");
      Check(Hooks::CollectGarbage(Owner), "the collector runs to completion");
      Check(Storage->ReturnCount() == 3 && LiveCount == 0,
            "every created value is destroyed and given back exactly once");
      Check(Hooks::UserdataReleaseCounters(Owner).IncompleteMetadata == 0,
            "every cleanup step ran with the metadata it needs");
    }
    Check(Storage->ReservationCount() == Storage->ReturnCount(),
          "the arena is balanced after the State that used it is gone");
  }
}

// A protocol Luna could not create or release a value through is refused
// transactionally, wherever in the declaration it was stated.
void CheckIncompatibleSelectedAllocatorsAreRefused() {
  const Luna::StorageRequest Requested =
      Luna::StorageRequest::ForClass<Vector3>();
  Luna::ClassAllocator::AllocateOperation Allocate =
      [](const Luna::StorageRequest &Wanted) -> void * {
    return ::operator new(Wanted.ByteCount, std::align_val_t{Wanted.Alignment});
  };
  Luna::ClassAllocator::DestroyOperation Destroy = [](void *Storage) {
    static_cast<Vector3 *>(Storage)->~Vector3();
    return Luna::AllocatorStepResult::Done();
  };
  Luna::ClassAllocator::DeallocateOperation Deallocate =
      [](void *Storage, const Luna::StorageRequest &Wanted) {
        ::operator delete(Storage, std::align_val_t{Wanted.Alignment});
        return Luna::AllocatorStepResult::Done();
      };

  struct Refusal final {
    Luna::ClassAllocator Storage;
    std::string_view Expected;
    std::string_view Description;
  };

  // One storage request too small for the class: the protocol is complete, but
  // the storage it hands out could never hold a value of this class.
  Luna::StorageRequest Undersized = Requested;
  Undersized.ByteCount = 1;

  const Refusal Refusals[] = {
      Refusal{Luna::ClassAllocator::Undeclared(), "names no storage protocol",
              "an undeclared protocol is refused"},
      Refusal{Luna::ClassAllocator::FromOperations(
                  std::string_view(), Requested, Allocate,
                  Luna::ClassAllocator::ConstructOperation(), Destroy,
                  Deallocate),
              "names no policy identity",
              "a protocol without a policy identity is refused"},
      Refusal{Luna::ClassAllocator::FromOperations(
                  "Studio.Unallocating", Requested,
                  Luna::ClassAllocator::AllocateOperation(),
                  Luna::ClassAllocator::ConstructOperation(), Destroy,
                  Deallocate),
              "declares no allocation step",
              "a protocol that cannot obtain storage is refused"},
      Refusal{Luna::ClassAllocator::FromOperations(
                  "Studio.Undestroying", Requested, Allocate,
                  Luna::ClassAllocator::ConstructOperation(),
                  Luna::ClassAllocator::DestroyOperation(), Deallocate),
              "declares no destruction step",
              "a protocol that could never destroy a value is refused"},
      Refusal{Luna::ClassAllocator::FromOperations(
                  "Studio.Unreturning", Requested, Allocate,
                  Luna::ClassAllocator::ConstructOperation(), Destroy,
                  Luna::ClassAllocator::DeallocateOperation()),
              "declares no deallocation step",
              "a protocol that could never give storage back is refused"},
      Refusal{Luna::ClassAllocator::FromOperations(
                  "Studio.Undersized", Undersized, Allocate,
                  Luna::ClassAllocator::ConstructOperation(), Destroy,
                  Deallocate),
              "storage size of the selected allocator",
              "a protocol whose storage cannot hold the class is refused"}};

  for (const Refusal &Case : Refusals) {
    for (int Order = 0; Order < 2; ++Order) {
      ResetCounters();
      Luna::State Owner;
      Luna::BindingRegistry Registry = Owner.Bindings();
      Luna::ClassBuilder<Vector3> Class =
          Registry.RegisterClass<Vector3>("Vector3", Vector3Key());

      Luna::RegistrationResult Result = Luna::RegistrationResult::Success();
      if (Order == 0) {
        Luna::ClassBuilder<Vector3> &WithStorage =
            Class.Allocator(Case.Storage);
        Luna::ClassBuilder<Vector3> &WithDefault = WithStorage.Constructor<>();
        Result = WithDefault.Commit();
      } else {
        Luna::ClassBuilder<Vector3> &WithDefault = Class.Constructor<>();
        Luna::ClassBuilder<Vector3> &WithStorage =
            WithDefault.Allocator(Case.Storage);
        Result = WithStorage.Commit();
      }

      Check(!Result.IsSuccess(), Case.Description);
      const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
      Check(Diagnostic && Diagnostic->Message().find(
                              std::string(Case.Expected)) != std::string::npos,
            "the refusal names exactly what the selected protocol lacks");
      Check(Hooks::RegisteredClassCount(Owner) == 0,
            "a refused storage protocol publishes no class at all");
      Check(Registry.Reflection().IsEmpty(),
            "a refused storage protocol contributes no reflection record");
      Check(Owner.IsReady() && Owner.Execute("return 1").IsSuccess(),
            "the State stays usable after a refused storage protocol");
    }
  }
}

// Luna retains the selected protocol, and whatever state its operations
// captured, until the last value created through it has completed cleanup -
// long after the declaration that named it is gone.
void CheckAllocatorMetadataOutlivesItsDeclaration() {
  ResetCounters();
  std::weak_ptr<Arena> Observed;
  {
    Luna::State Owner;
    {
      // Everything the consumer held is gone by the end of this scope: the
      // arena's own shared owner and the allocator value that named it.
      const std::shared_ptr<Arena> Storage = std::make_shared<Arena>();
      Observed = Storage;
      Luna::BindingRegistry Registry = Owner.Bindings();
      Luna::ClassBuilder<Vector3> Class =
          Registry.RegisterClass<Vector3>("Vector3", Vector3Key());
      Luna::ClassAllocator Protocol = ArenaProtocol(Storage);
      Luna::ClassBuilder<Vector3> &WithStorage =
          Class.Allocator(std::move(Protocol));
      Luna::ClassBuilder<Vector3> &WithDefault =
          WithStorage.Constructor<double, double, double>();
      Check(WithDefault.Commit().IsSuccess(), "the class publishes");
    }
    Check(!Observed.expired(),
          "the registration retains the state the protocol captured");

    Check(Owner.Execute("Held = Vector3.New(1, 2, 3)").IsSuccess(),
          "a value is created through the retained protocol");
    const std::shared_ptr<Arena> Reached = Observed.lock();
    Check(Reached && Reached->ReservationCount() == 1,
          "the retained protocol allocated from the state it captured");

    Check(Owner.Execute("Held = nil").IsSuccess(),
          "the script drops its only reference");
    Check(Hooks::CollectGarbage(Owner), "the collector runs to completion");
    Check(Reached && Reached->ReturnCount() == 1,
          "the retained protocol gave the storage back to the same state");
    Check(LiveCount == 0, "the created object was destroyed exactly once");
    Check(Hooks::UserdataReleaseCounters(Owner).IncompleteMetadata == 0,
          "no cleanup step ran without the protocol it required");
  }
  Check(Observed.expired(),
        "the protocol and its state are released once no value needs them");
}

// -- ambiguity among construction candidates --------------------------------

// One class whose two constructors no ranking can order for the same call.
struct Span final {
  double First = 0.0;
  double Second = 0.0;

  Span(int FirstValue, double SecondValue)
      : First(static_cast<double>(FirstValue)), Second(SecondValue) {
    ++ConstructedCount;
  }

  Span(double FirstValue, int SecondValue)
      : First(FirstValue), Second(static_cast<double>(SecondValue)) {
    ++ConstructedCount;
  }
};

// Two distinguishable constructor signatures both register, and a call neither
// of them dominates is refused as ambiguous instead of guessed at.
void CheckAmbiguousConstructionCandidatesAreRefused() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::ClassBuilder<Span> Class =
      Registry.RegisterClass<Span>("Span", Luna::StableTypeKey("Studio.Span"));
  Luna::ClassBuilder<Span> &WithFirst = Class.Constructor<int, double>();
  Luna::ClassBuilder<Span> &WithSecond = WithFirst.Constructor<double, int>();
  Check(WithSecond.Commit().IsSuccess(),
        "two distinguishable constructor signatures both register");
  Check(Hooks::OverloadCandidateCount(Owner, "Span.New") == 2,
        "both candidates join one canonical overload set");

  const auto Ambiguous = Owner.Execute("local S = Span.New(1, 2)");
  Check(!Ambiguous.IsSuccess(),
        "a construction no candidate dominates fails instead of guessing");
  const Luna::ErrorDiagnostic *Diagnostic = Ambiguous.Diagnostic();
  const std::string Message =
      Diagnostic ? Diagnostic->Message() : std::string();
  Check(Message.find("ambiguous") != std::string::npos,
        "the refusal reports the ambiguity");
  Check(Message.find("(signed 32-bit integer, number)") != std::string::npos &&
            Message.find("(number, signed 32-bit integer)") !=
                std::string::npos,
        "the refusal lists the non-dominated constructor signatures");
  Check(ConstructedCount == 0,
        "an ambiguous construction constructs no object at all");
  Check(Hooks::UserdataConstructionCounters(Owner).Allocate == 0,
        "an ambiguous construction allocates nothing");
  Check(Hooks::PublishedUserdataCount(Owner) == 0 &&
            Hooks::CachedIdentityCount(Owner) == 0,
        "an ambiguous construction publishes nothing and caches nothing");
  Check(Owner.Execute("local S = Span.New(1, 2)").IsSuccess() == false &&
            Owner.Execute("local T = Span.New(1, 2.5)").IsSuccess(),
        "an unambiguous construction still resolves afterwards");
  Check(ConstructedCount == 1 && Hooks::PublishedUserdataCount(Owner) == 1,
        "exactly the unambiguous call constructed and published a value");
}

// -- the singleton default policy through real calls ------------------------

// A singleton accessor defaults to borrowed ownership, and the whole
// consequence of that default is observable end to end: Luna allocates nothing,
// publishes one value per object, never destroys the engine's object, and hands
// the same value back on every later access.
void CheckSingletonDefaultPolicyThroughRealCalls() {
  ResetCounters();
  const double Before = EngineOrigin()->X;
  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Check(RegisterVector3(Registry).IsSuccess(), "the class publishes");

    const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
    Check(ReflectedPolicy(Snapshot, Luna::SymbolKind::Factory,
                          "Vector3.Origin") == "Luna.AdoptedStorage",
          "a singleton accessor reflects an adopted object by default");

    Check(Owner.Execute("Origin = Vector3.Origin()").IsSuccess(),
          "the default singleton accessor is callable from Luau");
    Check(Hooks::PublishedUserdataCount(Owner) == 1,
          "the accessor publishes exactly one borrowed value");
    Check(Hooks::UserdataConstructionCounters(Owner).Allocate == 0 &&
              ConstructedCount == 0,
          "a borrowed object is neither allocated nor constructed by Luna");

    // One object is one value: every later access is the value the script
    // already holds, and `==` in script agrees.
    Check(Owner.Execute("Again = Vector3.Origin()").IsSuccess(),
          "the accessor may be called again");
    Check(Hooks::PublishedUserdataCount(Owner) == 1 &&
              Hooks::LiveCachedIdentityCount(Owner) == 1,
          "re-accessing one borrowed object creates no second owner");
    Check(Owner
              .Execute("Result = 0\nif Again == Origin and typeof(Origin) == "
                       "'Vector3' then Result = 1 end")
              .IsSuccess(),
          "the script compares the two accesses");
    const auto Observed = Hooks::ObserveIntegerGlobal(Owner, "Result");
    Check(Observed && *Observed == 1,
          "both accesses are exactly one script value of the class");

    // A Lua-owned value in the same State is destroyed by collection; the
    // borrowed object is not touched by it at all.
    Check(Owner.Execute("Owned = Vector3.New(4, 5, 6)").IsSuccess(),
          "a constructed value is published alongside the borrowed one");
    Check(Owner.Execute("Owned = nil\nOrigin = nil\nAgain = nil").IsSuccess(),
          "the script drops both references");
    Check(Hooks::CollectGarbage(Owner), "the collector runs to completion");

    const Luna::Detail::ReleaseCounters Released =
        Hooks::UserdataReleaseCounters(Owner);
    Check(Released.Destroy == 1 && Released.Deallocate == 1,
          "collection destroys the Lua-owned object and nothing else");
    Check(Released.SharedRelease == 0,
          "a borrowed value releases no shared ownership reference");
    Check(Released.IncompleteMetadata == 0,
          "every cleanup step ran with the metadata it needs");
    Check(EngineOrigin()->X == Before,
          "the engine's own object survives collection of its value");
  }
  Check(EngineOrigin()->X == Before,
        "the engine's own object survives destruction of the State");
}

} // namespace

int RunClassConstructionTests() {
  FailureCount = 0;
  CheckConstructionCandidatesAreReflected();
  CheckConstructorsPublishOwnedValues();
  CheckConstructorOverloadsResolveByShape();
  CheckFactoriesAndSingletonsPublishTheirOwnership();
  CheckConflictingConstructionNamesAreRefused();
  CheckRefusedConstructionLeavesNothingBehind();
  CheckConstructorExceptionsAreTranslated();
  CheckSelectedAllocatorAppliesInEitherOrder();
  CheckIncompatibleSelectedAllocatorsAreRefused();
  CheckAllocatorMetadataOutlivesItsDeclaration();
  CheckAmbiguousConstructionCandidatesAreRefused();
  CheckSingletonDefaultPolicyThroughRealCalls();
  return FailureCount == 0 ? 0 : 1;
}
