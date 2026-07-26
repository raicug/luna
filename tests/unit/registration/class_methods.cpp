// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/instance_receiver.hpp>
#include <luna/binding/lifetime_handle.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/binding/overload.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/test_hooks.hpp"

#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
  std::cerr << "class method check failed: " << Description << '\n';
}

// How often each declared member target actually ran. An instance member runs
// on exactly the object the call site supplied, so these counters are what
// prove a refused receiver never reached native code at all.
std::size_t ReadCalls = 0;
std::size_t MutateCalls = 0;
std::size_t StaticCalls = 0;

void ResetCounters() {
  ReadCalls = 0;
  MutateCalls = 0;
  StaticCalls = 0;
}

// One ordinary value class with const and non-const members, an overload set,
// and one static member.
struct Vector3 final {
  double X = 0.0;
  double Y = 0.0;
  double Z = 0.0;

  Vector3() = default;
  Vector3(double XValue, double YValue, double ZValue)
      : X(XValue), Y(YValue), Z(ZValue) {}

  [[nodiscard]] double Largest() const {
    ++ReadCalls;
    double Result = X;
    if (Y > Result)
      Result = Y;
    if (Z > Result)
      Result = Z;
    return Result;
  }

  void Grow(double Factor) {
    ++MutateCalls;
    X *= Factor;
    Y *= Factor;
    Z *= Factor;
  }

  [[nodiscard]] double Combine(double First) const {
    ++ReadCalls;
    return X + First;
  }

  [[nodiscard]] double Combine(double First, double Second) const {
    ++ReadCalls;
    return X + First + Second;
  }

  [[nodiscard]] static double Dimensions() {
    ++StaticCalls;
    return 3.0;
  }

  void Throwing() {
    ++MutateCalls;
    throw std::runtime_error("no growth today");
  }
};

// One polymorphic class, so a virtual member proves it dispatches through the
// object the call site supplied rather than through the declared class.
struct Shape {
  virtual ~Shape() = default;

  [[nodiscard]] virtual int Sides() const {
    ++ReadCalls;
    return 0;
  }
};

struct Square final : Shape {
  [[nodiscard]] int Sides() const override {
    ++ReadCalls;
    return 4;
  }
};

[[nodiscard]] Luna::StableTypeKey Vector3Key() {
  return Luna::StableTypeKey("Studio.MemberVector3");
}

[[nodiscard]] Luna::StableTypeKey ShapeKey() {
  return Luna::StableTypeKey("Studio.MemberShape");
}

[[nodiscard]] std::shared_ptr<Shape> MakeSquare() {
  return std::static_pointer_cast<Shape>(std::make_shared<Square>());
}

// One explicit wrapper member: it states the object it operates on through its
// first parameter instead of through a member pointer.
[[nodiscard]] double SumOf(const Vector3 &Source) {
  ++ReadCalls;
  return Source.X + Source.Y + Source.Z;
}

