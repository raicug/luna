// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_member.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/core/results/execution_result.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/userdata/member_access.hpp"
#include "state/userdata/member_dispatch.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::Detail::ConstAccess;
using Luna::Detail::MemberDispatchStage;
using Luna::Detail::MemberDispatchStageText;
using Luna::Detail::OwnershipModel;
using Luna::Detail::StateFaultPoint;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "member boundary check failed: " << Description << '\n';
}

std::size_t GetterRuns = 0;
std::size_t SetterRuns = 0;
std::size_t ThrowingGetterRuns = 0;
std::size_t MutatingSetterRuns = 0;

void ResetCounters() {
  GetterRuns = 0;
  SetterRuns = 0;
  ThrowingGetterRuns = 0;
  MutatingSetterRuns = 0;
}

struct Gadget final {
  int Charge = 3;
  const int Serial = 42;
  int Trace = 0;
  bool GetterFails = false;

  [[nodiscard]] int Level() const {
    ++GetterRuns;
    return Charge * 2;
  }

  void SetLevel(int Value) {
    ++SetterRuns;
    Charge = Value;
  }

  [[nodiscard]] int Fragile() const {
    ++ThrowingGetterRuns;
    throw std::runtime_error("the fragile getter refused");
  }

  void SetMarked(int Value) {
    ++MutatingSetterRuns;
    Trace = Value;
    throw std::runtime_error("the marking setter refused after mutating");
  }

  [[nodiscard]] int Grow(int By) {
    Charge += By;
    return Charge;
  }
};

[[nodiscard]] Luna::RegistrationResult
RegisterGadget(Luna::BindingRegistry &Registry) {
  Luna::ClassBuilder<Gadget> Class = Registry.RegisterClass<Gadget>(
      "Gadget", Luna::StableTypeKey("Studio.BoundaryGadget"));
  Luna::ClassBuilder<Gadget> &WithLevel =
      Class.Property("Level", &Gadget::Level, &Gadget::SetLevel);
  Luna::ClassBuilder<Gadget> &WithFragile =
      WithLevel.Property("Fragile", &Gadget::Fragile);
  Luna::ClassBuilder<Gadget> &WithMarked = WithFragile.Property(
      "Marked", Luna::PropertyPolicy::WriteOnly(), &Gadget::SetMarked);
  Luna::ClassBuilder<Gadget> &WithCharge =
      WithMarked.Field("Charge", &Gadget::Charge);
  Luna::ClassBuilder<Gadget> &WithSerial =
      WithCharge.Field("Serial", &Gadget::Serial);
  Luna::ClassBuilder<Gadget> &WithMethod =
      WithSerial.Method("Grow", &Gadget::Grow);
  return WithMethod.Commit();
}

[[nodiscard]] bool ExposeGadget(Luna::State &Owner, const std::string &Path,
                                Gadget &Object, const std::uint64_t *Generation,
                                ConstAccess Access) {
  Luna::Detail::ClassExposureRequest Request;
  Request.QualifiedName = "Gadget";
  Request.Path = Path;
  Request.Storage = &Object;
  Request.Ownership = OwnershipModel::Borrowed;
  Request.Access = Access;
  Request.LifetimeGeneration = Generation;
  return Hooks::ExposeClassUserdata(Owner, Request).Status == "created";
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "member boundary source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

[[nodiscard]] std::string Refusal(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return std::string();
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  return Diagnostic ? Diagnostic->Message() : std::string("<no diagnostic>");
}

[[nodiscard]] bool Contains(std::string_view Text, std::string_view Needle) {
  return Text.find(Needle) != std::string_view::npos;
}

[[nodiscard]] int ScriptResult(Luna::State &Owner, const std::string &Source) {
  if (!Succeeds(Owner, Source))
    return -1;
  const auto Observed = Hooks::ObserveIntegerGlobal(Owner, "Result");
  return Observed ? *Observed : -1;
}

[[nodiscard]] bool RestoredCheckpoint(const Luna::State &Owner) {
  const auto Observation = Hooks::ObserveLastCallbackStackRestoration(Owner);
  return Observation.has_value() &&
         Observation->EntryDepth == Observation->RestoredDepth &&
         Observation->ErrorDepth == Observation->RestoredDepth + 1;
}

[[nodiscard]] bool RefusedAt(const Luna::State &Owner,
                             MemberDispatchStage Stage,
                             std::string_view Boundary) {
  const auto Observed = Hooks::ObserveLastClassMemberDispatch(Owner);
  if (!Observed || !Observed->Attempted || Observed->Succeeded)
    return false;
  if (MemberDispatchStageText(Observed->Stage) !=
      MemberDispatchStageText(Stage))
    return false;
  if (Luna::Detail::MemberSideEffectBoundaryText(Observed->Boundary) !=
      Boundary)
    return false;

  return Observed->PublishedCount == 0 &&
         Observed->EntryDepth == Observed->RestoredDepth;
}

void CheckMembersAreReachableThroughTheVirtualMachine() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterGadget(Registry).IsSuccess(), "the class registers");

  Gadget Object;
  std::uint64_t Generation = 1;
  Check(ExposeGadget(Owner, "Value", Object, &Generation, ConstAccess::Mutable),
        "one mutable value of the class is exposed");

  const int EntryDepth = Hooks::ObserveRootStackDepth(Owner).value_or(-1);

  Check(ScriptResult(Owner, "Result = Value.Level") == 6 && GetterRuns == 1,
        "a property read reaches the declared getter through the metatable");
  Check(ScriptResult(Owner, "Result = Value.Charge") == 3,
        "a field read reaches the object through its generated getter");

  Check(Succeeds(Owner, "Value.Charge = 11") && Object.Charge == 11,
        "a field write reaches the object through its generated setter");
  Check(Succeeds(Owner, "Value.Level = 5") && SetterRuns == 1 &&
            Object.Charge == 5,
        "a property write reaches the declared setter");

  Check(ScriptResult(Owner, "Result = Value:Grow(2)") == 7,
        "a declared method is still reached through the class table");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every member access returns the root stack to its entry depth");
}

