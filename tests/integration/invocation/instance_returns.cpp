// clang-format off
#include <luna/luna.hpp>
#include "state/testing/test_hooks.hpp"
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
  std::cerr << "instance return check failed: " << Description << '\n';
}

struct Http final {
  int Requests = 0;

  [[nodiscard]] std::string Encode(std::string Text) const {
    return "{\"body\":\"" + Text + "\"}";
  }

  [[nodiscard]] Luna::OwnedValue Decode(std::string Text) const {
    Luna::OwnedValue Decoded = Luna::OwnedValue::Table();
    Decoded.SetField("enabled", Luna::OwnedValue::Boolean(true));
    Decoded.SetField(
        "length", Luna::OwnedValue::Number(static_cast<double>(Text.size())));

    Luna::OwnedValue Names = Luna::OwnedValue::Table();
    Names.Append(Luna::OwnedValue::Text("alpha"));
    Names.Append(Luna::OwnedValue::Text("beta"));
    Decoded.SetField("names", std::move(Names));
    return Decoded;
  }

  [[nodiscard]] Luna::ValuePack Describe() const {
    Luna::ValuePack Produced;
    Produced.Append(Luna::OwnedValue::Text("http"));
    Luna::OwnedValue Detail = Luna::OwnedValue::Table();
    Detail.SetField("requests",
                    Luna::OwnedValue::Number(static_cast<double>(Requests)));
    Produced.Append(std::move(Detail));
    return Produced;
  }
};

struct Vector final {
  double X = 0.0;
  double Y = 0.0;

  [[nodiscard]] Vector Scaled(double Factor) const {
    return Vector{X * Factor, Y * Factor};
  }

  [[nodiscard]] Vector Doubled() const { return Vector{X * 2.0, Y * 2.0}; }
};

struct Ghost final {
  int Unused = 0;
};

struct Game final {
  int Cookie = 0;
  Http Service;
  Vector Anchor{11.0, 12.0};

  [[nodiscard]] Http *GetService(std::string Name) {
    return Name == "HttpService" ? &Service : nullptr;
  }

  [[nodiscard]] std::shared_ptr<Vector> MakeShared() const {
    return std::make_shared<Vector>(Vector{3.0, 4.0});
  }

  [[nodiscard]] Luna::OwnedValue GetChildren() const;
  [[nodiscard]] Luna::ValuePack ChildPack() const;
  [[nodiscard]] Luna::ReturnPack ChildList() const;
  [[nodiscard]] Luna::OwnedValue UnregisteredChild() const;
  [[nodiscard]] Luna::OwnedValue LifetimelessChild();
  [[nodiscard]] Luna::OwnedValue NullChild() const;
};

} // namespace

template <> struct Luna::RegisteredClassTrait<Http> : std::true_type {};
template <> struct Luna::RegisteredClassTrait<Vector> : std::true_type {};
template <> struct Luna::RegisteredClassTrait<Ghost> : std::true_type {};
template <> struct Luna::RegisteredClassTrait<Game> : std::true_type {};

namespace {

Luna::OwnedValue Game::GetChildren() const {
  Luna::OwnedValue Children = Luna::OwnedValue::Table();
  Children.Append(Luna::OwnedValue::Instance<Vector>(Vector{1.0, 2.0}));
  Children.Append(
      Luna::OwnedValue::Instance<Vector>(std::make_shared<Vector>(Anchor)));

  Luna::OwnedValue Nested = Luna::OwnedValue::Table();
  Nested.Append(Luna::OwnedValue::Instance<Vector>(Vector{5.0, 6.0}));
  Children.SetField("nested", std::move(Nested));
  Children.SetField("count", Luna::OwnedValue::Number(2.0));
  return Children;
}

Luna::ValuePack Game::ChildPack() const {
  Luna::ValuePack Produced;
  Produced.Append(Luna::OwnedValue::Instance<Vector>(Vector{7.0, 8.0}));
  Produced.Append(Luna::OwnedValue::Text("tail"));
  return Produced;
}

Luna::ReturnPack Game::ChildList() const {
  Luna::ReturnPack Produced;
  Produced.AppendText("head");
  Produced.AppendInstance<Vector>(Vector{9.0, 10.0});
  return Produced;
}

Luna::OwnedValue Game::UnregisteredChild() const {
  Luna::OwnedValue Children = Luna::OwnedValue::Table();
  Children.Append(Luna::OwnedValue::Instance<Vector>(Vector{1.0, 1.0}));
  Children.Append(Luna::OwnedValue::Instance<Ghost>(Ghost{}));
  return Children;
}

Luna::OwnedValue Game::LifetimelessChild() {
  Luna::OwnedValue Children = Luna::OwnedValue::Table();
  Children.Append(Luna::OwnedValue::Instance<Vector>(
      &Anchor, Luna::OwnershipPolicy::LuaOwned()));
  return Children;
}

Luna::OwnedValue Game::NullChild() const {
  Luna::OwnedValue Children = Luna::OwnedValue::Table();
  Children.Append(
      Luna::OwnedValue::Instance<Vector>(std::shared_ptr<Vector>()));
  return Children;
}

[[nodiscard]] Game *HostGame() {
  static Game Only;
  return &Only;
}

[[nodiscard]] bool RegisterModel(Luna::State &Owner,
                                 const Luna::LifetimeHandle &Host) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

