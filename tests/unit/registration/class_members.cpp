// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_member.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/binding/value.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/registration/member_plan.hpp"
#include "state/registration/plan.hpp"
#include "state/testing/test_hooks.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::Detail::MemberCollision;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "class member check failed: " << Description << '\n';
}

std::size_t LevelReads = 0;
std::size_t LevelWrites = 0;
std::size_t WeightReads = 0;
std::size_t ExpensiveReads = 0;
std::size_t NotifiedChanges = 0;
std::size_t TrackedChanges = 0;

struct Gadget final {
  int Charge = 3;
  const int Serial = 42;
  std::string Label = "gadget";
  bool GetterFails = false;
  int Notified = 0;
  int Tracked = 0;

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
  TrackedChanges = 0;
}

[[nodiscard]] Luna::StableTypeKey GadgetKey() {
  return Luna::StableTypeKey("Studio.Gadget");
}

[[nodiscard]] Luna::RegistrationResult
RegisterGadget(Luna::BindingRegistry &Registry) {
  Luna::ClassBuilder<Gadget> Class =
      Registry.RegisterClass<Gadget>("Gadget", GadgetKey());
  Luna::ClassBuilder<Gadget> &WithLevel =
      Class.Property("Level", &Gadget::Level, &Gadget::SetLevel);
  Luna::ClassBuilder<Gadget> &WithWeight =
      WithLevel.Property("Weight", &Gadget::Weight);
  Luna::ClassBuilder<Gadget> &WithComputed = WithWeight.Property(
      "Computed", Luna::PropertyPolicy::Computed(), &Gadget::Level);
  Luna::ClassBuilder<Gadget> &WithLazy = WithComputed.Property(
      "Expensive", Luna::PropertyPolicy::Lazy(), &Gadget::Expensive);
  Luna::ClassBuilder<Gadget> &WithLazyPair =
      WithLazy.Property("Cached", Luna::PropertyPolicy::LazyReadWrite(),
                        &Gadget::Level, &Gadget::SetLevel);
  Luna::ClassBuilder<Gadget> &WithHidden = WithLazyPair.Property(
      "Hidden", Luna::PropertyPolicy::WriteOnly(), &Gadget::SetLevel);
  Luna::ClassBuilder<Gadget> &WithCharge =
      WithHidden.Field("Charge", &Gadget::Charge);
  Luna::ClassBuilder<Gadget> &WithSerial =
      WithCharge.Field("Serial", &Gadget::Serial);
  Luna::ClassBuilder<Gadget> &WithLabel =
      WithSerial.Field("Label", &Gadget::Label, Luna::FieldPolicy::ReadOnly());
  Luna::ClassBuilder<Gadget> &WithNotifiedProperty =
      WithLabel.Property("Notified", &Gadget::Level, &Gadget::SetLevel,
                         [](Gadget &Instance, const int &) {
                           ++NotifiedChanges;
                           ++Instance.Notified;
                         });
  Luna::ClassBuilder<Gadget> &WithTrackedField = WithNotifiedProperty.Field(
      "Tracked", &Gadget::Charge, [](Gadget &Instance, const int &Updated) {
        ++TrackedChanges;
        Instance.Tracked = Updated;
      });
  Luna::ClassBuilder<Gadget> &Documented = WithTrackedField.Documentation(
      "Level", "The doubled charge of this gadget.");
  Luna::ClassBuilder<Gadget> &Annotated =
      Documented.Attribute("Expensive", "cost", "high");
  return Annotated.Commit();
}

