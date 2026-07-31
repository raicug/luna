// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <iostream>
#include <memory>
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
  std::cerr << "delegate object check failed: " << Description << '\n';
}

struct Vector final {
  double X = 0.0;
  double Y = 0.0;

  [[nodiscard]] double Sum() const { return X + Y; }
};

struct Ghost final {
  int Unused = 0;
};

} // namespace

template <> struct Luna::RegisteredClassTrait<Vector> : std::true_type {};
template <> struct Luna::RegisteredClassTrait<Ghost> : std::true_type {};

namespace {

struct Hub final {
  Luna::Signal<void(Vector)> Moved;
  Luna::Delegate<void(Vector *)> Borrowed;
  Luna::Delegate<void(std::shared_ptr<Vector>)> Shared;
  Luna::Delegate<void(Luna::OwnedValue)> Described;
  Luna::Delegate<void(Luna::ValuePack)> Spread;
  Luna::Delegate<bool(Vector)> Filter;
  Luna::Delegate<void(Ghost)> Unregistered;

  Vector Anchor{11.0, 12.0};
  Luna::LifetimeHandle Anchorage;
};

Hub *Active = nullptr;

[[nodiscard]] std::string Report(const Luna::DelegateCallResult &Result) {
  if (Result.IsSuccess())
    return "delivered";
  return std::string(Luna::DelegateStatusText(Result.Status()));
}

[[nodiscard]] int SubscribeMoved(Luna::Delegate<void(Vector)> Handler) {
  return Active ? Active->Moved.Subscribe(std::move(Handler)) : 0;
}

[[nodiscard]] int EmitMoved(double X, double Y) {
  if (!Active)
    return -1;
  const Luna::SignalEmission Reported = Active->Moved.Emit(Vector{X, Y});
  return static_cast<int>(Reported.Delivered);
}

void SubscribeBorrowed(Luna::Delegate<void(Vector *)> Handler) {
  if (!Active)
    return;
  Handler.DeclareOwnership(Luna::OwnershipPolicy::Borrowed(Active->Anchorage));
  Active->Borrowed = std::move(Handler);
}

[[nodiscard]] std::string EmitBorrowed() {
  return Active ? Report(Active->Borrowed.Invoke(&Active->Anchor))
                : std::string("released");
}

void SubscribeLifetimeless(Luna::Delegate<void(Vector *)> Handler) {
  if (Active)
    Active->Borrowed = std::move(Handler);
}

void SubscribeShared(Luna::Delegate<void(std::shared_ptr<Vector>)> Handler) {
  if (Active)
    Active->Shared = std::move(Handler);
}

[[nodiscard]] std::string EmitShared(bool WithObject) {
  if (!Active)
    return "released";
  std::shared_ptr<Vector> Held =
      WithObject ? std::make_shared<Vector>(Vector{6.0, 8.0})
                 : std::shared_ptr<Vector>();
  return Report(Active->Shared.Invoke(std::move(Held)));
}

void SubscribeDescribed(Luna::Delegate<void(Luna::OwnedValue)> Handler) {
  if (Active)
    Active->Described = std::move(Handler);
}

[[nodiscard]] std::string EmitDescribed() {
  if (!Active)
    return "released";

  Luna::OwnedValue Event = Luna::OwnedValue::Table();
  Event.SetField("name", Luna::OwnedValue::Text("move"));
  Event.SetField("origin",
                 Luna::OwnedValue::Instance<Vector>(Vector{1.0, 2.0}));

  Luna::OwnedValue Nested = Luna::OwnedValue::Table();
  Nested.Append(Luna::OwnedValue::Instance<Vector>(Vector{3.0, 4.0}));
  Nested.Append(Luna::OwnedValue::Number(9.0));
  Event.SetField("path", std::move(Nested));
  return Report(Active->Described.Invoke(std::move(Event)));
}

void SubscribeSpread(Luna::Delegate<void(Luna::ValuePack)> Handler) {
  if (Active)
    Active->Spread = std::move(Handler);
}

[[nodiscard]] std::string EmitSpread() {
  if (!Active)
    return "released";

  Luna::ValuePack Carried;
  Carried.Append(Luna::OwnedValue::Text("began"));
  Carried.Append(Luna::OwnedValue::Instance<Vector>(Vector{5.0, 5.0}));
  Carried.Append(Luna::OwnedValue::Number(2.0));
  return Report(Active->Spread.Invoke(std::move(Carried)));
}

void SubscribeFilter(Luna::Delegate<bool(Vector)> Handler) {
  if (Active)
    Active->Filter = std::move(Handler);
}

[[nodiscard]] bool EmitFilter(double X, double Y) {
  if (!Active)
    return false;
  const Luna::DelegateCallResult Result = Active->Filter.Invoke(Vector{X, Y});
  const Luna::Value *Produced = Result.Produced();
  return Result.IsSuccess() && Produced != nullptr &&
         std::holds_alternative<bool>(*Produced) && std::get<bool>(*Produced);
}

[[nodiscard]] std::string ReleaseAndEmitMoved() {
  if (!Active)
    return "released";
  Luna::Delegate<void(Vector)> Held;
  Active->Moved.Clear();
  return Report(Held.Invoke(Vector{1.0, 1.0}));
}

[[nodiscard]] Luna::StableTypeKey VectorKey() {
  return Luna::StableTypeKey("Studio.DelegateVector");
}

[[nodiscard]] bool RegisterModel(Luna::State &Owner) {
  {
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    Luna::ClassBuilder<Vector> Vectors =
        Studio.RegisterClass<Vector>("Vector", VectorKey());
    Luna::ClassBuilder<Vector> &Declared = Vectors.Constructor<>()
                                               .Field("X", &Vector::X)
                                               .Field("Y", &Vector::Y)
                                               .Method("Sum", &Vector::Sum);
    static_cast<void>(Declared.QualifiedName());

    const Luna::RegistrationResult Committed = Studio.Commit();
    if (!Committed.IsSuccess()) {
      if (const Luna::ErrorDiagnostic *Diagnostic = Committed.Diagnostic())
        std::cerr << "delegate object class refused: " << Diagnostic->Message()
                  << '\n';
      return false;
    }
  }

  Luna::BindingRegistry Registry = Owner.Bindings();
  bool Published = true;
  const auto Publish = [&Published](std::string_view Name,
                                    const Luna::RegistrationResult &Result) {
    if (Result.IsSuccess())
      return;
    Published = false;
    if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
      std::cerr << "delegate object function '" << Name
                << "' refused: " << Diagnostic->Message() << '\n';
  };

  Publish("SubscribeMoved",
          Registry.RegisterFunction("SubscribeMoved", &SubscribeMoved));
  Publish("EmitMoved", Registry.RegisterFunction("EmitMoved", &EmitMoved));
  Publish("SubscribeBorrowed",
          Registry.RegisterFunction("SubscribeBorrowed", &SubscribeBorrowed));
  Publish("SubscribeLifetimeless",
          Registry.RegisterFunction("SubscribeLifetimeless",
                                    &SubscribeLifetimeless));
  Publish("EmitBorrowed",
          Registry.RegisterFunction("EmitBorrowed", &EmitBorrowed));
  Publish("SubscribeShared",
          Registry.RegisterFunction("SubscribeShared", &SubscribeShared));
  Publish("EmitShared", Registry.RegisterFunction("EmitShared", &EmitShared));
  Publish("SubscribeDescribed",
          Registry.RegisterFunction("SubscribeDescribed", &SubscribeDescribed));
  Publish("EmitDescribed",
          Registry.RegisterFunction("EmitDescribed", &EmitDescribed));
  Publish("SubscribeSpread",
          Registry.RegisterFunction("SubscribeSpread", &SubscribeSpread));
  Publish("EmitSpread", Registry.RegisterFunction("EmitSpread", &EmitSpread));
  Publish("SubscribeFilter",
          Registry.RegisterFunction("SubscribeFilter", &SubscribeFilter));
  Publish("EmitFilter", Registry.RegisterFunction("EmitFilter", &EmitFilter));
  Publish(
      "ReleaseAndEmitMoved",
      Registry.RegisterFunction("ReleaseAndEmitMoved", &ReleaseAndEmitMoved));
  return Published;
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "delegate object source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

void CheckHandlerReceivesAnInstance() {
  Luna::State Owner;
  Hub Local;
  Active = &Local;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "local Seen = nil\n"
                        "SubscribeMoved(function(Where)\n"
                        "  assert(type(Where) == 'userdata')\n"
                        "  Seen = Where.X + Where.Y + Where:Sum()\n"
                        "end)\n"
                        "assert(EmitMoved(2, 3) == 1)\n"
                        "assert(Seen == 10)"),
        "a delegate parameter declared as a registered class delivers real "
        "userdata the handler can read and call methods on");