// The whole member surface of one class, declared as one plan.
[[nodiscard]] Luna::RegistrationResult
RegisterVector3(Luna::BindingRegistry &Registry) {
  Luna::ClassBuilder<Vector3> Class =
      Registry.RegisterClass<Vector3>("Vector3", Vector3Key());
  Luna::ClassBuilder<Vector3> &WithConstructor =
      Class.Constructor<double, double, double>();
  Luna::ClassBuilder<Vector3> &WithLargest =
      WithConstructor.Method("Largest", &Vector3::Largest);
  Luna::ClassBuilder<Vector3> &WithGrow =
      WithLargest.Method("Grow", &Vector3::Grow);
  Luna::ClassBuilder<Vector3> &WithSum = WithGrow.Method("Sum", &SumOf);
  Luna::ClassBuilder<Vector3> &WithFirstCombine = WithSum.Method(
      "Combine", Luna::Overload<double(double), Vector3>(&Vector3::Combine));
  Luna::ClassBuilder<Vector3> &WithSecondCombine = WithFirstCombine.Method(
      "Combine",
      Luna::Overload<double(double, double), Vector3>(&Vector3::Combine));

  // One member name declared twice, once for a mutable object and once for a
  // const one. A mutable receiver prefers the member that may mutate it; a
  // const receiver reaches only the one that may not.
  auto Adjusting = [](Vector3 &Source, double Factor) {
    ++MutateCalls;
    Source.X *= Factor;
    return Source.X;
  };
  auto Reading = [](const Vector3 &Source, double Factor) {
    ++ReadCalls;
    return Source.X * Factor;
  };
  Luna::ClassBuilder<Vector3> &WithMutating =
      WithSecondCombine.Method("Adjust", Adjusting);
  Luna::ClassBuilder<Vector3> &WithReading =
      WithMutating.Method("Adjust", Reading);

  Luna::ClassBuilder<Vector3> &WithStatic =
      WithReading.StaticMethod("Dimensions", &Vector3::Dimensions);
  Luna::ClassBuilder<Vector3> &WithThrowing =
      WithStatic.Method("Throwing", &Vector3::Throwing);

  // A trailing optional parameter of one member follows exactly the ordinary
  // declared parameter shape.
  auto Offset = [](const Vector3 &Source, std::optional<double> Amount) {
    ++ReadCalls;
    return Source.X + (Amount ? *Amount : 100.0);
  };
  Luna::ClassBuilder<Vector3> &WithOptional =
      WithThrowing.Method("Offset", Offset);

  // A variadic member consumes the call positions after its receiver, and the
  // positions it reports are the ordinary one-based positions of those
  // arguments.
  auto Tally = [](const Vector3 &Source, const Luna::ArgumentView &Rest) {
    ++ReadCalls;
    double Total = Source.X;
    for (std::size_t Index = 0; Index < Rest.Size(); ++Index) {
      if (const std::optional<double> Element = Rest.ToNumber(Index))
        Total += *Element;
    }
    return Total;
  };
  Luna::ClassBuilder<Vector3> &WithVariadic =
      WithOptional.Method("Tally", Tally);

  Luna::ClassBuilder<Vector3> &Documented =
      WithVariadic.Documentation("Largest", "The largest component.");
  Luna::ClassBuilder<Vector3> &Annotated =
      Documented.Attribute("Grow", "Mutates", "yes");
  return Annotated.Commit();
}