void CheckMembersAreReflected() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(RegisterGadget(Registry).IsSuccess(),
        "one class commits with every property mode and both field forms");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "publishing typed members restores the entry stack depth");
  Check(Hooks::ClassMemberCount(Owner, "Gadget") == 11,
        "every declared member is published with its class");
  Check(Hooks::ClassMemberIsRegistered(Owner, "Gadget", "Level") &&
            Hooks::ClassMemberIsRegistered(Owner, "Gadget", "Charge"),
        "a property and a field are both published as members");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ReflectionRecord Level = Snapshot.Find("Gadget.Level");
  Check(Level.IsValid() && Level.Kind() == Luna::SymbolKind::Property,
        "a declared property reflects as a property");
  Check(Level.IsReadable() && Level.IsWritable() &&
            Level.AccessPolicy() == "read-write",
        "a read-write property reflects both directions");
  Check(Level.Evaluation() == "immediate",
        "an ordinary property reflects immediate evaluation");
  Check(Level.Receiver().IsValid() &&
            Level.Receiver() == *Hooks::ClassTypeOf(Owner, "Gadget"),
        "a member reflects the canonical receiver type it is reached through");
  Check(Level.Type().IsValid() && Level.ReceiverPermitsConst(),
        "a member declared with a const getter reflects a const-readable "
        "receiver and its declared value type");

  const Luna::ReflectionRecord Weight = Snapshot.Find("Gadget.Weight");
  Check(Weight.IsValid() && !Weight.ReceiverPermitsConst(),
        "a member declared with a non-const getter needs a mutable receiver");
  Check(Level.Documentation() == "The doubled charge of this gadget.",
        "a documented member reflects its documentation");
  Check(Level.Scope().Owner() == Snapshot.Find("Gadget").Id(),
        "a member is scoped to the class that declares it");

  const Luna::ReflectionRecord Lazy = Snapshot.Find("Gadget.Expensive");
  Check(Lazy.IsValid() && Lazy.Evaluation() == "lazy" && Lazy.IsReadable() &&
            !Lazy.IsWritable(),
        "an explicitly lazy property reflects its lazy policy");
  Check(Lazy.AttributeCount() == 1 && Lazy.Attribute(0).Name() == "cost" &&
            Lazy.Attribute(0).Value() == "high",
        "an annotated member reflects its attribute");

  const Luna::ReflectionRecord Computed = Snapshot.Find("Gadget.Computed");
  Check(Computed.IsValid() && Computed.Evaluation() == "computed",
        "a computed property reflects computed evaluation");

  const Luna::ReflectionRecord Hidden = Snapshot.Find("Gadget.Hidden");
  Check(Hidden.IsValid() && !Hidden.IsReadable() && Hidden.IsWritable() &&
            Hidden.AccessPolicy() == "write-only",
        "a write-only property reflects only its write direction");

  const Luna::ReflectionRecord Charge = Snapshot.Find("Gadget.Charge");
  Check(Charge.IsValid() && Charge.Kind() == Luna::SymbolKind::Field &&
            Charge.IsReadable() && Charge.IsWritable(),
        "a mutable data member reflects as a writable field");
  Check(Charge.MemberOwnershipPolicy() == "copied",
        "a field reflects the copied ownership it obeys");

  const Luna::ReflectionRecord Serial = Snapshot.Find("Gadget.Serial");
  Check(Serial.IsValid() && Serial.IsReadable() && !Serial.IsWritable(),
        "a const data member is read-only whatever else is stated");

  const Luna::ReflectionRecord Label = Snapshot.Find("Gadget.Label");
  Check(Label.IsValid() && Label.IsReadable() && !Label.IsWritable(),
        "an explicitly read-only field reflects only its read direction");

  Check(Hooks::LazyMemberCacheEntryCount(Owner) == 0,
        "registration records no cached value at all");
}

void CheckContradictoryDeclarationsAreRefused() {
  ResetCounters();

  Luna::State First;
  Luna::BindingRegistry FirstRegistry = First.Bindings();
  Luna::ClassBuilder<Gadget> Contradicted =
      FirstRegistry.RegisterClass<Gadget>("Gadget", GadgetKey());
  Luna::ClassBuilder<Gadget> &WithGetter = Contradicted.Property(
      "Level", Luna::PropertyPolicy::WriteOnly(), &Gadget::Level);
  const Luna::RegistrationResult Refused = WithGetter.Commit();
  Check(!Refused.IsSuccess(),
        "a write-only policy given a getter is refused transactionally");
  Check(Hooks::RegisteredClassCount(First) == 0,
        "a refused member publishes no class at all");

  Luna::State Second;
  Luna::BindingRegistry SecondRegistry = Second.Bindings();
  Luna::ClassBuilder<Gadget> Constant =
      SecondRegistry.RegisterClass<Gadget>("Gadget", GadgetKey());
  Luna::ClassBuilder<Gadget> &WithWritableConstant =
      Constant.Field("Serial", &Gadget::Serial, Luna::FieldPolicy::ReadWrite());
  Check(!WithWritableConstant.Commit().IsSuccess(),
        "a const field asked to permit writes is refused transactionally");

  Luna::State Third;
  Luna::BindingRegistry ThirdRegistry = Third.Bindings();
  Luna::ClassBuilder<Gadget> Borrowed =
      ThirdRegistry.RegisterClass<Gadget>("Gadget", GadgetKey());
  Luna::ClassBuilder<Gadget> &WithBorrowedField =
      Borrowed.Field("Charge", &Gadget::Charge,
                     Luna::FieldPolicy::Owned(Luna::MemberOwnership::Borrowed));
  Check(!WithBorrowedField.Commit().IsSuccess(),
        "a field declaring an ownership other than copied is refused");
}

