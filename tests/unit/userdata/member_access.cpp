// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_member.hpp>
#include <luna/binding/value.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/test_hooks.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/lazy_cache.hpp"
#include "state/userdata/ownership.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::Detail::ConstAccess;
using Luna::Detail::OwnershipModel;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "member access check failed: " << Description << '\n';
}

std::size_t LevelReads = 0;
std::size_t LevelWrites = 0;
std::size_t WeightReads = 0;
std::size_t ExpensiveReads = 0;
std::size_t NotifiedChanges = 0;

struct Gadget final {
  int Charge = 3;
  const int Serial = 42;
  std::string Label = "gadget";
  bool GetterFails = false;
  bool NotifyThrows = false;
  int LastNotified = 0;

  [[nodiscard]] int Level() const {
    ++LevelReads;
    return Charge * 2;
  }

  void SetLevel(int Value) {
    ++LevelWrites;
    Charge = Value;
  }

  [[nodiscard]] double Weight() {
    ++WeightReads;
    return 1.5;
  }

  [[nodiscard]] int Expensive() const {
    ++ExpensiveReads;
    if (GetterFails)
      throw std::runtime_error("the expensive getter refused");
    return Charge + 100;
  }
};

void ResetCounters() {
  LevelReads = 0;
  LevelWrites = 0;
  WeightReads = 0;
  ExpensiveReads = 0;
  NotifiedChanges = 0;
}

[[nodiscard]] Luna::RegistrationResult
RegisterGadget(Luna::BindingRegistry &Registry) {
  Luna::ClassBuilder<Gadget> Class = Registry.RegisterClass<Gadget>(
      "Gadget", Luna::StableTypeKey("Studio.Gadget"));
  Luna::ClassBuilder<Gadget> &WithLevel =
      Class.Property("Level", &Gadget::Level, &Gadget::SetLevel);
  Luna::ClassBuilder<Gadget> &WithWeight =
      WithLevel.Property("Weight", &Gadget::Weight);
  Luna::ClassBuilder<Gadget> &WithLazy = WithWeight.Property(
      "Expensive", Luna::PropertyPolicy::Lazy(), &Gadget::Expensive);
  Luna::ClassBuilder<Gadget> &WithHidden = WithLazy.Property(
      "Hidden", Luna::PropertyPolicy::WriteOnly(), &Gadget::SetLevel);
  Luna::ClassBuilder<Gadget> &WithCharge =
      WithHidden.Field("Charge", &Gadget::Charge);
  Luna::ClassBuilder<Gadget> &WithSerial =
      WithCharge.Field("Serial", &Gadget::Serial);
  Luna::ClassBuilder<Gadget> &WithLabel =
      WithSerial.Field("Label", &Gadget::Label);
  Luna::ClassBuilder<Gadget> &WithNotified = WithLabel.Property(
      "Notified", &Gadget::Level, &Gadget::SetLevel,
      [](Gadget &Instance, const int &Updated) {
        if (Instance.NotifyThrows)
          throw std::runtime_error("the on-change handler refused");
        ++NotifiedChanges;
        Instance.LastNotified = Updated;
      });
  return WithNotified.Commit();
}

[[nodiscard]] std::string ExposeGadget(Luna::State &Owner,
                                       const std::string &Path, Gadget &Object,
                                       const std::uint64_t *Generation,
                                       ConstAccess Access) {
  Luna::Detail::ClassExposureRequest Request;
  Request.QualifiedName = "Gadget";
  Request.Path = Path;
  Request.Storage = &Object;
  Request.Ownership = OwnershipModel::Borrowed;
  Request.Access = Access;
  Request.LifetimeGeneration = Generation;
  const Luna::Detail::ClassExposureObservation Exposed =
      Hooks::ExposeClassUserdata(Owner, Request);
  return Exposed.Status;
}

[[nodiscard]] Luna::Detail::ClassMemberAccessObservation
ReadMember(Luna::State &Owner, const std::string &Path,
           const std::string &Member) {
  Luna::Detail::ClassMemberAccessRequest Request;
  Request.QualifiedName = "Gadget";
  Request.Member = Member;
  Request.Path = Path;
  return Hooks::ReadClassMemberValue(Owner, Request);
}