  Luna::ClassBuilder<Http> Services = Studio.RegisterClass<Http>(
      "Http", Luna::StableTypeKey("Studio.ReturnHttp"));
  Luna::ClassBuilder<Http> &DeclaredHttp =
      Services.Field("Requests", &Http::Requests)
          .Method("Encode", &Http::Encode)
          .Method("Decode", &Http::Decode)
          .Method("Describe", &Http::Describe);
  static_cast<void>(DeclaredHttp.QualifiedName());

  Luna::ClassBuilder<Vector> Vectors = Studio.RegisterClass<Vector>(
      "Vector", Luna::StableTypeKey("Studio.ReturnVector"));
  Luna::ClassBuilder<Vector> &DeclaredVector =
      Vectors.Constructor<>()
          .Field("X", &Vector::X)
          .Field("Y", &Vector::Y)
          .Method("Scaled", &Vector::Scaled)
          .Operator(Luna::ClassOperator::Negate, &Vector::Doubled);
  static_cast<void>(DeclaredVector.QualifiedName());

  Luna::ClassBuilder<Game> Games = Studio.RegisterClass<Game>(
      "Game", Luna::StableTypeKey("Studio.ReturnGame"));
  Luna::ClassBuilder<Game> &DeclaredGame =
      Games.Singleton("Get", &HostGame, Luna::OwnershipPolicy::Borrowed(Host))
          .Method("GetService", &Game::GetService,
                  Luna::OwnershipPolicy::Borrowed(Host))
          .Method("MakeShared", &Game::MakeShared)
          .Method("GetChildren", &Game::GetChildren)
          .Method("ChildPack", &Game::ChildPack)
          .Method("ChildList", &Game::ChildList)
          .Method("UnregisteredChild", &Game::UnregisteredChild)
          .Method("LifetimelessChild", &Game::LifetimelessChild)
          .Method("NullChild", &Game::NullChild);
  static_cast<void>(DeclaredGame.QualifiedName());

  const Luna::RegistrationResult Committed = Studio.Commit();
  if (!Committed.IsSuccess()) {
    if (const Luna::ErrorDiagnostic *Diagnostic = Committed.Diagnostic())
      std::cerr << "instance return model refused: " << Diagnostic->Message()
                << '\n';
  }
  return Committed.IsSuccess();
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "instance return source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

void CheckMethodReturnsBorrowedInstance() {
  Luna::LifetimeHandle Host;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Host), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "game = Studio.Game.Get()\n"
                        "local http = game:GetService('HttpService')\n"
                        "assert(type(http) == 'userdata')\n"
                        "assert(http:Encode('hi') == '{\"body\":\"hi\"}')"),
        "a method returns an instance of another registered class, usable "
        "immediately");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "an instance-returning call restores the entry stack depth");
}

void CheckMethodReturnsOwnedAndSharedInstances() {
  Luna::LifetimeHandle Host;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Host), "the model publishes");

  Check(Succeeds(Owner, "local V = Studio.Vector.New()\n"
                        "V.X = 2\nV.Y = 3\n"
                        "local S = V:Scaled(2.5)\n"
                        "assert(S.X == 5 and S.Y == 7.5)"),
        "a method returns a new instance by value, owned by Lua afterwards");

  Check(Succeeds(Owner, "local V = Studio.Vector.New()\n"
                        "V.X = 1\n"
                        "local D = -V\n"
                        "assert(D.X == 2)"),
        "an operator returns a new instance");

  Check(Succeeds(Owner, "game = Studio.Game.Get()\n"
                        "local H = game:MakeShared()\n"
                        "assert(H.X == 3 and H.Y == 4)"),
        "a method returns a shared instance that keeps its own ownership");
}

void CheckMethodReturnsTableAndOwnedPack() {
  Luna::LifetimeHandle Host;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Host), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "game = Studio.Game.Get()\n"
                        "local http = game:GetService('HttpService')\n"
                        "local decoded = http:Decode('abcd')\n"
                        "assert(type(decoded) == 'table')\n"
                        "assert(decoded.enabled == true)\n"
                        "assert(decoded.length == 4)\n"
                        "assert(decoded.names[1] == 'alpha')\n"
                        "assert(decoded.names[2] == 'beta')"),
        "a method returns a table whose shape it decided at run time, with "
        "nested arrays reachable by index");

  Check(Succeeds(Owner, "game = Studio.Game.Get()\n"
                        "local http = game:GetService('HttpService')\n"
                        "http.Requests = 7\n"
                        "local Name, Detail = http:Describe()\n"
                        "assert(Name == 'http')\n"
                        "assert(Detail.requests == 7)"),
        "a method returns ordered multiple values where one of them is a "
        "table");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every owned-value return restores the entry stack depth");
}