[[nodiscard]] bool Succeeds(Luna::State &Host, std::string_view Source) {
  const Luna::ExecutionResult Result = Host.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "class method source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

[[nodiscard]] std::string Refusal(Luna::State &Host, std::string_view Source) {
  const Luna::ExecutionResult Result = Host.Execute(Source);
  if (Result.IsSuccess())
    return std::string();
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  return Diagnostic ? Diagnostic->Message() : std::string("<no diagnostic>");
}

[[nodiscard]] bool Contains(std::string_view Text, std::string_view Needle) {
  return Text.find(Needle) != std::string_view::npos;
}

// One number the script computed, read back without any conversion of its own.
[[nodiscard]] int ScriptResult(Luna::State &Host, const std::string &Source) {
  if (!Succeeds(Host, Source))
    return -1;
  const auto Observed = Hooks::ObserveIntegerGlobal(Host, "Result");
  return Observed ? *Observed : -1;
}

// The callback checkpoint a refused call restores exactly.
[[nodiscard]] bool RestoredCheckpoint(const Luna::State &Host) {
  const auto Observation = Hooks::ObserveLastCallbackStackRestoration(Host);
  return Observation.has_value() &&
         Observation->EntryDepth == Observation->RestoredDepth &&
         Observation->ErrorDepth == Observation->RestoredDepth + 1;
}

// The reflected record of one member candidate of one qualified name.
[[nodiscard]] Luna::ReflectionRecord
CandidateOf(const Luna::ReflectionSnapshot &Taken, Luna::SymbolKind Kind,
            std::string_view QualifiedName, std::size_t ParameterCount) {
  const Luna::ReflectionRecordRange Candidates = Taken.Symbols(Kind);
  for (std::size_t Index = 0; Index < Candidates.Size(); ++Index) {
    const Luna::ReflectionRecord Candidate = Candidates.At(Index);
    if (Candidate.QualifiedName() != QualifiedName)
      continue;
    if (Candidate.ParameterCount() == ParameterCount)
      return Candidate;
  }
  return Luna::ReflectionRecord();
}

void CheckMembersAreReflectedAsMembers() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(RegisterVector3(Registry).IsSuccess(),
        "one class commits with every member candidate as a unit");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "publishing member candidates restores the entry stack depth");
  Check(ReadCalls == 0 && MutateCalls == 0 && StaticCalls == 0,
        "registering members invokes no member at all");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ReflectionRecord Class = Snapshot.Find("Vector3");
  Check(Class.IsValid() && Class.Kind() == Luna::SymbolKind::Class,
        "the class itself is still reflected as one class record");

  const Luna::ReflectionRecord Largest =
      CandidateOf(Snapshot, Luna::SymbolKind::Method, "Vector3.Largest", 0);
  Check(Largest.IsValid(), "an instance method reflects its own candidate");
  Check(Largest.Declaration() == Class.Id(),
        "a method candidate names the class that declares it");
  Check(Largest.Documentation() == "The largest component.",
        "a member candidate reflects its documentation");
  Check(Contains(Largest.Signature(), "Studio.MemberVector3"),
        "a method signature carries the receiver it operates on");
  Check(Contains(Largest.Signature(), "const"),
        "a const method reflects its const receiver");

  const Luna::ReflectionRecord Grow =
      CandidateOf(Snapshot, Luna::SymbolKind::Method, "Vector3.Grow", 1);
  Check(Grow.IsValid() && Grow.ParameterCount() == 1,
        "a non-const method reflects its ordinary parameters only");
  Check(!Contains(Grow.Signature(), "const"),
        "a non-const method reflects a mutable receiver");
  Check(Grow.AttributeCount() == 1,
        "a member candidate reflects its attributes");

  const Luna::ReflectionRecord Dimensions = CandidateOf(
      Snapshot, Luna::SymbolKind::StaticMethod, "Vector3.Dimensions", 0);
  Check(Dimensions.IsValid(),
        "a static method reflects its own static candidate");
  Check(!Contains(Dimensions.Signature(), "Studio.MemberVector3"),
        "a static method reflects no receiver at all");

  // Two candidates of one member name form one canonical overload set, exactly
  // as two constructors of one class do.
  const Luna::ReflectionRecord Combine = Snapshot.Find("Vector3.Combine");
  Check(Combine.IsValid() && Combine.Kind() == Luna::SymbolKind::OverloadSet,
        "several methods of one name form one reflected overload set");
  Check(Hooks::OverloadCandidateCount(Owner, "Vector3.Combine") == 2,
        "two method candidates publish two candidates of one overload set");
  Check(Hooks::OverloadCandidateCount(Owner, "Vector3.Adjust") == 2,
        "a const and a non-const member of one name are two candidates");
}

void CheckColonAndDotCallsAreOneCall() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterVector3(Registry).IsSuccess(), "the class publishes");

  Check(ScriptResult(Owner, "local V = Vector3.New(1, 5, 2)\n"
                            "Result = V:Largest()") == 5,
        "a colon call reaches the member with the object as its receiver");
  Check(ScriptResult(Owner, "local V = Vector3.New(1, 6, 2)\n"
                            "Result = V.Largest(V)") == 6,
        "a dot call with an explicit receiver is the same call");
  Check(ScriptResult(Owner, "local V = Vector3.New(1, 7, 2)\n"
                            "Result = Vector3.Largest(V)") == 7,
        "the class-scope spelling of the same member is the same call");
  Check(ReadCalls == 3, "each of the three spellings ran the member once");

  // The member value reached through the object and the member value reached
  // through the class are one function, not two agreeing ones.
  Check(ScriptResult(Owner, "local V = Vector3.New(1, 1, 1)\n"
                            "Result = 0\n"
                            "if V.Largest == Vector3.Largest then\n"
                            "  Result = 1\n"
                            "end") == 1,
        "an instance reaches exactly the member value its class declares");

  // A virtual member of a nested class dispatches through the object the call
  // site supplied, and its members are reached through the nested class table
  // exactly as a root-scope class's are.
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Shape> Class =
      Studio.RegisterClass<Shape>("Shape", ShapeKey());
  Luna::ClassBuilder<Shape> &WithFactory = Class.Factory("Square", &MakeSquare);
  Luna::ClassBuilder<Shape> &WithSides =
      WithFactory.Method("Sides", &Shape::Sides);
  static_cast<void>(WithSides.QualifiedName());
  Check(Studio.Commit().IsSuccess(), "the nested polymorphic class publishes");
  Check(ScriptResult(Owner, "local S = Studio.Shape.Square()\n"
                            "Result = S:Sides()") == 4,
        "a virtual member dispatches through the supplied object");
  Check(ScriptResult(Owner, "local S = Studio.Shape.Square()\n"
                            "Result = Studio.Shape.Sides(S)") == 4,
        "a nested class reaches its members through its own class table");
}

