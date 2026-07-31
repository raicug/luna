// clang-format off
#include <luna/luna.hpp>
#include "state/testing/test_hooks.hpp"
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "instance member check failed: " << Description << '\n';
}

struct Vector final {
  double X = 0.0;
  double Y = 0.0;

  [[nodiscard]] double Magnitude() const { return std::sqrt(X * X + Y * Y); }

  [[nodiscard]] Vector Unit() const {
    const double Length = Magnitude();
    return Length == 0.0 ? Vector{0.0, 0.0} : Vector{X / Length, Y / Length};
  }
};

struct Http final {
  int Requests = 0;

  [[nodiscard]] std::string Encode(std::string Text) const {
    return "{\"body\":\"" + Text + "\"}";
  }
};

struct Body final {
  Vector Position;
  Http Client;
  Http *Attached = nullptr;

  [[nodiscard]] Vector Heading() const { return Vector{0.0, 1.0}; }

  [[nodiscard]] std::shared_ptr<Vector> Anchor() const {
    return std::make_shared<Vector>(Vector{6.0, 8.0});
  }

  [[nodiscard]] Http *Service() { return &Client; }

  void Move(Vector Target) { Position = Target; }

  [[nodiscard]] Vector Where() const { return Position; }

  void Place(const Vector &Target) { Position = Target; }

  void Consume(Vector &Target) {
    Position = Target;
    Target.X = -1.0;
  }

  [[nodiscard]] Http *Link() const { return Attached; }

  void Attach(Http *Incoming) { Attached = Incoming; }

  [[nodiscard]] int LinkedRequests() const {
    return Attached != nullptr ? Attached->Requests : -1;
  }
};

struct Machine final {
  Http Front;

  [[nodiscard]] Http *Leading() { return &Front; }
};

} // namespace

template <> struct Luna::RegisteredClassTrait<Vector> : std::true_type {};
template <> struct Luna::RegisteredClassTrait<Http> : std::true_type {};
template <> struct Luna::RegisteredClassTrait<Body> : std::true_type {};
template <> struct Luna::RegisteredClassTrait<Machine> : std::true_type {};

namespace {

[[nodiscard]] Luna::StableTypeKey VectorKey() {
  return Luna::StableTypeKey("Studio.MemberVector");
}

[[nodiscard]] Luna::StableTypeKey HttpKey() {
  return Luna::StableTypeKey("Studio.MemberHttp");
}

[[nodiscard]] Luna::StableTypeKey BodyKey() {
  return Luna::StableTypeKey("Studio.MemberBody");
}

[[nodiscard]] Luna::StableTypeKey MachineKey() {
  return Luna::StableTypeKey("Studio.MemberMachine");
}

[[nodiscard]] bool RegisterModel(Luna::State &Owner,
                                 const Luna::LifetimeHandle &Host) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

  Luna::ClassBuilder<Vector> Vectors =
      Studio.RegisterClass<Vector>("Vector", VectorKey());
  Luna::ClassBuilder<Vector> &DeclaredVector =
      Vectors.Constructor<>()
          .Field("X", &Vector::X)
          .Field("Y", &Vector::Y)
          .Property("Magnitude", &Vector::Magnitude)
          .Property("Unit", &Vector::Unit);
  static_cast<void>(DeclaredVector.QualifiedName());

  Luna::ClassBuilder<Http> Services =
      Studio.RegisterClass<Http>("Http", HttpKey());
  Luna::ClassBuilder<Http> &DeclaredHttp =
      Services.Constructor<>()
          .Field("Requests", &Http::Requests)
          .Method("Encode", &Http::Encode);
  static_cast<void>(DeclaredHttp.QualifiedName());

  Luna::ClassBuilder<Body> Bodies =
      Studio.RegisterClass<Body>("Body", BodyKey());
  Luna::ClassBuilder<Body> &DeclaredBody =
      Bodies.Constructor<>()
          .Field("Position", &Body::Position)
          .Property("Heading", &Body::Heading)
          .Property("Anchor", &Body::Anchor)
          .Property("Service", &Body::Service,
                    Luna::OwnershipPolicy::Borrowed(Host))
          .Property("Where", &Body::Where, &Body::Place)
          .Property("Taken", &Body::Where, &Body::Consume)
          .Property("Link", &Body::Link, &Body::Attach,
                    Luna::OwnershipPolicy::Borrowed(Host))
          .Method("Move", &Body::Move)
          .Method("LinkedRequests", &Body::LinkedRequests);
  static_cast<void>(DeclaredBody.QualifiedName());