  Check(Succeeds(Owner,
                 "local Sum = nil\n"
                 "SubscribeShared(function(Where) Sum = Where:Sum() end)\n"
                 "assert(EmitShared(true) == 'delivered')\n"
                 "assert(Sum == 14)"),
        "a shared instance parameter delivers the shared object");

  Check(Succeeds(Owner,
                 "local Seen = nil\n"
                 "SubscribeBorrowed(function(Where) Seen = Where:Sum() end)\n"
                 "assert(EmitBorrowed() == 'delivered')\n"
                 "assert(Seen == 23)"),
        "a borrowed instance parameter with a declared lifetime delivers the "
        "host's own object");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every delegate call restores the entry stack depth");
  Active = nullptr;
}

void CheckHandlerReceivesTablesAndPacks() {
  Luna::State Owner;
  Hub Local;
  Active = &Local;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "local Name, Origin, Deep, Tail = nil, nil, nil, nil\n"
                        "SubscribeDescribed(function(Event)\n"
                        "  assert(type(Event) == 'table')\n"
                        "  Name = Event.name\n"
                        "  Origin = Event.origin:Sum()\n"
                        "  Deep = Event.path[1]:Sum()\n"
                        "  Tail = Event.path[2]\n"
                        "end)\n"
                        "assert(EmitDescribed() == 'delivered')\n"
                        "assert(Name == 'move')\n"
                        "assert(Origin == 3)\n"
                        "assert(Deep == 7)\n"
                        "assert(Tail == 9)"),
        "an OwnedValue parameter delivers a host-decided table whose fields "
        "and nested elements carry manufactured instances");

  Check(Succeeds(Owner, "local A, B, C, Count = nil, nil, nil, nil\n"
                        "SubscribeSpread(function(...)\n"
                        "  Count = select('#', ...)\n"
                        "  A, B, C = ...\n"
                        "end)\n"
                        "assert(EmitSpread() == 'delivered')\n"
                        "assert(Count == 3)\n"
                        "assert(A == 'began')\n"
                        "assert(type(B) == 'userdata' and B:Sum() == 10)\n"
                        "assert(C == 2)"),
        "a trailing ValuePack parameter spreads its values at that position, "
        "objects included");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "a table-carrying delegate call restores the entry stack depth");
  Active = nullptr;
}

