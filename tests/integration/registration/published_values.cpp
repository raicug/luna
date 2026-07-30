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
  std::cerr << "published value check failed: " << Description << '\n';
}

struct Workspace final {
  int Parts = 0;

  [[nodiscard]] int Count() const { return Parts; }
};

struct Camera final {
  double Zoom = 1.0;
};

struct Game final {
  int Cookie = 0;
  Workspace Space;

  [[nodiscard]] Workspace *GetService(std::string Name) {
    return Name == "Workspace" ? &Space : nullptr;
  }

  [[nodiscard]] Camera Lens() const { return Camera{3.5}; }
};

} // namespace

template <> struct Luna::RegisteredClassTrait<Workspace> : std::true_type {};
template <> struct Luna::RegisteredClassTrait<Game> : std::true_type {};
template <> struct Luna::RegisteredClassTrait<Camera> : std::true_type {};

namespace {

[[nodiscard]] Game *HostGame() {
  static Game Only;
  return &Only;
}

[[nodiscard]] Camera OwnedCamera() {
  return Camera{2.5};
}

[[nodiscard]] std::shared_ptr<Camera> SharedCamera() {
  return std::make_shared<Camera>(Camera{4.0});
}

[[nodiscard]] bool RegisterModel(Luna::State &Owner,
                                 const Luna::LifetimeHandle &Host) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

  Luna::ClassBuilder<Workspace> Spaces = Studio.RegisterClass<Workspace>(
      "Workspace", Luna::StableTypeKey("Studio.ValueWorkspace"));
  static_cast<void>(Spaces.Field("Parts", &Workspace::Parts)
                        .Method("Count", &Workspace::Count));

  Luna::ClassBuilder<Camera> Cameras = Studio.RegisterClass<Camera>(
      "Camera", Luna::StableTypeKey("Studio.ValueCamera"));
  static_cast<void>(Cameras.Field("Zoom", &Camera::Zoom));

  Luna::ClassBuilder<Game> Games = Studio.RegisterClass<Game>(
      "Game", Luna::StableTypeKey("Studio.ValueGame"));
  static_cast<void>(Games
                        .Method("GetService", &Game::GetService,
                                Luna::OwnershipPolicy::Borrowed(Host))
                        .Property("Lens", &Game::Lens));

  const Luna::RegistrationResult Committed = Studio.Commit();
  if (!Committed.IsSuccess()) {
    if (const Luna::ErrorDiagnostic *Diagnostic = Committed.Diagnostic())
      std::cerr << "published value model refused: " << Diagnostic->Message()
                << '\n';
  }
  return Committed.IsSuccess();
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "published value source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

void CheckGlobalValueResolvesWithoutACall() {
  Luna::LifetimeHandle Host;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Host), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Luna::BindingRegistry Registry = Owner.Bindings();
  const Luna::RegistrationResult Published = Registry.RegisterValue(
      "game", &HostGame, Luna::OwnershipPolicy::Borrowed(Host));
  Check(Published.IsSuccess(),
        "a borrowed instance publishes at a global name");
  if (!Published.IsSuccess()) {
    if (const Luna::ErrorDiagnostic *Diagnostic = Published.Diagnostic())
      std::cerr << "published value refused: " << Diagnostic->Message() << '\n';
    return;
  }

  Check(Succeeds(Owner, "assert(type(game) == 'userdata')"),
        "the name resolves to userdata rather than to a function");
  Check(Succeeds(Owner, "local W = game:GetService('Workspace')\n"
                        "W.Parts = 3\n"
                        "assert(game:GetService('Workspace'):Count() == 3)"),
        "game:GetService('Workspace') reaches the host object without a call "
        "on the name itself");
  Check(Succeeds(Owner, "assert(type(game.Lens) == 'userdata')\n"
                        "assert(typeof(game.Lens) == 'Studio.Camera')\n"
                        "assert(game.Lens.Zoom == 3.5)"),
        "an instance-valued property of a published value resolves on each "
        "read");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "publishing a value restores the entry stack depth");
}