  const Luna::RegistrationResult Committed = Studio.Commit();
  if (!Committed.IsSuccess()) {
    if (const Luna::ErrorDiagnostic *Diagnostic = Committed.Diagnostic())
      std::cerr << "instance member model refused: " << Diagnostic->Message()
                << '\n';
  }
  return Committed.IsSuccess();
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "instance member source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

void CheckPropertyPublishesAnOwnedInstance() {
  Luna::LifetimeHandle Host;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Host), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "local V = Studio.Vector.New()\n"
                        "V.X = 3\nV.Y = 4\n"
                        "local U = V.Unit\n"
                        "assert(type(U) == 'userdata')\n"
                        "assert(U.X == 0.6 and U.Y == 0.8)\n"
                        "assert(V.Magnitude == 5)"),
        "a property of the declaring class's own type publishes one Lua-owned "
        "instance per read");

  Check(Succeeds(Owner, "local V = Studio.Vector.New()\n"
                        "V.X = 3\nV.Y = 4\n"
                        "local First, Second = V.Unit, V.Unit\n"
                        "First.X = 0\n"
                        "assert(Second.X == 0.6)\n"
                        "assert(V.X == 3)"),
        "each read publishes its own object, so writing one copy leaves the "
        "receiver and the other copy alone");

  Check(Succeeds(Owner, "local B = Studio.Body.New()\n"
                        "local H = B.Heading\n"
                        "assert(H.Y == 1)"),
        "a property publishes an instance of another registered class");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "an instance-member read restores the entry stack depth");
}

void CheckSharedAndBorrowedInstanceProperties() {
  Luna::LifetimeHandle Host;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Host), "the model publishes");

  Check(Succeeds(Owner, "local B = Studio.Body.New()\n"
                        "local A = B.Anchor\n"
                        "assert(A.X == 6 and A.Y == 8)"),
        "a property returning std::shared_ptr publishes a shared instance");

  Check(Succeeds(Owner, "local B = Studio.Body.New()\n"
                        "local S = B.Service\n"
                        "S.Requests = 4\n"
                        "assert(S:Encode('hi') == '{\"body\":\"hi\"}')\n"
                        "assert(B.Service.Requests == 4)"),
        "a borrowed pointer property publishes the owner's own object, so a "
        "write through it is observed by the next read");
}

void CheckInstanceFieldPublishesACopy() {
  Luna::LifetimeHandle Host;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Host), "the model publishes");

  Check(Succeeds(Owner, "local B = Studio.Body.New()\n"
                        "local T = Studio.Vector.New()\n"
                        "T.X = 2\nT.Y = 7\n"
                        "B:Move(T)\n"
                        "assert(B.Position.X == 2 and B.Position.Y == 7)"),
        "a data member of a registered class type reads as one instance copy");

  Check(Succeeds(Owner, "local B = Studio.Body.New()\n"
                        "local P = B.Position\n"
                        "P.X = 11\n"
                        "assert(B.Position.X == 0)"),
        "the published copy is not a window into the owner's storage");

  const Luna::ExecutionResult Written =
      Owner.Execute("local B = Studio.Body.New()\n"
                    "B.Position = Studio.Vector.New()");
  Check(!Written.IsSuccess(),
        "assigning an instance-valued field is refused deterministically");
}

void CheckBorrowedInstanceMemberWithoutLifetimeIsRefused() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

  Luna::ClassBuilder<Http> Services =
      Studio.RegisterClass<Http>("Http", HttpKey());
  static_cast<void>(Services.QualifiedName());

  Luna::ClassBuilder<Body> Bodies =
      Studio.RegisterClass<Body>("Body", BodyKey());
  Luna::ClassBuilder<Body> &Declared =
      Bodies.Property("Service", &Body::Service);
  static_cast<void>(Declared.QualifiedName());

  const Luna::RegistrationResult Committed = Studio.Commit();
  Check(!Committed.IsSuccess(),
        "a pointer-valued property without a declared lifetime is refused at "
        "registration");
}

void CheckWritableInstancePropertyAcceptsAnInstanceOperand() {
  Luna::LifetimeHandle Host;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Host), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "local B = Studio.Body.New()\n"
                        "local V = Studio.Vector.New()\n"
                        "V.X = 2\nV.Y = 7\n"
                        "B.Where = V\n"
                        "assert(B.Where.X == 2 and B.Where.Y == 7)\n"
                        "assert(B.Position.X == 2)"),
        "a read-write instance property accepts an instance of its class and "
        "the setter reaches the receiver's own storage");

  Check(Succeeds(Owner, "local B = Studio.Body.New()\n"
                        "local V = Studio.Vector.New()\n"
                        "V.X = 5\nV.Y = 5\n"
                        "B.Taken = V\n"
                        "assert(B.Taken.X == 5)\n"
                        "assert(V.X == -1)"),
        "a setter taking the class by mutable reference receives the caller's "
        "own native object rather than a copy of it");

  Check(Succeeds(Owner, "local B = Studio.Body.New()\n"
                        "local H = Studio.Http.New()\n"
                        "H.Requests = 9\n"
                        "B.Link = H\n"
                        "assert(B:LinkedRequests() == 9)\n"
                        "H.Requests = 12\n"
                        "assert(B:LinkedRequests() == 12)"),
        "a setter taking the class by pointer stores the caller's object, so a "
        "later write through the original is observed natively");

  const Luna::ExecutionResult Reread =
      Owner.Execute("local B = Studio.Body.New()\n"
                    "local H = Studio.Http.New()\n"
                    "B.Link = H\n"
                    "local Back = B.Link");
  Check(!Reread.IsSuccess(),
        "reading a borrowed instance property back after storing a Lua-owned "
        "instance in it is refused, because one native address cannot be "
        "exposed under two ownership models");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "an instance-member write restores the entry stack depth");
}