void CheckBorrowedReturnWithoutLifetimeIsRefused() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Http> Services = Studio.RegisterClass<Http>(
      "Http", Luna::StableTypeKey("Studio.ReturnHttp"));
  static_cast<void>(Services.QualifiedName());

  Luna::ClassBuilder<Game> Games = Studio.RegisterClass<Game>(
      "Game", Luna::StableTypeKey("Studio.ReturnGame"));

  Luna::ClassBuilder<Game> &Declared =
      Games.Method("GetService", &Game::GetService);
  static_cast<void>(Declared.QualifiedName());

  const Luna::RegistrationResult Committed = Studio.Commit();
  Check(!Committed.IsSuccess(),
        "a pointer-returning method without a declared lifetime is refused at "
        "registration");
}

void CheckManufacturedInstancesTravelInsideOwnedValues() {
  Luna::LifetimeHandle Host;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Host), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "game = Studio.Game.Get()\n"
                        "local Children = game:GetChildren()\n"
                        "assert(type(Children) == 'table')\n"
                        "assert(Children.count == 2)\n"
                        "assert(#Children == 2)\n"
                        "assert(type(Children[1]) == 'userdata')\n"
                        "assert(Children[1].X == 1 and Children[1].Y == 2)\n"
                        "assert(Children[2].X == 11 and Children[2].Y == 12)\n"
                        "assert(Children[1]:Scaled(3).X == 3)\n"
                        "assert(Children.nested[1].X == 5)"),
        "a table returned by a method carries instances the call "
        "manufactured, each usable as a receiver, including inside a nested "
        "table");

  Check(Succeeds(Owner, "game = Studio.Game.Get()\n"
                        "local First, Second = game:ChildPack()\n"
                        "assert(type(First) == 'userdata')\n"
                        "assert(First.X == 7 and First.Y == 8)\n"
                        "assert(Second == 'tail')"),
        "a ValuePack element publishes a manufactured instance beside a "
        "scalar");

  Check(Succeeds(Owner, "game = Studio.Game.Get()\n"
                        "local Head, Part = game:ChildList()\n"
                        "assert(Head == 'head')\n"
                        "assert(type(Part) == 'userdata')\n"
                        "assert(Part.X == 9 and Part.Y == 10)"),
        "a ReturnPack keeps the scalars appended before the first instance and "
        "publishes both in order");

  Check(Succeeds(Owner, "game = Studio.Game.Get()\n"
                        "local A = game:GetChildren()[1]\n"
                        "local B = game:GetChildren()[1]\n"
                        "A.X = 40\n"
                        "assert(B.X == 1)"),
        "each manufactured instance owns its own copy");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "publishing manufactured instances restores the entry stack depth");
}

void CheckMalformedManufacturedInstancesPublishNothing() {
  Luna::LifetimeHandle Host;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Host), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "game = Studio.Game.Get()\n"
                        "local Ok, Message = pcall(function()\n"
                        "  return game:UnregisteredChild()\n"
                        "end)\n"
                        "assert(not Ok, 'an unregistered class refuses')\n"
                        "assert(type(Message) == 'string')"),
        "a table element naming a class this State never registered refuses "
        "the whole return");

  Check(Succeeds(Owner, "game = Studio.Game.Get()\n"
                        "local Ok = pcall(function()\n"
                        "  return game:LifetimelessChild()\n"
                        "end)\n"
                        "assert(not Ok)"),
        "a borrowed element with no declared lifetime refuses the whole "
        "return");

  Check(Succeeds(Owner, "game = Studio.Game.Get()\n"
                        "local Ok = pcall(function()\n"
                        "  return game:NullChild()\n"
                        "end)\n"
                        "assert(not Ok)"),
        "a null element refuses the whole return");

  Check(Succeeds(Owner, "game = Studio.Game.Get()\n"
                        "local Children = game:GetChildren()\n"
                        "assert(Children[1].X == 1)"),
        "a refused return leaves the State able to publish the next one");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "a refused manufactured instance restores the entry stack depth");
}

} // namespace

int RunInstanceReturnTests();

int RunInstanceReturnTests() {
  FailureCount = 0;
  CheckMethodReturnsBorrowedInstance();
  CheckMethodReturnsOwnedAndSharedInstances();
  CheckMethodReturnsTableAndOwnedPack();
  CheckBorrowedReturnWithoutLifetimeIsRefused();
  CheckManufacturedInstancesTravelInsideOwnedValues();
  CheckMalformedManufacturedInstancesPublishNothing();
  return FailureCount == 0 ? 0 : 1;
}