void CheckOrdinaryArgumentsFollowTheReceiver() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterVector3(Registry).IsSuccess(), "the class publishes");

  Check(ScriptResult(Owner, "local V = Vector3.New(2, 0, 0)\n"
                            "V:Grow(3)\n"
                            "Result = V:Largest()") == 6,
        "a non-const member mutates exactly the object it was called on");
  Check(MutateCalls == 1, "the mutating member ran exactly once");

  // Ordinary arguments keep their own one-based positions: the receiver is rank
  // position zero, not argument one.
  const std::string Message =
      Refusal(Owner, "local V = Vector3.New(1, 1, 1)\nV:Grow('a')");
  Check(Contains(Message, "argument 1"),
        "an ordinary argument of a member keeps its one-based position");
  Check(Contains(Message, "Vector3.Grow"),
        "a member diagnostic names the class and member qualified name");
  Check(MutateCalls == 1, "a refused conversion never reached the member");
  Check(RestoredCheckpoint(Owner),
        "a refused member call restores its callback checkpoint exactly");

  // The same call written as a dot call reports exactly the same argument
  // position, because it is the same call.
  const std::string DotMessage =
      Refusal(Owner, "local V = Vector3.New(1, 1, 1)\nVector3.Grow(V, 'a')");
  Check(DotMessage == Message,
        "a colon call and a dot call report one identical diagnostic");

  Check(ScriptResult(Owner, "local V = Vector3.New(4, 0, 0)\n"
                            "Result = V:Offset()") == 104,
        "an omitted optional parameter of a member uses its declared shape");
  Check(ScriptResult(Owner, "local V = Vector3.New(4, 0, 0)\n"
                            "Result = V:Offset(6)") == 10,
        "a supplied optional parameter of a member converts ordinarily");

  Check(ScriptResult(Owner, "local V = Vector3.New(1, 0, 0)\n"
                            "Result = V:Combine(2)") == 3,
        "one member overload is selected by its ordinary arity");
  Check(ScriptResult(Owner, "local V = Vector3.New(1, 0, 0)\n"
                            "Result = V:Combine(2, 3)") == 6,
        "the other member overload is selected by its ordinary arity");

  Check(ScriptResult(Owner, "local V = Vector3.New(1, 0, 0)\n"
                            "Result = V:Tally(2, 3, 4)") == 10,
        "a variadic member consumes the positions after its receiver");
  const std::string Variadic =
      Refusal(Owner, "local V = Vector3.New(1, 0, 0)\nV:Tally(2, {}, 4)");
  Check(Contains(Variadic, "argument 2"),
        "a variadic member names the first failing ordinary position");
}