void CheckWritableInstancePropertyRefusesOtherValues() {
  Luna::LifetimeHandle Host;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Host), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  const Luna::ExecutionResult OtherClass =
      Owner.Execute("local B = Studio.Body.New()\n"
                    "B.Where = B.Service");
  Check(!OtherClass.IsSuccess(),
        "assigning an instance of another registered class is refused");

  const Luna::ExecutionResult Scalar =
      Owner.Execute("local B = Studio.Body.New()\n"
                    "B.Where = 4");
  Check(!Scalar.IsSuccess(),
        "assigning a scalar to an instance property is refused");

  const Luna::ExecutionResult ReadOnly =
      Owner.Execute("local B = Studio.Body.New()\n"
                    "B.Heading = Studio.Vector.New()");
  Check(!ReadOnly.IsSuccess(),
        "a property declaring no setter stays read-only, and the class's "
        "Assign operator is never consulted for a declared member");

  Check(Succeeds(Owner, "local B = Studio.Body.New()\n"
                        "B.Where = Studio.Vector.New()\n"
                        "assert(B.Where.X == 0)"),
        "a refused write leaves the receiver able to accept the next one");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "a refused instance-member write restores the entry stack depth");
}

void CheckInstancePropertyChangeHandlerIsRefused() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

  Luna::ClassBuilder<Vector> Vectors =
      Studio.RegisterClass<Vector>("Vector", VectorKey());
  static_cast<void>(Vectors.Constructor<>().QualifiedName());

  Luna::ClassBuilder<Body> Bodies =
      Studio.RegisterClass<Body>("Body", BodyKey());
  Luna::ClassBuilder<Body> &Declared = Bodies.Property(
      "Where", &Body::Where, &Body::Place, [](Body &, const Vector &) {});
  static_cast<void>(Declared.QualifiedName());

  const Luna::RegistrationResult Committed = Studio.Commit();
  Check(!Committed.IsSuccess(),
        "an on-change handler on an instance-valued property is refused at "
        "registration");
  if (const Luna::ErrorDiagnostic *Diagnostic = Committed.Diagnostic())
    Check(Diagnostic->Message().find("on-change") != std::string::npos,
          "the refusal names the on-change handler");
}

void CheckUnregisteredValueClassIsRefused() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

  Luna::ClassBuilder<Body> Bodies =
      Studio.RegisterClass<Body>("Body", BodyKey());
  Luna::ClassBuilder<Body> &Declared =
      Bodies.Property("Heading", &Body::Heading);
  static_cast<void>(Declared.QualifiedName());

  const Luna::RegistrationResult Committed = Studio.Commit();
  Check(!Committed.IsSuccess(),
        "a member publishing an instance of a class this plan never registers "
        "is refused at registration");
}

void CheckSharedAddressWithAnotherClassIsRefusedAtRead() {
  Luna::LifetimeHandle Host;
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

  Luna::ClassBuilder<Http> Services =
      Studio.RegisterClass<Http>("Http", HttpKey());
  static_cast<void>(
      Services.Field("Requests", &Http::Requests).QualifiedName());

  Luna::ClassBuilder<Machine> Machines =
      Studio.RegisterClass<Machine>("Machine", MachineKey());
  Luna::ClassBuilder<Machine> &Declared = Machines.Constructor<>().Property(
      "Leading", &Machine::Leading, Luna::OwnershipPolicy::Borrowed(Host));
  static_cast<void>(Declared.QualifiedName());

  const Luna::RegistrationResult Committed = Studio.Commit();
  Check(Committed.IsSuccess(),
        "a borrowed property pointing at a member at offset zero registers");

  const Luna::ExecutionResult Read =
      Owner.Execute("local M = Studio.Machine.New()\n"
                    "local F = M.Leading");
  Check(!Read.IsSuccess(),
        "publishing a second class at one native address is refused "
        "deterministically rather than aliasing the owner's identity");
}

} // namespace

int RunInstanceMemberTests();

int RunInstanceMemberTests() {
  FailureCount = 0;
  CheckPropertyPublishesAnOwnedInstance();
  CheckSharedAndBorrowedInstanceProperties();
  CheckInstanceFieldPublishesACopy();
  CheckBorrowedInstanceMemberWithoutLifetimeIsRefused();
  CheckWritableInstancePropertyAcceptsAnInstanceOperand();
  CheckWritableInstancePropertyRefusesOtherValues();
  CheckInstancePropertyChangeHandlerIsRefused();
  CheckUnregisteredValueClassIsRefused();
  CheckSharedAddressWithAnotherClassIsRefusedAtRead();
  return FailureCount == 0 ? 0 : 1;
}
