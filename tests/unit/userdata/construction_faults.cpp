// Focused coverage of failure at every stage of constructing one class value,
// driven through real Luau calls wherever a stage can be reached that way.
//
// Constructing a value has six stages, and each one is a milestone whose
// completion decides the cleanup a failure warrants: obtain storage, construct
// the object in it, establish its ownership, record it in the native identity
// cache, associate the class metatable, and publish exactly one value. This
// file fails each stage and asserts the same four things every time - nothing
// published, exactly the cleanup the completed milestones warrant, the stack
// restored to its entry depth, and one deterministic diagnostic - plus that the
// State keeps constructing afterwards.
//
// Which stages have an injection site, and which are reached through a real
// refusal instead:
//
//   * Allocation: no injection site is needed. A class selects a storage
//     protocol whose allocation step produces nothing, so a real Luau
//     constructor call reaches the refusal itself.
//   * Construction: no injection site is needed either. A constructor that
//     throws, and one whose class refuses to be built, both mean no object
//     exists.
//   * Ownership: no injection site exists, and no consumer declaration can
//     reach one, because a construction candidate always states an ownership
//     payload its own model accepts. It is covered through the same write path
//     the candidate uses, with a protocol whose statement Luna cannot honor.
//   * Cache insertion: no injection site exists. It is reached through a real
//     refusal: one native object exposed twice under two different ownership
//     models is the conflict the cache is there to catch.
//   * Metatable association: no injection site exists, and the only failure is
//   a
//     virtual machine that cannot create the table at all. What is asserted
//     instead is the invariant around it - exactly one metatable per class,
//     created once and reused by every later construction.
//   * Publication: the return writer's two fault points reach it, both before
//   it
//     begins and after it completed, so an injected failure after a complete
//     publication has to release the value it already published.
//   * Final release: reached through collection, an explicit lifecycle action,
//     and State destruction, including a destruction step that throws.

// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_construction.hpp>
#include <luna/binding/lifetime_handle.hpp>
#include <luna/core/results/execution_result.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

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
using FaultPoint = Luna::Detail::StateFaultPoint;
using Luna::AllocatorStepResult;
using Luna::ClassAllocator;
using Luna::StorageRequest;
using Luna::Detail::ConstAccess;
using Luna::Detail::OwnershipModel;
using Luna::Detail::ReleaseCause;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "class construction fault check failed: " << Description << '\n';
}

// -- the model under test ---------------------------------------------------

// What the generated storage protocol and the class's own constructor are asked
// to do. Every flag models one stage failing, and nothing else about the model
// changes with it.
struct StagePolicy final {
  bool AllocationProducesNothing = false;
  bool ConstructionThrows = false;
  bool DestructionThrows = false;
};

StagePolicy Policy;

std::size_t AllocateCalls = 0;
std::size_t DeallocateCalls = 0;
std::size_t DestroyCalls = 0;
std::size_t ConstructedCount = 0;
std::size_t LiveCount = 0;

void ResetModel() {
  Policy = StagePolicy();
  AllocateCalls = 0;
  DeallocateCalls = 0;
  DestroyCalls = 0;
  ConstructedCount = 0;
  LiveCount = 0;
}

struct Widget final {
  double Width = 0.0;

  Widget() : Widget(0.0) {}

  explicit Widget(double WidthValue) : Width(WidthValue) {
    if (Policy.ConstructionThrows)
      throw std::runtime_error("the widget refused to be built");
    ++ConstructedCount;
    ++LiveCount;
  }

  Widget(const Widget &Other) : Widget(Other.Width) {}
  Widget(Widget &&Other) noexcept : Width(Other.Width) {
    ++ConstructedCount;
    ++LiveCount;
  }

  Widget &operator=(const Widget &) = default;
  Widget &operator=(Widget &&) noexcept = default;

  ~Widget() { --LiveCount; }
};

// One engine-owned widget a singleton accessor borrows and a shared factory
// hands out through a non-owning reference, so both name exactly one object.
[[nodiscard]] Widget *EngineWidget() {
  static Widget Engine(11.0);
  return &Engine;
}

[[nodiscard]] std::shared_ptr<Widget> SharedEngineWidget() {
  return std::shared_ptr<Widget>(EngineWidget(), [](Widget *) {});
}