void CheckMissingReceiverFailsBeforeArguments() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterVector3(Registry).IsSuccess(), "the class publishes");

  // A dot call with no receiver at all fails receiver validation, ahead of
  // every ordinary-argument decision.
  const std::string Missing = Refusal(Owner, "Vector3.Largest()");
  Check(Contains(Missing, "receiver"),
        "a dot call without a receiver fails receiver validation");
  Check(Contains(Missing, "no value"),
        "the refusal names the receiver that was never supplied");
  Check(!Contains(Missing, "argument 1"),
        "no ordinary-argument diagnostic precedes the receiver refusal");
  Check(ReadCalls == 0, "a refused receiver never reached native code");
  Check(RestoredCheckpoint(Owner),
        "a refused receiver restores the callback checkpoint exactly");

  // A member whose ordinary arguments are supplied but whose receiver is not
  // still fails on the receiver first.
  const std::string Shifted = Refusal(Owner, "Vector3.Grow(3)");
  Check(Contains(Shifted, "receiver"),
        "a shifted dot call fails on its receiver rather than its arguments");
  Check(MutateCalls == 0, "a shifted dot call never reached native code");

  // A value that is not of this class at all is refused by the same gate.
  const std::string Foreign = Refusal(Owner, "Vector3.Largest('text')");
  Check(Contains(Foreign, "receiver") &&
            Contains(Foreign, "not a Luna userdata"),
        "a receiver that is not a Luna userdata is refused as the receiver");
  Check(ReadCalls == 0, "a foreign receiver never reached native code");

  // A value of another registered class carries a metatable this State knows,
  // so the receiver gate refuses it on its dynamic type: no accessible base
  // path and no cast policy relates the two classes.
  Luna::ClassBuilder<Shape> Class =
      Registry.RegisterClass<Shape>("Shape", ShapeKey());
  Luna::ClassBuilder<Shape> &WithFactory = Class.Factory("Square", &MakeSquare);
  Luna::ClassBuilder<Shape> &WithSides =
      WithFactory.Method("Sides", &Shape::Sides);
  Check(WithSides.Commit().IsSuccess(), "the second class publishes");

  const std::string WrongClass = Refusal(Owner, "local S = Shape.Square()\n"
                                                "return Vector3.Largest(S)");
  Check(Contains(WrongClass, "receiver") &&
            Contains(WrongClass, "userdata of another registered class"),
        "a value of another registered class is refused as the receiver");
  Check(ReadCalls == 0, "a wrongly typed receiver never reached native code");

  Check(Owner.IsReady(), "the State remains usable after receiver refusals");
  Check(ScriptResult(Owner, "local V = Vector3.New(9, 0, 0)\n"
                            "Result = V:Largest()") == 9,
        "a later member call still succeeds");
}

void CheckStaticMethodsUseTheOrdinaryPipeline() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterVector3(Registry).IsSuccess(), "the class publishes");

  Check(ScriptResult(Owner, "Result = Vector3.Dimensions()") == 3,
        "a static method is called without any instance at all");
  Check(StaticCalls == 1, "the static member ran exactly once");

  // A static member takes no receiver, so a supplied one is an ordinary arity
  // failure rather than a receiver failure.
  const std::string Message =
      Refusal(Owner, "local V = Vector3.New(1, 1, 1)\nV:Dimensions()");
  Check(!Contains(Message, "receiver"),
        "a static member reports no receiver diagnostic");
  Check(Contains(Message, "expected 0 arguments"),
        "a static member reports the ordinary arity failure");
  Check(StaticCalls == 1, "the refused call never ran the static member");
}

void CheckConstReceiversRejectMutatingMembers() {
  ResetCounters();
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(RegisterVector3(Registry).IsSuccess(), "the class publishes");

  Vector3 Engine(5.0, 1.0, 1.0);
  const Luna::LifetimeHandle Lifetime;

  Luna::Detail::ClassValueExposureRequest Request;
  Request.QualifiedName = "Vector3";
  Request.Path = "Frozen";
  Request.Storage = &Engine;
  Request.Ownership = OwnershipModel::Borrowed;
  Request.Access = ConstAccess::Const;
  Request.Handle = Lifetime;
  Check(Hooks::ExposeClassValue(Owner, Request).Published,
        "one const view of an engine-owned object is published");

  // A const value of the class accepts every member that only reads it.
  Check(ScriptResult(Owner, "Result = Frozen:Largest()") == 5,
        "a const receiver permits a const member");
  Check(ReadCalls == 1, "the const member ran exactly once");

  // It refuses every member that would mutate it, before native code runs.
  const std::string Message = Refusal(Owner, "Frozen:Grow(2)");
  Check(Contains(Message, "receiver") && Contains(Message, "const view"),
        "a const receiver refuses a mutating member");
  Check(MutateCalls == 0, "the refused mutating member never ran");
  Check(Engine.X == 5.0, "the refused member left the native object unchanged");
  Check(RestoredCheckpoint(Owner),
        "a refused const access restores the callback checkpoint exactly");

  // One overload set with a const and a non-const candidate: the const value
  // reaches only the const one, and a mutable value prefers the other.
  Check(ScriptResult(Owner, "Result = Frozen:Adjust(2)") == 10,
        "a const receiver selects the const candidate of one member name");
  Check(ReadCalls == 2 && MutateCalls == 0,
        "the const candidate ran and the mutating candidate did not");

  Check(ScriptResult(Owner, "local V = Vector3.New(3, 0, 0)\n"
                            "Result = V:Adjust(4)") == 12,
        "a mutable receiver prefers the candidate that may mutate it");
  Check(MutateCalls == 1, "the mutating candidate of the set ran exactly once");

  // The owner of the borrowed object ends its lifetime: every later member call
  // on the value exposed through it fails before native code, whether that
  // member reads its object or mutates it.
  Luna::LifetimeHandle Ending = Lifetime;
  Ending.Invalidate();
  const std::string Expired = Refusal(Owner, "Frozen:Largest()");
  Check(Contains(Expired, "receiver") &&
            Contains(Expired, "no longer accessible"),
        "an invalidated lifetime refuses the receiver of every member");
  Check(ReadCalls == 2, "an expired receiver never reached native code");
}