void CheckScalarResultSurvivesObjectParameters() {
  Luna::State Owner;
  Hub Local;
  Active = &Local;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");

  Check(Succeeds(Owner, "SubscribeFilter(function(Where)\n"
                        "  return Where:Sum() > 5\n"
                        "end)\n"
                        "assert(EmitFilter(4, 4) == true)\n"
                        "assert(EmitFilter(1, 1) == false)"),
        "a delegate may still declare a scalar result while its parameters "
        "carry objects");
  Active = nullptr;
}

void CheckMalformedObjectArgumentsPublishNothing() {
  Luna::State Owner;
  Hub Local;
  Active = &Local;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "local Calls = 0\n"
                        "SubscribeShared(function() Calls = Calls + 1 end)\n"
                        "assert(EmitShared(false) == 'handler_failed')\n"
                        "assert(Calls == 0)"),
        "a null instance argument refuses the call and never reaches the "
        "handler");

  Check(Succeeds(Owner,
                 "local Calls = 0\n"
                 "SubscribeLifetimeless(function() Calls = Calls + 1 end)\n"
                 "assert(EmitBorrowed() == 'handler_failed')\n"
                 "assert(Calls == 0)"),
        "a borrowed instance argument with no declared lifetime refuses the "
        "call and never reaches the handler");

  Check(Succeeds(Owner, "assert(ReleaseAndEmitMoved() == 'released')"),
        "a released handler still reports the released status");

  Check(Succeeds(Owner,
                 "local Sum = nil\n"
                 "SubscribeShared(function(Where) Sum = Where:Sum() end)\n"
                 "assert(EmitShared(true) == 'delivered')\n"
                 "assert(Sum == 14)"),
        "a refused call leaves the next one able to deliver");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "a refused delegate call restores the entry stack depth");
  Active = nullptr;
}

void CheckUnregisteredDelegateClassIsRefused() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  Luna::BindingRegistry Registry = Owner.Bindings();
  const Luna::RegistrationResult Refused = Registry.RegisterFunction(
      "SubscribeGhost",
      +[](Luna::Delegate<void(Ghost)> Handler) { static_cast<void>(Handler); });
  Check(!Refused.IsSuccess(),
        "a delegate whose instance parameter names a class this State never "
        "registered is refused at registration");
}

} // namespace

int RunDelegateObjectTests();

int RunDelegateObjectTests() {
  FailureCount = 0;
  CheckHandlerReceivesAnInstance();
  CheckHandlerReceivesTablesAndPacks();
  CheckScalarResultSurvivesObjectParameters();
  CheckMalformedObjectArgumentsPublishNothing();
  CheckUnregisteredDelegateClassIsRefused();
  return FailureCount == 0 ? 0 : 1;
}