[[nodiscard]] std::shared_ptr<Widget> MissingWidget() {
  return std::shared_ptr<Widget>();
}

[[nodiscard]] Luna::StableTypeKey WidgetKey() {
  return Luna::StableTypeKey("Studio.FaultWidget");
}

// The consumer's own storage protocol for this class, counted so the exact
// cleanup of every stage is observable.
[[nodiscard]] ClassAllocator WidgetStorageProtocol() {
  ClassAllocator::AllocateOperation Allocate =
      [](const StorageRequest &Wanted) -> void * {
    ++AllocateCalls;
    if (Policy.AllocationProducesNothing)
      return nullptr;
    return ::operator new(Wanted.ByteCount, std::align_val_t{Wanted.Alignment});
  };
  ClassAllocator::ConstructOperation Construct = [](void *Storage) {
    static_cast<void>(new (Storage) Widget());
    return AllocatorStepResult::Done();
  };
  ClassAllocator::DestroyOperation Destroy = [](void *Storage) {
    ++DestroyCalls;
    if (Policy.DestructionThrows)
      throw std::runtime_error("the widget refused to be destroyed");
    static_cast<Widget *>(Storage)->~Widget();
    return AllocatorStepResult::Done();
  };
  ClassAllocator::DeallocateOperation Deallocate =
      [](void *Storage, const StorageRequest &Wanted) {
        ++DeallocateCalls;
        ::operator delete(Storage, std::align_val_t{Wanted.Alignment});
        return AllocatorStepResult::Done();
      };
  return ClassAllocator::FromOperations(
      "Studio.WidgetArena", StorageRequest::ForClass<Widget>(),
      std::move(Allocate), std::move(Construct), std::move(Destroy),
      std::move(Deallocate));
}

// One class carrying every construction form this file drives, all of them
// creating their values through the consumer's own protocol.
[[nodiscard]] Luna::RegistrationResult RegisterWidget(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::ClassBuilder<Widget> Class =
      Registry.RegisterClass<Widget>("Widget", WidgetKey());
  Luna::ClassBuilder<Widget> &WithStorage =
      Class.Allocator(WidgetStorageProtocol());
  Luna::ClassBuilder<Widget> &WithDefault = WithStorage.Constructor<>();
  Luna::ClassBuilder<Widget> &WithWidth = WithDefault.Constructor<double>();
  Luna::ClassBuilder<Widget> &WithBorrowed =
      WithWidth.Singleton("Engine", &EngineWidget);
  Luna::ClassBuilder<Widget> &WithShared =
      WithBorrowed.Factory("Boxed", &SharedEngineWidget);
  Luna::ClassBuilder<Widget> &WithMissing =
      WithShared.Factory("Absent", &MissingWidget);
  return WithMissing.Commit();
}

[[nodiscard]] std::string Failed(Luna::State &Host, std::string_view Source) {
  const Luna::ExecutionResult Result = Host.Execute(Source);
  if (Result.IsSuccess())
    return std::string();
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  return Diagnostic ? Diagnostic->Message() : std::string("<no diagnostic>");
}

[[nodiscard]] bool Contains(std::string_view Text, std::string_view Needle) {
  return Text.find(Needle) != std::string_view::npos;
}

// Nothing became visible, nothing is owned, and the callback checkpoint the
// refused call entered with is exactly the one it left.
void CheckNothingWasPublished(Luna::State &Host, std::string_view Description) {
  Check(Hooks::PublishedUserdataCount(Host) == 0, Description);
  Check(Hooks::OwnedUserdataCount(Host) == 0,
        "a refused construction leaves no owner behind");
  Check(Hooks::CachedIdentityCount(Host) == 0,
        "a refused construction records no identity-cache entry");
  const auto Restored = Hooks::ObserveLastCallbackStackRestoration(Host);
  Check(Restored.has_value() &&
            Restored->EntryDepth == Restored->RestoredDepth &&
            Restored->ErrorDepth == Restored->RestoredDepth + 1,
        "a refused construction restores the exact callback checkpoint");
}

// -- allocation -------------------------------------------------------------