void CheckMemberCollisionsFollowOneOrder() {
  ResetCounters();

  Luna::State Reserved;
  Luna::BindingRegistry ReservedRegistry = Reserved.Bindings();
  Luna::ClassBuilder<Gadget> ReservedClass =
      ReservedRegistry.RegisterClass<Gadget>("Gadget", GadgetKey());
  Luna::ClassBuilder<Gadget> &WithReserved =
      ReservedClass.Property("__index", &Gadget::Level);
  const Luna::RegistrationResult ReservedResult = WithReserved.Commit();
  Check(!ReservedResult.IsSuccess(),
        "a member in Luna's reserved namespace is refused");
  const Luna::ErrorDiagnostic *ReservedDiagnostic = ReservedResult.Diagnostic();
  Check(ReservedDiagnostic != nullptr &&
            std::string(ReservedDiagnostic->Message()).find("metamethod") !=
                std::string::npos,
        "the reserved-name refusal names Luna's own namespace");

  Luna::State Duplicated;
  Luna::BindingRegistry DuplicateRegistry = Duplicated.Bindings();
  Luna::ClassBuilder<Gadget> DuplicateClass =
      DuplicateRegistry.RegisterClass<Gadget>("Gadget", GadgetKey());
  Luna::ClassBuilder<Gadget> &WithFirst =
      DuplicateClass.Property("Level", &Gadget::Level);
  Luna::ClassBuilder<Gadget> &WithSecond =
      WithFirst.Property("Level", &Gadget::Level);
  Check(!WithSecond.Commit().IsSuccess(),
        "two properties of one name are a same-category duplicate");

  Luna::State Mixed;
  Luna::BindingRegistry MixedRegistry = Mixed.Bindings();
  Luna::ClassBuilder<Gadget> MixedClass =
      MixedRegistry.RegisterClass<Gadget>("Gadget", GadgetKey());
  Luna::ClassBuilder<Gadget> &WithConstructor =
      MixedClass.Constructor<>("Level");
  Luna::ClassBuilder<Gadget> &WithProperty =
      WithConstructor.Property("Level", &Gadget::Level);
  Check(!WithProperty.Commit().IsSuccess(),
        "a property colliding with another category is refused");

  Luna::Detail::MemberCollisionRequest Request;
  Request.Segment = "Level";
  Request.QualifiedName = "Gadget.Level";
  Request.Kind = Luna::SymbolKind::Property;
  Check(Luna::Detail::ClassifyMemberCollision(Request) == MemberCollision::None,
        "an available member name collides with nothing");

  Luna::Detail::MemberCollisionRequest ReservedRequest = Request;
  ReservedRequest.Segment = "__gc";
  ReservedRequest.NameIsDeclared = true;
  ReservedRequest.InheritedNameIsAmbiguous = true;
  Check(Luna::Detail::ClassifyMemberCollision(ReservedRequest) ==
            MemberCollision::ReservedSystemName,
        "a reserved name outranks every later collision");

  Luna::Detail::MemberCollisionRequest SameRequest = Request;
  SameRequest.NameIsDeclared = true;
  SameRequest.ExistingKind = Luna::SymbolKind::Property;
  SameRequest.ExistingCategory = Luna::Detail::PlanEntryKind::ClassMember;
  SameRequest.InheritedNameIsAmbiguous = true;
  Check(Luna::Detail::ClassifyMemberCollision(SameRequest) ==
            MemberCollision::SameCategory,
        "a same-category duplicate outranks an incompatible category and an "
        "inherited ambiguity");

  Luna::Detail::MemberCollisionRequest OtherRequest = SameRequest;
  OtherRequest.ExistingKind = Luna::SymbolKind::Method;
  OtherRequest.ExistingCategory = Luna::Detail::PlanEntryKind::Function;
  Check(Luna::Detail::ClassifyMemberCollision(OtherRequest) ==
            MemberCollision::IncompatibleCategory,
        "a declaration of another category outranks an inherited ambiguity");

  Luna::Detail::MemberCollisionRequest InheritedRequest = Request;
  InheritedRequest.InheritedNameIsAmbiguous = true;
  Check(Luna::Detail::ClassifyMemberCollision(InheritedRequest) ==
            MemberCollision::InheritedAmbiguity,
        "an inherited ambiguity is the last arm of the member collision order");
}

} // namespace

int RunClassMemberTests();

int RunClassMemberTests() {
  FailureCount = 0;
  CheckMembersAreReflected();
  CheckContradictoryDeclarationsAreRefused();
  CheckMemberCollisionsFollowOneOrder();
  return FailureCount == 0 ? 0 : 1;
}
