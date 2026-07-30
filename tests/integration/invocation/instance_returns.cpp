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

struct Game final {
  int Cookie = 0;
  Http Service;

  [[nodiscard]] Http *GetService(std::string Name) {
    return Name == "HttpService" ? &Service : nullptr;
  }

  [[nodiscard]] std::shared_ptr<Vector> MakeShared() const {
    return std::make_shared<Vector>(Vector{3.0, 4.0});
  }
};

} // namespace

template <> struct Luna::RegisteredClassTrait<Http> : std::true_type {};
template <> struct Luna::RegisteredClassTrait<Vector> : std::true_type {};
template <> struct Luna::RegisteredClassTrait<Game> : std::true_type {};

namespace {

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
          .Method("MakeShared", &Game::MakeShared);
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

} // namespace

int RunInstanceReturnTests();

int RunInstanceReturnTests() {
  FailureCount = 0;
  CheckMethodReturnsBorrowedInstance();
  CheckMethodReturnsOwnedAndSharedInstances();
  CheckMethodReturnsTableAndOwnedPack();
  CheckBorrowedReturnWithoutLifetimeIsRefused();
  return FailureCount == 0 ? 0 : 1;
}