// Storage that never existed is cleaned up by nothing at all, and the refusal
// is reached by the ordinary call rather than by an injected fault.
void CheckAllocationFailureThroughRealCalls() {
  ResetModel();
  Luna::State Owner;
  Check(RegisterWidget(Owner).IsSuccess(), "the widget class publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  Policy.AllocationProducesNothing = true;

  const std::string Refused = Failed(Owner, "local W = Widget.New(3)");
  Check(!Refused.empty(), "a construction whose storage never existed refuses");
  Check(Contains(Refused, "Widget"),
        "the refusal names the class whose value could not be constructed");
  CheckNothingWasPublished(Owner,
                           "an allocation that produced nothing publishes "
                           "nothing");
  Check(AllocateCalls == 1 && ConstructedCount == 0,
        "no object is constructed in storage that does not exist");
  Check(DestroyCalls == 0 && DeallocateCalls == 0,
        "an allocation that produced nothing is cleaned up by nothing");

  const Luna::Detail::ConstructionCounters Built =
      Hooks::UserdataConstructionCounters(Owner);
  Check(Built.AllocationFailure == 1 && Built.Allocate == 0 &&
            Built.Construct == 0 && Built.ContainedException == 0,
        "the State counts exactly one refused allocation");
  const Luna::Detail::ReleaseCounters Released =
      Hooks::UserdataReleaseCounters(Owner);
  Check(Released.MetadataRelease == 0 && Released.Invalidate == 0,
        "no release step runs for a value that was never staged");

  // The same failure reports one identical message however often it happens.
  Check(Failed(Owner, "local W = Widget.New(3)") == Refused,
        "one construction failure family reports one identical message");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every refused construction restores the exact root stack depth");

  // The State constructs as soon as its storage exists again.
  Policy.AllocationProducesNothing = false;
  Check(Owner.Execute("Kept = Widget.New(3)").IsSuccess(),
        "the State constructs a value after a refused allocation");
  Check(ConstructedCount == 1 && Hooks::PublishedUserdataCount(Owner) == 1,
        "the recovered construction publishes exactly one value");
}

// -- construction -----------------------------------------------------------

// An object that was never constructed is never destroyed, and the storage it
// would have occupied is given straight back.
void CheckConstructionFailureThroughRealCalls() {
  ResetModel();
  Luna::State Owner;
  Check(RegisterWidget(Owner).IsSuccess(), "the widget class publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  Policy.ConstructionThrows = true;

  const std::string Refused = Failed(Owner, "local W = Widget.New(4)");
  Check(!Refused.empty(), "a constructor that throws refuses the call");
  CheckNothingWasPublished(Owner,
                           "a construction that produced no object publishes "
                           "nothing");
  Check(AllocateCalls == 1 && ConstructedCount == 0 && LiveCount == 0,
        "the construction step ran once and left no object behind");
  Check(DestroyCalls == 0,
        "storage nothing was constructed in is never destroyed");
  Check(DeallocateCalls == 1,
        "construction failure gives the storage back exactly once");

  const Luna::Detail::ConstructionCounters Built =
      Hooks::UserdataConstructionCounters(Owner);
  Check(Built.Allocate == 1 && Built.Construct == 0 &&
            Built.ConstructionFailure == 1 && Built.ContainedException == 1,
        "the State counts one refused construction and contains its exception");

  const Luna::Detail::ReleaseCounters Released =
      Hooks::UserdataReleaseCounters(Owner);
  Check(Released.Destroy == 0 && Released.Deallocate == 1 &&
            Released.MetadataRelease == 1,
        "construction failure releases the storage and nothing else");
  Check(Released.IncompleteMetadata == 0,
        "the cleanup of a failed construction kept the metadata it needs");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "a refused construction restores the exact root stack depth");

  // A produced object that is no object at all is refused before any milestone
  // is reached, and names exactly that.
  const std::string Absent = Failed(Owner, "local W = Widget.Absent()");
  Check(Contains(Absent, "produced no object"),
        "a candidate that produced no object names exactly that");
  Check(AllocateCalls == 1,
        "a candidate that produced no object allocates nothing");

  Policy.ConstructionThrows = false;
  Check(Owner.Execute("Kept = Widget.New(4)").IsSuccess() &&
            ConstructedCount == 1,
        "the State constructs a value after a refused construction");
  Check(Owner.IsReady(), "the State stays ready through every refusal");
}

// -- ownership --------------------------------------------------------------

// Ownership establishment has no injection site, and no consumer declaration
// can reach a refusal there: a candidate always states the payload its own
// model accepts. What can be refused is a statement Luna itself could not
// honor, so the stage is driven through exactly the write path a candidate
// uses.
void CheckOwnershipFailureAfterConstruction() {
  ResetModel();
  Luna::State Owner;
  Check(RegisterWidget(Owner).IsSuccess(), "the widget class publishes");

  // Owned storage Luna could never destroy: allocation and deallocation are
  // declared, so staging and construction both succeed and only ownership
  // establishment can refuse it.
  ClassAllocator::AllocateOperation Allocate =
      [](const StorageRequest &Wanted) -> void * {
    ++AllocateCalls;
    return ::operator new(Wanted.ByteCount, std::align_val_t{Wanted.Alignment});
  };
  ClassAllocator::DeallocateOperation Deallocate =
      [](void *Storage, const StorageRequest &Wanted) {
        ++DeallocateCalls;
        ::operator delete(Storage, std::align_val_t{Wanted.Alignment});
        return AllocatorStepResult::Done();
      };
  const ClassAllocator Undestroyable = ClassAllocator::FromOperations(
      "Studio.UndestroyableWidgetArena", StorageRequest::ForClass<Widget>(),
      std::move(Allocate), ClassAllocator::ConstructOperation(),
      ClassAllocator::DestroyOperation(), std::move(Deallocate));

  Luna::Detail::ClassValueConstructionRequest Request;
  Request.QualifiedName = "Widget";
  Request.Path = "Unowned";
  Request.Ownership = OwnershipModel::LuaOwned;
  Request.Access = ConstAccess::Mutable;
  Request.Allocator = Undestroyable;
  Request.Construct = [](void *Storage) {
    static_cast<void>(new (Storage) Widget(5.0));
    return true;
  };

  const auto Refused = Hooks::ConstructClassValue(Owner, Request);
  Check(!Refused.Published && Refused.PublishedCount == 0,
        "a constructed value whose ownership Luna cannot honor publishes "
        "nothing");
  Check(Refused.FinalStackDepth == Refused.EntryStackDepth,
        "a refused ownership leaves the stack exactly as it found it");
  Check(AllocateCalls == 1 && ConstructedCount == 1,
        "the object was allocated and constructed before ownership refused it");
  Check(DeallocateCalls == 1,
        "the storage of a refused ownership is given back exactly once");
  Check(Hooks::OwnedUserdataCount(Owner) == 0 &&
            Hooks::PublishedUserdataCount(Owner) == 0,
        "a refused ownership leaves no owner and publishes nothing");

  const Luna::Detail::ReleaseCounters Released =
      Hooks::UserdataReleaseCounters(Owner);
  Check(Released.Invalidate == 1 && Released.CacheRemoval == 1,
        "ownership failure invalidates access and evicts the cache entry once");
  Check(Released.Deallocate == 1 && Released.MetadataRelease == 1,
        "every applicable release step of a refused ownership runs once");

  // This is the one situation the incomplete-metadata counter exists to name: a
  // constructed object Luna was asked to own without the destruction step its
  // release would need. It is unreachable from any consumer declaration,
  // because a class that selects such a protocol is refused transactionally
  // instead.
  Check(Released.IncompleteMetadata == 1,
        "cleanup names the destruction step this statement never declared");
  Check(DestroyCalls == 0,
        "a destruction step the protocol never declared is never performed");

  // The State constructs a complete value straight afterwards, through the
  // ordinary Luau call.
  Check(Owner.Execute("Kept = Widget.New(6)").IsSuccess(),
        "the State constructs a value after a refused ownership");
  Check(Hooks::PublishedUserdataCount(Owner) == 1,
        "exactly the complete value is published");
}

// -- identity cache ---------------------------------------------------------

// The identity cache decides before anything is owned, so one native object
// asked for under two different ownership models is refused there - and that
// refusal is reachable from script, because a borrowed accessor and a shared
// factory can name exactly the same object.
void CheckCacheInsertionRefusalThroughRealCalls() {
  for (int Order = 0; Order < 2; ++Order) {
    ResetModel();
    Luna::State Owner;
    Check(RegisterWidget(Owner).IsSuccess(), "the widget class publishes");
    const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

    const std::string_view First =
        Order == 0 ? "Held = Widget.Engine()" : "Held = Widget.Boxed()";
    const std::string_view Second =
        Order == 0 ? "Other = Widget.Boxed()" : "Other = Widget.Engine()";

    Check(Owner.Execute(First).IsSuccess(),
          "the first accessor publishes exactly one value of the object");
    Check(Hooks::PublishedUserdataCount(Owner) == 1 &&
              Hooks::LiveCachedIdentityCount(Owner) == 1,
          "one object has exactly one value and one cache entry");

    const std::string Refused = Failed(Owner, std::string(Second));
    Check(!Refused.empty(),
          "asking for one object under another ownership model is refused");
    Check(Hooks::PublishedUserdataCount(Owner) == 1 &&
              Hooks::LiveCachedIdentityCount(Owner) == 1,
          "a refused cache insertion creates no second value and no second "
          "owner");
    Check(Hooks::UserdataConstructionCounters(Owner).Allocate == 0,
          "neither accessor allocates storage Luna would own");
    Check(DestroyCalls == 0 && DeallocateCalls == 0,
          "a refused cache insertion destroys and deallocates nothing");
    Check(Failed(Owner, std::string(Second)) == Refused,
          "the conflicting request reports one identical message");
    Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
          "a refused cache insertion restores the exact root stack depth");

    // The value that was already published is untouched, and the engine object
    // it names is too.
    Check(Owner.Execute("assert(Held ~= nil, 'held')").IsSuccess(),
          "the value published first is still the script's value");
    Check(EngineWidget()->Width == 11.0,
          "a refused cache insertion never touches the native object");
    Check(Owner.Execute("Again = Widget.New(7)").IsSuccess(),
          "the State constructs a value after a refused cache insertion");
  }
}