[[nodiscard]] Luna::Detail::ClassMemberAccessObservation
WriteMember(Luna::State &Owner, const std::string &Path,
            const std::string &Member, Luna::Value Incoming) {
  Luna::Detail::ClassMemberAccessRequest Request;
  Request.QualifiedName = "Gadget";
  Request.Member = Member;
  Request.Path = Path;
  Request.Incoming = std::move(Incoming);
  return Hooks::WriteClassMemberValue(Owner, Request);
}

[[nodiscard]] int
IntegerOf(const Luna::Detail::ClassMemberAccessObservation &Observed) {
  if (!Observed.Produced)
    return -1;
  const int *Held = std::get_if<int>(&*Observed.Produced);
  return Held ? *Held : -1;
}

void CheckTypedAccessFollowsItsDeclaration() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterGadget(Registry).IsSuccess(), "the class registers");

  Gadget Object;
  std::uint64_t Generation = 1;
  Check(ExposeGadget(Owner, "Gadget_Value", Object, &Generation,
                     ConstAccess::Mutable) == "created",
        "one mutable value of the class is exposed");

  const auto Level = ReadMember(Owner, "Gadget_Value", "Level");
  Check(Level.Reached && IntegerOf(Level) == 6 && LevelReads == 1,
        "a read-write property reads through its declared getter exactly once");

  const auto Written = WriteMember(Owner, "Gadget_Value", "Level", 21);
  Check(Written.Reached && LevelWrites == 1 && Object.Charge == 21,
        "a read-write property writes through its declared setter");

  const auto Field = ReadMember(Owner, "Gadget_Value", "Charge");
  Check(
      Field.Reached && IntegerOf(Field) == 21,
      "a field reads the value the object holds through its generated getter");

  const auto FieldWritten = WriteMember(Owner, "Gadget_Value", "Charge", 9);
  Check(FieldWritten.Reached && Object.Charge == 9,
        "a writable field writes through its generated setter");

  const int Before = Object.Charge;
  const auto Mistyped =
      WriteMember(Owner, "Gadget_Value", "Charge", std::string("nine"));
  Check(!Mistyped.Reached && Mistyped.Failure == "incompatible_value" &&
            Object.Charge == Before,
        "a value of another canonical type is refused before the setter runs");

  const auto ConstantWrite = WriteMember(Owner, "Gadget_Value", "Serial", 1);
  Check(!ConstantWrite.Reached && ConstantWrite.Failure == "unwritable_member",
        "a const field permits no write");
  const auto HiddenRead = ReadMember(Owner, "Gadget_Value", "Hidden");
  Check(!HiddenRead.Reached && HiddenRead.Failure == "unreadable_member",
        "a write-only property permits no read");

  const auto Weight = ReadMember(Owner, "Gadget_Value", "Weight");
  Check(Weight.Reached && WeightReads == 1,
        "a non-const getter reads through a mutable view");

  const auto Unknown = ReadMember(Owner, "Gadget_Value", "Missing");
  Check(!Unknown.Reached && Unknown.Failure == "unknown_member",
        "a member this class never declared is not accessible at all");
}

void CheckOnChangeHandlerRunsAfterASuccessfulWrite() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterGadget(Registry).IsSuccess(), "the class registers");

  Gadget Object;
  std::uint64_t Generation = 5;
  Check(ExposeGadget(Owner, "Notified_Value", Object, &Generation,
                     ConstAccess::Mutable) == "created",
        "one mutable value of the class is exposed");

  const auto Written = WriteMember(Owner, "Notified_Value", "Notified", 30);
  Check(Written.Reached && NotifiedChanges == 1 && Object.LastNotified == 30 &&
            Object.Charge == 30,
        "a successful write invokes the declared on-change handler with the "
        "new value");

  const int Before = Object.Charge;
  const auto Mistyped =
      WriteMember(Owner, "Notified_Value", "Notified", std::string("thirty"));
  Check(!Mistyped.Reached && NotifiedChanges == 1 && Object.Charge == Before,
        "an incompatible value never reaches the on-change handler");

  Object.NotifyThrows = true;
  const auto Thrown = WriteMember(Owner, "Notified_Value", "Notified", 40);
  Check(!Thrown.Reached && Thrown.Failure == "contained_exception" &&
            Object.Charge == 40 && NotifiedChanges == 1,
        "an on-change handler that throws is contained rather than escaping, "
        "though the underlying write already took effect");
  Object.NotifyThrows = false;
}