void CheckNamespaceValueAndOwnershipForms() {
  Luna::LifetimeHandle Host;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Host), "the model publishes");

  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  static_cast<void>(Studio.RegisterValue("Active", &OwnedCamera)
                        .RegisterValue("Shared", &SharedCamera));
  const Luna::RegistrationResult Committed = Studio.Commit();
  Check(Committed.IsSuccess(),
        "a namespace publishes by-value and shared instances");
  if (!Committed.IsSuccess()) {
    if (const Luna::ErrorDiagnostic *Diagnostic = Committed.Diagnostic())
      std::cerr << "namespace value refused: " << Diagnostic->Message() << '\n';
    return;
  }

  Check(Succeeds(Owner, "assert(Studio.Active.Zoom == 2.5)\n"
                        "assert(Studio.Shared.Zoom == 4)"),
        "a namespace-scoped value reads as userdata under its scope");
}

void CheckRefusedProducers() {
  Luna::LifetimeHandle Host;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Host), "the model publishes");

  Luna::BindingRegistry Registry = Owner.Bindings();

  const Luna::RegistrationResult Scalar =
      Registry.RegisterValue("Count", []() { return 7; });
  Check(!Scalar.IsSuccess(), "a producer returning a scalar is refused");

  const Luna::RegistrationResult Table = Registry.RegisterValue(
      "Shape", []() { return Luna::OwnedValue::Table(); });
  Check(!Table.IsSuccess(), "a producer returning a table is refused");

  const Luna::RegistrationResult Borrowed =
      Registry.RegisterValue("game", &HostGame);
  Check(!Borrowed.IsSuccess(),
        "a pointer producer without a declared lifetime is refused");

  const Luna::RegistrationResult Async =
      Registry.RegisterValue("Later", []() { return Luna::AsyncTask<int>(); });
  Check(!Async.IsSuccess(), "an asynchronous producer is refused");

  const Luna::RegistrationResult Missing = Registry.RegisterValue(
      "Nothing", []() -> Camera * { return nullptr; },
      Luna::OwnershipPolicy::Borrowed(Host));
  Check(!Missing.IsSuccess(), "a producer returning no object is refused");
}

void CheckUnregisteredClassIsRefused() {
  Luna::LifetimeHandle Host;
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  Luna::BindingRegistry Registry = Owner.Bindings();
  const Luna::RegistrationResult Published = Registry.RegisterValue(
      "game", &HostGame, Luna::OwnershipPolicy::Borrowed(Host));
  Check(!Published.IsSuccess(),
        "publishing an instance of a class this State never registered is "
        "refused");
}

void CheckValueIsReflectedAndGenerated() {
  Luna::LifetimeHandle Host;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Host), "the model publishes");

  Luna::BindingRegistry Registry = Owner.Bindings();
  static_cast<void>(Registry.RegisterValue(
      "game", &HostGame, Luna::OwnershipPolicy::Borrowed(Host)));

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ReflectionRecord Record = Snapshot.Find("game");
  Check(Record.IsValid() && Record.Kind() == Luna::SymbolKind::Constant,
        "a published value reflects as one value record");
  Check(Record.Descriptor().Kind() == Luna::TypeKind::Class,
        "the record names the registered class as its type");

  const Luna::GeneratedArtifact Declarations =
      Luna::GenerateDeclarations(Snapshot, Luna::DeclarationOptions());
  Check(Declarations.IsComplete(), "the declarations generate completely");
  const std::string Text(Declarations.Bytes());
  Check(Text.find("declare game: Studio_Game") != std::string::npos,
        "the value generates as a declared global of its class type");
}

} // namespace

int RunPublishedValueTests();

int RunPublishedValueTests() {
  FailureCount = 0;
  CheckGlobalValueResolvesWithoutACall();
  CheckNamespaceValueAndOwnershipForms();
  CheckRefusedProducers();
  CheckUnregisteredClassIsRefused();
  CheckValueIsReflectedAndGenerated();
  return FailureCount == 0 ? 0 : 1;
}