// -- metatable association --------------------------------------------------

// The only metatable failure is a virtual machine that cannot create the table,
// which has no injection site. The invariant around the stage is asserted
// instead: one class owns one metatable, created by the first constructed value
// and reused by every later one, and no refusal ever creates a second.
void CheckMetatableAssociationIsCreatedOnceAndReused() {
  ResetModel();
  Luna::State Owner;
  Check(RegisterWidget(Owner).IsSuccess(), "the widget class publishes");
  Check(!Hooks::ClassMetatableIsCreated(Owner, "Widget"),
        "registering a class creates no metatable at all");

  Check(Owner.Execute("A = Widget.New(1)").IsSuccess(),
        "the first construction publishes a value");
  Check(Hooks::ClassMetatableIsCreated(Owner, "Widget") &&
            Hooks::ClassMetatableCreationCount(Owner, "Widget") == 1,
        "the first constructed value creates exactly one class metatable");

  Check(Owner.Execute("B = Widget.New(2)\nC = Widget.Engine()").IsSuccess(),
        "later constructions and accessors publish their values");
  Check(Hooks::ClassMetatableCreationCount(Owner, "Widget") == 1,
        "every later value of the class reuses the one metatable");

  Policy.AllocationProducesNothing = true;
  Check(!Failed(Owner, "D = Widget.New(3)").empty(),
        "a refused construction still refuses");
  Check(Hooks::ClassMetatableCreationCount(Owner, "Widget") == 1,
        "a refused construction creates no second metatable");
  Policy.AllocationProducesNothing = false;

  Check(Owner
            .Execute("Result = 0\nif typeof(A) == 'Widget' and typeof(B) == "
                     "'Widget' then Result = 1 end")
            .IsSuccess(),
        "every published value carries the metatable of its class");
  const auto Observed = Hooks::ObserveIntegerGlobal(Owner, "Result");
  Check(Observed && *Observed == 1,
        "the script sees one class type for every value of the class");
}