void CheckReceiverRanksBeforeEverythingElse() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterGadget(Registry).IsSuccess(), "the class registers");

  Gadget Object;
  std::uint64_t Generation = 4;
  Check(ExposeGadget(Owner, "Const_Value", Object, &Generation,
                     ConstAccess::Const) == "created",
        "one const value of the class is exposed");

  const auto Read = ReadMember(Owner, "Const_Value", "Level");
  Check(Read.Reached && LevelReads == 1,
        "a const receiver permits a const-declared read");

  const int Before = Object.Charge;
  const auto Refused = WriteMember(Owner, "Const_Value", "Level", 11);
  Check(!Refused.Reached && Refused.Failure == "refused_receiver" &&
            Refused.Receiver == "const_violation" && LevelWrites == 0 &&
            Object.Charge == Before,
        "a const receiver refuses a setter before the setter runs");

  const auto RefusedField = WriteMember(Owner, "Const_Value", "Charge", 11);
  Check(!RefusedField.Reached && RefusedField.Receiver == "const_violation" &&
            Object.Charge == Before,
        "a const receiver refuses a writable field before native code");

  const auto MutableGetter = ReadMember(Owner, "Const_Value", "Weight");
  Check(!MutableGetter.Reached && MutableGetter.Failure == "refused_receiver" &&
            MutableGetter.Receiver == "const_violation" && WeightReads == 0,
        "a getter declared on a mutable object is refused at a const view");

  ++Generation;
  const auto Expired = WriteMember(Owner, "Const_Value", "Serial", 1);
  Check(!Expired.Reached && Expired.Failure == "refused_receiver" &&
            Expired.Receiver == "expired_lifetime_handle",
        "an expired receiver is reported before the member's own direction");
}

void CheckLazyValuesAreCachedAndInvalidated() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterGadget(Registry).IsSuccess(), "the class registers");

  Gadget First;
  Gadget Second;
  std::uint64_t Generation = 2;
  Check(ExposeGadget(Owner, "First_Value", First, &Generation,
                     ConstAccess::Mutable) == "created" &&
            ExposeGadget(Owner, "Second_Value", Second, &Generation,
                         ConstAccess::Mutable) == "created",
        "two values of the class are exposed");
  Check(Hooks::LazyMemberCacheEntryCount(Owner) == 0,
        "an exposed value starts with no cached member value");

  const auto FirstRead = ReadMember(Owner, "First_Value", "Expensive");
  Check(FirstRead.Reached && ExpensiveReads == 1 &&
            !FirstRead.ServedFromCache && FirstRead.Recorded,
        "the first read of a lazy property runs its getter and records the "
        "value");
  Check(Hooks::LazyMemberCacheEntryCountOf(Owner, &First) == 1 &&
            Hooks::LazyMemberCacheEntryCountOf(Owner, &Second) == 0,
        "a cached value belongs to exactly one exposed object");
  Check(Hooks::ClassUserdataNamesLazyEntries(Owner, "First_Value"),
        "the userdata header names the Luna-owned entries of its value");
  Check(Hooks::ClassUserdataLazyGeneration(Owner, "First_Value") ==
            Hooks::LazyMemberCacheGenerationOf(Owner, &First),
        "the header slot and the entries agree on their dispatch generation");

  const auto Reused = ReadMember(Owner, "First_Value", "Expensive");
  Check(Reused.Reached && ExpensiveReads == 1 && Reused.ServedFromCache,
        "a second read of a lazy property reuses the recorded value");

  const auto SecondRead = ReadMember(Owner, "Second_Value", "Expensive");
  Check(SecondRead.Reached && ExpensiveReads == 2 &&
            !SecondRead.ServedFromCache,
        "a lazy value is cached per userdata, not per member");

  const auto Invalidating = WriteMember(Owner, "First_Value", "Charge", 7);
  Check(Invalidating.Reached && Invalidating.Invalidated == 1 &&
            Hooks::LazyMemberCacheEntryCountOf(Owner, &First) == 0,
        "a successful field write invalidates the cached values of its object");
  Check(Hooks::LazyMemberCacheEntryCountOf(Owner, &Second) == 1,
        "a write to one object leaves another object's cached value alone");

  const auto AfterWrite = ReadMember(Owner, "First_Value", "Expensive");
  Check(AfterWrite.Reached && ExpensiveReads == 3 &&
            !AfterWrite.ServedFromCache && IntegerOf(AfterWrite) == 107,
        "the read after an invalidating write runs the getter again");

  const auto RefusedWrite =
      WriteMember(Owner, "First_Value", "Charge", std::string("seven"));
  Check(!RefusedWrite.Reached &&
            Hooks::LazyMemberCacheEntryCountOf(Owner, &First) == 1,
        "a refused write invalidates nothing at all");

  Check(Hooks::InvalidateClassMemberCache(Owner, "First_Value") == 1 &&
            Hooks::LazyMemberCacheEntryCountOf(Owner, &First) == 0,
        "an explicit invalidation drops the cached values of its object");

  Second.GetterFails = true;
  Check(Hooks::InvalidateClassMemberCache(Owner, "Second_Value") == 1,
        "the second object's recorded value is dropped before it is retried");
  const std::size_t BeforeFailure = ExpensiveReads;
  const auto Failed = ReadMember(Owner, "Second_Value", "Expensive");
  Check(!Failed.Reached && Failed.Failure == "contained_exception" &&
            ExpensiveReads == BeforeFailure + 1,
        "a getter that throws is contained rather than escaping");
  Check(Hooks::LazyMemberCacheEntryCountOf(Owner, &Second) == 0,
        "a failed lazy getter records nothing");
  Second.GetterFails = false;

  const auto Recorded = ReadMember(Owner, "Second_Value", "Expensive");
  Check(Recorded.Reached && Recorded.Recorded &&
            Hooks::LiveLazyMemberCacheEntryCount(Owner) == 1,
        "a successful lazy getter records one live value");
  Check(Hooks::AdvanceLifecycleGeneration(Owner),
        "the registered model is replaced by a later generation");
  Check(Hooks::LazyMemberCacheEntryCount(Owner) == 1 &&
            Hooks::LiveLazyMemberCacheEntryCount(Owner) == 0,
        "a generation change invalidates every earlier entry by mismatch");

  const Luna::Detail::LazyCacheCounters Before =
      Hooks::LazyMemberCacheCounters(Owner);
  const auto AfterGeneration = ReadMember(Owner, "Second_Value", "Expensive");
  const Luna::Detail::LazyCacheCounters After =
      Hooks::LazyMemberCacheCounters(Owner);
  Check(AfterGeneration.Reached && !AfterGeneration.ServedFromCache &&
            After.GenerationMismatch == Before.GenerationMismatch + 1,
        "a read under a later generation misses by mismatch and runs the "
        "getter");
  Check(Hooks::LiveLazyMemberCacheEntryCount(Owner) == 1 &&
            Hooks::LazyMemberCacheEntryCount(Owner) == 1,
        "the replacement generation records its own value in place of the "
        "stale one");
}