void CheckMemberFailuresAndRefusals() {
  {
    // Two candidates of one member name that no call could tell apart.
    ResetCounters();
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Vector3> Class =
        Registry.RegisterClass<Vector3>("Vector3", Vector3Key());
    Luna::ClassBuilder<Vector3> &First =
        Class.Method("Largest", &Vector3::Largest);
    Luna::ClassBuilder<Vector3> &Second = First.Method("Largest", &SumOf);
    const Luna::RegistrationResult Result = Second.Commit();
    Check(!Result.IsSuccess(),
          "two indistinguishable member signatures are refused");
    Check(Result.Diagnostic() && Result.Diagnostic()->Category() ==
                                     Luna::ErrorCategory::DuplicateGlobalName,
          "an indistinguishable member candidate is a deterministic duplicate");
    Check(Hooks::RegisteredClassCount(Owner) == 0,
          "a refused member candidate publishes no class at all");
    Check(Registry.Reflection().IsEmpty(),
          "a refused member candidate contributes no reflection record");
  }

  {
    // One member name is either an instance member or a static one.
    ResetCounters();
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Vector3> Class =
        Registry.RegisterClass<Vector3>("Vector3", Vector3Key());
    Luna::ClassBuilder<Vector3> &Instance =
        Class.Method("Largest", &Vector3::Largest);
    Luna::ClassBuilder<Vector3> &Static =
        Instance.StaticMethod("Largest", &Vector3::Dimensions);
    const Luna::RegistrationResult Result = Static.Commit();
    Check(!Result.IsSuccess(),
          "one member name declared with and without a receiver is refused");
    Check(Result.Diagnostic() &&
              Contains(Result.Diagnostic()->Message(), "member category"),
          "the refusal names the member-category collision");
    Check(Hooks::RegisteredClassCount(Owner) == 0,
          "a refused member category publishes no class at all");
  }

  {
    // Documenting a member that was never declared stays a deterministic
    // failure, and a declared member may be documented.
    ResetCounters();
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Vector3> Class =
        Registry.RegisterClass<Vector3>("Vector3", Vector3Key());
    Luna::ClassBuilder<Vector3> &Documented =
        Class.Documentation("Largest", "nothing declares this yet.");
    Check(!Documented.Commit().IsSuccess(),
          "documenting an undeclared member is refused");
  }

  {
    // A throwing member is translated exactly as any other native target.
    ResetCounters();
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Check(RegisterVector3(Registry).IsSuccess(), "the class publishes");
    const std::string Message =
        Refusal(Owner, "local V = Vector3.New(1, 1, 1)\nV:Throwing()");
    Check(Contains(Message, "no growth today"),
          "a throwing member is translated with its context");
    Check(MutateCalls == 1, "the throwing member ran exactly once");
    Check(Owner.IsReady(), "the State remains usable after a throwing member");
  }
}

} // namespace

int RunClassMethodTests() {
  FailureCount = 0;
  CheckMembersAreReflectedAsMembers();
  CheckColonAndDotCallsAreOneCall();
  CheckOrdinaryArgumentsFollowTheReceiver();
  CheckMissingReceiverFailsBeforeArguments();
  CheckStaticMethodsUseTheOrdinaryPipeline();
  CheckConstReceiversRejectMutatingMembers();
  CheckMemberFailuresAndRefusals();
  return FailureCount == 0 ? 0 : 1;
}