// -- publication ------------------------------------------------------------

// The two return-writer fault points bracket the publication stage: one refuses
// before it begins, the other after it completed. The second one is the only
// stage failure that has to undo a complete publication.
void CheckPublicationFailuresPublishNothing() {
  // Refused before publication began: the candidate ran, but nothing was ever
  // allocated, constructed, or owned.
  ResetModel();
  {
    Luna::State Owner;
    Check(RegisterWidget(Owner).IsSuccess(), "the widget class publishes");
    const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

    Hooks::InjectFault(Owner, FaultPoint::ReturnStackCapacity);
    const std::string Refused = Failed(Owner, "local W = Widget.New(8)");
    Check(Contains(Refused, "reserve stack capacity"),
          "a publication that cannot reserve its resources names that");
    CheckNothingWasPublished(Owner,
                             "a publication refused before it began publishes "
                             "nothing");
    Check(AllocateCalls == 0 && ConstructedCount == 0,
          "nothing is allocated or constructed for a value never published");
    Check(DestroyCalls == 0 && DeallocateCalls == 0,
          "a publication refused before it began cleans up nothing");
    Check(Hooks::PendingFaults(Owner, FaultPoint::ReturnStackCapacity) == 0,
          "the injected publication fault was consumed exactly once");
    Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
          "a refused publication restores the exact root stack depth");
  }

  // Refused after publication completed: allocation, construction, ownership,
  // the cache entry, the metatable, and the value all succeeded, so the refusal
  // has to release that value exactly once and leave nothing visible.
  ResetModel();
  {
    Luna::State Owner;
    Check(RegisterWidget(Owner).IsSuccess(), "the widget class publishes");
    const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

    Hooks::InjectFault(Owner, FaultPoint::ReturnWrite);
    const std::string Refused = Failed(Owner, "local W = Widget.New(9)");
    Check(Contains(Refused, "Injected internal return-writer failure"),
          "an injected publication failure names itself");
    Check(AllocateCalls == 1 && ConstructedCount == 1,
          "the value was completely constructed before publication refused");
    Check(DestroyCalls == 1 && DeallocateCalls == 1 && LiveCount == 0,
          "a refused publication destroys the object once and gives the "
          "storage back once");
    CheckNothingWasPublished(Owner,
                             "a publication refused after it completed leaves "
                             "no visible value");

    const Luna::Detail::ReleaseCounters Released =
        Hooks::UserdataReleaseCounters(Owner);
    Check(Released.Invalidate == 1 && Released.CacheRemoval == 1,
          "a refused publication invalidates access and evicts its cache entry "
          "once");
    Check(Released.Destroy == 1 && Released.Deallocate == 1 &&
              Released.MetadataRelease == 1,
          "every applicable release step of a refused publication runs once");
    Check(Released.IncompleteMetadata == 0,
          "the cleanup of a refused publication kept its metadata");
    Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
          "a refused publication restores the exact root stack depth");

    // And the very next construction publishes normally.
    Check(Owner.Execute("Kept = Widget.New(10)").IsSuccess(),
          "the State constructs a value after a refused publication");
    Check(Hooks::PublishedUserdataCount(Owner) == 1 && ConstructedCount == 2,
          "exactly the recovered value is published");
  }
  Check(DestroyCalls == 2 && DeallocateCalls == 2,
        "State destruction releases only the value it still owned");

  // A publication the identity cache satisfied established no owner of its own,
  // so a refusal after it must leave the value that already existed alone.
  ResetModel();
  {
    Luna::State Owner;
    Check(RegisterWidget(Owner).IsSuccess(), "the widget class publishes");
    Check(Owner.Execute("Held = Widget.Engine()").IsSuccess(),
          "one borrowed value of the engine object is published");
    const Luna::Detail::ReleaseCounters Before =
        Hooks::UserdataReleaseCounters(Owner);

    Hooks::InjectFault(Owner, FaultPoint::ReturnWrite);
    Check(!Failed(Owner, "Other = Widget.Engine()").empty(),
          "the second access is refused by the injected publication failure");
    Check(Hooks::PublishedUserdataCount(Owner) == 1 &&
              Hooks::OwnedUserdataCount(Owner) == 1,
          "the value that already existed is still owned and published");

    const Luna::Detail::ReleaseCounters After =
        Hooks::UserdataReleaseCounters(Owner);
    Check(After.Invalidate == Before.Invalidate &&
              After.MetadataRelease == Before.MetadataRelease,
          "a refusal after a reused publication releases nothing at all");
    Check(EngineWidget()->Width == 11.0,
          "the engine's own object is left exactly as it was");
    Check(Owner.Execute("assert(typeof(Held) == 'Widget', 'held')").IsSuccess(),
          "the value the script already held is still a value of its class");
  }
}