void CheckRetiringOneValueDropsItsCachedValues() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterGadget(Registry).IsSuccess(), "the class registers");

  Gadget Object;
  std::uint64_t Generation = 3;
  Check(ExposeGadget(Owner, "Retired_Value", Object, &Generation,
                     ConstAccess::Mutable) == "created",
        "one value of the class is exposed");

  const auto Read = ReadMember(Owner, "Retired_Value", "Expensive");
  Check(Read.Reached && Hooks::LazyMemberCacheNodeCount(Owner) == 1,
        "one exposed object holds one entry node");

  Check(Hooks::RetireClassUserdata(Owner, &Object),
        "the exposed value is retired ahead of any payload release");
  Check(Hooks::LazyMemberCacheNodeCount(Owner) == 0 &&
            Hooks::LazyMemberCacheEntryCount(Owner) == 0,
        "retiring one value drops its entry node before anything is released");
  Check(Hooks::ClassUserdataNamesLazyEntries(Owner, "Retired_Value") == false,
        "the retired value's header names no entries any more");

  const auto AfterRetire = ReadMember(Owner, "Retired_Value", "Expensive");
  Check(!AfterRetire.Reached && AfterRetire.Failure == "refused_receiver",
        "a retired value refuses every later member access");
  Check(Hooks::LazyMemberCacheEntryCount(Owner) == 0,
        "a refused access records nothing");
  Check(Hooks::UserdataReleaseCounters(Owner).IncompleteMetadata == 0,
        "no cleanup step ran without the metadata it requires");
}

} // namespace

int RunClassMemberAccessTests();

int RunClassMemberAccessTests() {
  FailureCount = 0;
  CheckTypedAccessFollowsItsDeclaration();
  CheckOnChangeHandlerRunsAfterASuccessfulWrite();
  CheckReceiverRanksBeforeEverythingElse();
  CheckLazyValuesAreCachedAndInvalidated();
  CheckRetiringOneValueDropsItsCachedValues();
  return FailureCount == 0 ? 0 : 1;
}
