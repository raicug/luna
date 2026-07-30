// clang-format off
#include <luna/luna.hpp>
#include "state/testing/test_hooks.hpp"
#include <iostream>
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
  std::cerr << "class constant check failed: " << Description << '\n';
}

enum class Channel : int { Debug = 10, Info = 20 };

struct Vector final {
  double X = 0.0;
  double Y = 0.0;

  [[nodiscard]] double Magnitude() const { return X + Y; }

  [[nodiscard]] static Vector Origin() { return Vector{}; }
};

} // namespace

template <> struct Luna::RegisteredClassTrait<Vector> : std::true_type {};

namespace {

[[nodiscard]] Luna::StableTypeKey VectorKey() {
  return Luna::StableTypeKey("Studio.ConstantVector");
}

[[nodiscard]] Luna::StableTypeKey ChannelKey() {
  return Luna::StableTypeKey("Studio.ConstantChannel");
}

[[nodiscard]] bool RegisterModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

  Luna::EnumBuilder<Channel> Channels =
      Studio.RegisterEnum<Channel>("Channel", ChannelKey());
  static_cast<void>(Channels.Value("Debug", Channel::Debug)
                        .Value("Info", Channel::Info)
                        .QualifiedName());

  Luna::ClassBuilder<Vector> Vectors =
      Studio.RegisterClass<Vector>("Vector", VectorKey());
  Luna::ClassBuilder<Vector> &Declared =
      Vectors.Constructor<>()
          .Field("X", &Vector::X)
          .Field("Y", &Vector::Y)
          .Property("Magnitude", &Vector::Magnitude)
          .StaticMethod("Origin", &Vector::Origin)
          .Constant("Dimensions", 2)
          .Constant("xAxis", "1,0")
          .Constant("Epsilon", 0.5)
          .Constant("Traced", false)
          .Constant("DefaultChannel", Channel::Info, ChannelKey())
          .Documentation("Dimensions", "How many axes one vector carries.");
  static_cast<void>(Declared.QualifiedName());

  const Luna::RegistrationResult Committed = Studio.Commit();
  if (!Committed.IsSuccess()) {
    if (const Luna::ErrorDiagnostic *Diagnostic = Committed.Diagnostic())
      std::cerr << "class constant model refused: " << Diagnostic->Message()
                << '\n';
  }
  return Committed.IsSuccess();
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "class constant source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

void CheckClassConstantsReachTheClassTable() {
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "assert(Studio.Vector.Dimensions == 2)\n"
                        "assert(Studio.Vector.xAxis == '1,0')\n"
                        "assert(Studio.Vector.Epsilon == 0.5)\n"
                        "assert(Studio.Vector.Traced == false)\n"
                        "assert(Studio.Vector.DefaultChannel == 20)"),
        "a class constant is a plain value on the class table, not a call");

  Check(Succeeds(Owner, "local V = Studio.Vector.New()\n"
                        "V.X = 1\nV.Y = 2\n"
                        "assert(V.Magnitude == 3)\n"
                        "assert(Studio.Vector.Origin().X == 0)"),
        "constants sit alongside the construction candidates and static "
        "methods of the same class");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "reading a class constant restores the entry stack depth");
}

void CheckClassConstantsAreReflected() {
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");

  const Luna::ReflectionSnapshot Snapshot = Owner.Bindings().Reflection();
  const Luna::ReflectionRecord Record =
      Snapshot.Find("Studio.Vector.Dimensions");
  Check(Record.IsValid() && Record.Kind() == Luna::SymbolKind::Constant,
        "a class constant publishes one constant reflection record");
  Check(Record.HasValue() && Record.ValueText() == "2",
        "the record carries the declared value");
  Check(Record.Documentation() == "How many axes one vector carries.",
        "a class constant is documented through the class builder");

  const Luna::ReflectionRecord Class = Snapshot.Find("Studio.Vector");
  Check(Class.IsValid() && Record.Scope().Owner() == Class.Id(),
        "the constant is scoped to the class that declared it");

  const Luna::GeneratedArtifact Declarations =
      Luna::GenerateDeclarations(Snapshot, Luna::DeclarationOptions());
  Check(Declarations.IsComplete(), "the declarations generate completely");
  const std::string Text(Declarations.Bytes());
  Check(Text.find("Dimensions: number,") != std::string::npos,
        "a class constant generates as a field of the class table");
  Check(Text.find("xAxis: string,") != std::string::npos,
        "a string class constant generates with its declared type");
  Check(Text.find("function Dimensions") == std::string::npos,
        "a class constant never generates as a function");
}

void CheckConstantCollidingWithAMemberIsRefused() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Vector> Vectors =
      Studio.RegisterClass<Vector>("Vector", VectorKey());
  Luna::ClassBuilder<Vector> &Declared =
      Vectors.Field("X", &Vector::X).Constant("X", 1);
  static_cast<void>(Declared.QualifiedName());

  const Luna::RegistrationResult Committed = Studio.Commit();
  Check(!Committed.IsSuccess(),
        "a constant colliding with a declared member name is refused");
}

void CheckConstantCollidingWithAStaticMethodIsRefused() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Vector> Vectors =
      Studio.RegisterClass<Vector>("Vector", VectorKey());
  Luna::ClassBuilder<Vector> &Declared =
      Vectors.StaticMethod("Origin", &Vector::Origin).Constant("Origin", 0);
  static_cast<void>(Declared.QualifiedName());

  const Luna::RegistrationResult Committed = Studio.Commit();
  Check(!Committed.IsSuccess(),
        "a constant colliding with a static method name is refused");
}

void CheckDuplicateConstantIsRefused() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Vector> Vectors =
      Studio.RegisterClass<Vector>("Vector", VectorKey());
  Luna::ClassBuilder<Vector> &Declared =
      Vectors.Constant("Dimensions", 2).Constant("Dimensions", 3);
  static_cast<void>(Declared.QualifiedName());

  const Luna::RegistrationResult Committed = Studio.Commit();
  Check(!Committed.IsSuccess(), "a duplicate class constant is refused");
}

void CheckConstructionNameCollisionIsRefused() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Vector> Vectors =
      Studio.RegisterClass<Vector>("Vector", VectorKey());
  Luna::ClassBuilder<Vector> &Declared =
      Vectors.Constant("New", 1).Constructor<>();
  static_cast<void>(Declared.QualifiedName());

  const Luna::RegistrationResult Committed = Studio.Commit();
  Check(!Committed.IsSuccess(),
        "a constructor colliding with an already declared class constant is "
        "refused");
}

} // namespace

int RunClassConstantTests();

int RunClassConstantTests() {
  FailureCount = 0;
  CheckClassConstantsReachTheClassTable();
  CheckClassConstantsAreReflected();
  CheckConstantCollidingWithAMemberIsRefused();
  CheckConstantCollidingWithAStaticMethodIsRefused();
  CheckDuplicateConstantIsRefused();
  CheckConstructionNameCollisionIsRefused();
  return FailureCount == 0 ? 0 : 1;
}