// -- final release ----------------------------------------------------------

// The last stage: every applicable step exactly once, whichever cause ends the
// value, and a destruction step that throws is contained rather than escaping
// into a collector.
void CheckFinalReleaseRunsEveryStepOnce() {
  // Collection.
  ResetModel();
  {
    Luna::State Owner;
    Check(RegisterWidget(Owner).IsSuccess(), "the widget class publishes");
    Check(Owner.Execute("Held = Widget.New(1)").IsSuccess(),
          "one value is constructed");
    Check(Owner.Execute("Held = nil").IsSuccess(),
          "the script drops its only reference");
    Check(Hooks::CollectGarbage(Owner), "the collector runs to completion");
    Check(DestroyCalls == 1 && DeallocateCalls == 1 && LiveCount == 0,
          "a collected value is destroyed once and deallocated once");
    Check(Hooks::CollectGarbage(Owner), "the collector runs again");
    Check(DestroyCalls == 1 && DeallocateCalls == 1,
          "a second collection performs no second release step");
    Check(Hooks::UserdataReleaseCounters(Owner).IncompleteMetadata == 0,
          "every cleanup step ran with the metadata it needs");
    Check(Hooks::ObserveUserdataCollections().ContainedException == 0,
          "nothing was thrown at the collection boundary");
  }
  Check(DestroyCalls == 1 && DeallocateCalls == 1,
        "State destruction performs no second release of a collected value");

  // A destruction step that throws: contained, counted, and every remaining
  // step still runs.
  ResetModel();
  {
    Luna::State Owner;
    Check(RegisterWidget(Owner).IsSuccess(), "the widget class publishes");
    Check(Owner.Execute("Held = Widget.New(2)").IsSuccess(),
          "one value is constructed");
    Policy.DestructionThrows = true;
    Check(Owner.Execute("Held = nil").IsSuccess(),
          "the script drops its only reference");
    Check(Hooks::CollectGarbage(Owner), "the collector runs to completion");
    Check(DestroyCalls == 1 && DeallocateCalls == 1,
          "a destruction step that throws still counts as the one destruction");

    const Luna::Detail::ReleaseCounters Released =
        Hooks::UserdataReleaseCounters(Owner);
    Check(Released.ContainedException == 1,
          "the exception the destruction step threw was contained once");
    Check(Released.Destroy == 1 && Released.Deallocate == 1 &&
              Released.MetadataRelease == 1,
          "every remaining release step still runs exactly once");
    Check(Hooks::ObserveUserdataCollections().ContainedException == 0,
          "nothing escaped into the collection boundary");
    Policy.DestructionThrows = false;
    Check(Owner.Execute("Again = Widget.New(3)").IsSuccess(),
          "the State constructs a value after a contained destruction failure");
  }

  // State destruction is the last cause, and it releases exactly what the State
  // still owned.
  ResetModel();
  {
    Luna::State Owner;
    Check(RegisterWidget(Owner).IsSuccess(), "the widget class publishes");
    Check(Owner.Execute("A = Widget.New(1)\nB = Widget.New(2)").IsSuccess(),
          "two values are constructed");
    Check(ConstructedCount == 2 && DestroyCalls == 0,
          "nothing is released while the State is still usable");
  }
  Check(DestroyCalls == 2 && DeallocateCalls == 2 && LiveCount == 0,
        "State destruction releases each value it owned exactly once");
  Hooks::ResetUserdataCollections();
}

} // namespace

int RunClassConstructionFaultTests() {
  FailureCount = 0;
  Hooks::ResetUserdataCollections();
  CheckAllocationFailureThroughRealCalls();
  CheckConstructionFailureThroughRealCalls();
  CheckOwnershipFailureAfterConstruction();
  CheckCacheInsertionRefusalThroughRealCalls();
  CheckMetatableAssociationIsCreatedOnceAndReused();
  CheckPublicationFailuresPublishNothing();
  CheckFinalReleaseRunsEveryStepOnce();
  Hooks::ResetUserdataCollections();
  return FailureCount == 0 ? 0 : 1;
}