void CheckRefusalsBeforeUserCodeLeaveTheObjectUnchanged() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterGadget(Registry).IsSuccess(), "the class registers");

  Gadget Object;
  Gadget Frozen;
  std::uint64_t Generation = 1;
  Check(ExposeGadget(Owner, "Value", Object, &Generation, ConstAccess::Mutable),
        "one mutable value of the class is exposed");
  Check(ExposeGadget(Owner, "Frozen", Frozen, &Generation, ConstAccess::Const),
        "one const value of the class is exposed");

  const int Before = Object.Charge;
  const std::string Mistyped = Refusal(Owner, "Value.Charge = 'nine'");
  Check(Contains(Mistyped, "Member 'Gadget.Charge' value"),
        "a refused member value names the class and member qualified name");
  Check(Contains(Mistyped, "expected signed 32-bit integer") &&
            Contains(Mistyped, "received string"),
        "the refusal keeps the foundation's own classification");
  Check(Object.Charge == Before,
        "a refused conversion leaves the native object exactly as it was");
  Check(RefusedAt(Owner, MemberDispatchStage::Value, "before_user_code"),
        "a refused conversion is reported before user code and publishes "
        "nothing");
  Check(RestoredCheckpoint(Owner),
        "a refused conversion restores the callback checkpoint exactly");

  const std::string Const = Refusal(Owner, "Frozen.Charge = 1");
  Check(Contains(Const, "Member 'Gadget.Charge' receiver") &&
            Contains(Const, "const view"),
        "a const receiver is refused as the receiver of that member");
  Check(Frozen.Charge == 3 && SetterRuns == 0,
        "a refused receiver never reached native code");
  Check(RefusedAt(Owner, MemberDispatchStage::Receiver, "before_user_code"),
        "a refused receiver is reported before user code");
  Check(RestoredCheckpoint(Owner), "a refused receiver restores the stack");

  const std::string ReadOnly = Refusal(Owner, "Value.Serial = 1");
  Check(Contains(ReadOnly, "Member 'Gadget.Serial' permits no write."),
        "a const field permits no write and says which member refused");
  Check(RefusedAt(Owner, MemberDispatchStage::Direction, "before_user_code"),
        "an unwritable member is refused at the direction step");

  const std::string WriteOnly = Refusal(Owner, "Result = Value.Marked");
  Check(Contains(WriteOnly, "Member 'Gadget.Marked' permits no read."),
        "a write-only property permits no read and says which member refused");
  Check(RefusedAt(Owner, MemberDispatchStage::Direction, "before_user_code"),
        "an unreadable member is refused at the direction step");

  const std::string Unknown = Refusal(Owner, "Result = Value.Missing");
  Check(Contains(Unknown, "Class 'Gadget' declares no member 'Missing'."),
        "a name the class never declared names the class");
  Check(
      RefusedAt(Owner, MemberDispatchStage::UnknownMember, "before_user_code"),
      "an unknown member is refused before user code");

  const std::string Method = Refusal(Owner, "Value.Grow = 1");
  Check(Contains(Method, "Member 'Gadget.Grow' is a declared method"),
        "an assignment to a declared method is refused as that method");
  Check(
      RefusedAt(Owner, MemberDispatchStage::UnknownMember, "before_user_code"),
      "an assignment to a method is refused before user code");

  Check(Owner.IsReady() && ScriptResult(Owner, "Result = Value.Charge") == 3,
        "the State keeps reading members after every refusal");
}

void CheckRefusalsAfterUserCodeOnlyRollBackTheVirtualMachine() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterGadget(Registry).IsSuccess(), "the class registers");

  Gadget Object;
  std::uint64_t Generation = 1;
  Check(ExposeGadget(Owner, "Value", Object, &Generation, ConstAccess::Mutable),
        "one mutable value of the class is exposed");

  const int EntryDepth = Hooks::ObserveRootStackDepth(Owner).value_or(-1);

  const std::string Thrown = Refusal(Owner, "Result = Value.Fragile");
  Check(Contains(Thrown, "Runtime error:") &&
            Contains(Thrown, "member 'Gadget.Fragile' getter threw:") &&
            Contains(Thrown, "the fragile getter refused"),
        "a throwing getter keeps the foundation's exception translation and "
        "names the member");
  Check(ThrowingGetterRuns == 1, "the declared getter ran exactly once");
  Check(RefusedAt(Owner, MemberDispatchStage::Target, "after_user_code"),
        "a throwing getter is reported after user code and publishes nothing");
  Check(RestoredCheckpoint(Owner),
        "a throwing getter restores the callback checkpoint exactly");
  Check(ScriptResult(Owner, "Result = 0\nif Value.Charge == 3 then Result = 1 "
                            "end") == 1,
        "the refused read left no script-visible partial result behind");

  Check(Object.Trace == 0, "the object starts unmarked");
  const std::string Marked = Refusal(Owner, "Value.Marked = 7");
  Check(Contains(Marked, "member 'Gadget.Marked' setter threw:"),
        "a throwing setter names the member and the direction");
  Check(MutatingSetterRuns == 1, "the declared setter ran exactly once");
  Check(Object.Trace == 7,
        "the native side effect the consumer's own code performed survives, "
        "because Luna promises no native rollback after user code starts");
  Check(RefusedAt(Owner, MemberDispatchStage::Target, "after_user_code"),
        "a throwing setter is reported after user code");
  Check(RestoredCheckpoint(Owner),
        "a throwing setter still restores the callback checkpoint exactly");

  Hooks::InjectFault(Owner, StateFaultPoint::MemberValuePublication, 1);
  const std::string Unpublished = Refusal(Owner, "Result = Value.Level");
  Check(Contains(Unpublished, "member 'Gadget.Level'") &&
            Contains(Unpublished, "could not be published"),
        "a refused publication names the member whose value it was");
  Check(GetterRuns == 1, "the getter that produced the value ran once");
  Check(RefusedAt(Owner, MemberDispatchStage::Publication, "after_user_code"),
        "a refused publication is reported after user code and publishes "
        "nothing");
  Check(RestoredCheckpoint(Owner),
        "a refused publication restores the callback checkpoint exactly");
  Check(Hooks::PendingFaults(Owner, StateFaultPoint::MemberValuePublication) ==
            0,
        "the injected publication fault was consumed exactly once");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every refusal returns the root stack to its entry depth");
  Check(ScriptResult(Owner, "Result = Value.Level") == 6,
        "the State keeps reading the same member afterwards");
}

void CheckMemberDiagnosticsNameOneSubject() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterGadget(Registry).IsSuccess(), "the class registers");

  Gadget Object;
  std::uint64_t Generation = 1;
  Check(ExposeGadget(Owner, "Value", Object, &Generation, ConstAccess::Mutable),
        "one mutable value of the class is exposed");

  const std::string Argument = Refusal(Owner, "Result = Value:Grow('x')");
  Check(Contains(Argument, "Member 'Gadget.Grow' argument 1"),
        "an ordinary argument of a member names the member, not a bare "
        "callable");
  Check(Contains(Argument, "expected signed 32-bit integer"),
        "the argument refusal keeps the foundation's classification");
  Check(RestoredCheckpoint(Owner),
        "a refused member argument restores the callback checkpoint");

  const std::string Receiver = Refusal(Owner, "Result = Gadget.Grow()");
  Check(Contains(Receiver, "Member 'Gadget.Grow' receiver"),
        "the receiver refusal of the same member names the same subject");

  const std::string Getter = Refusal(Owner, "Result = Value.Fragile");
  Check(Contains(Getter, "member 'Gadget.Fragile' getter"),
        "the getter refusal of the same class names its member the same way");
  Check(Object.Charge == 3, "no refusal changed the native object");
}

} // namespace

int RunClassMemberBoundaryTests();

int RunClassMemberBoundaryTests() {
  FailureCount = 0;
  CheckMembersAreReachableThroughTheVirtualMachine();
  CheckRefusalsBeforeUserCodeLeaveTheObjectUnchanged();
  CheckRefusalsAfterUserCodeOnlyRollBackTheVirtualMachine();
  CheckMemberDiagnosticsNameOneSubject();
  return FailureCount == 0 ? 0 : 1;
}
