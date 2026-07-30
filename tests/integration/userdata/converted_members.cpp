// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <cstddef>
#include <iostream>
#include <optional>
#include <string_view>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "converted member check failed: " << Description << '\n';
}

struct Vector3 final {
  double X = 0.0;
  double Y = 0.0;
  double Z = 0.0;
};

[[nodiscard]] bool operator==(const Vector3 &Left, const Vector3 &Right) {
  return Left.X == Right.X && Left.Y == Right.Y && Left.Z == Right.Z;
}

struct Body final {
  Vector3 Position;
  Vector3 Velocity{1.0, 2.0, 3.0};

  [[nodiscard]] Vector3 GetPosition() const { return Position; }
  void SetPosition(Vector3 Updated) { Position = std::move(Updated); }
};

[[nodiscard]] Luna::StableTypeKey BodyKey() {
  return Luna::StableTypeKey("Studio.ConvertedBody");
}

} // namespace

namespace Luna {

template <> class TypeConverter<Vector3> {
public:
  [[nodiscard]] ConversionProbe Probe(ValueView Source,
                                      const ConversionContext &Context) const {
    if (!Source.IsTable() || Source.Size() != 3)
      return RejectedProbe(Context.Describe("a Vector3 is a table of X, Y, Z"));
    return ViableProbe(ConversionRank::User);
  }

  [[nodiscard]] ConversionResult<Vector3>
  Read(ValueView Source, ConversionContext &Context) const {
    ConversionResult<Vector3> Result;
    const std::optional<double> X = Source.Element(0).ToNumber();
    const std::optional<double> Y = Source.Element(1).ToNumber();
    const std::optional<double> Z = Source.Element(2).ToNumber();
    if (!X || !Y || !Z) {
      Result.Status = ConversionStatus::TypeMismatch;
      Result.Diagnostic = Context.Describe("expected three numeric elements");
      return Result;
    }
    Result.Status = ConversionStatus::Success;
    Result.ConvertedValue = Vector3{*X, *Y, *Z};
    return Result;
  }

  [[nodiscard]] WriteResult Write(const Vector3 &Source,
                                  ConversionContext &Context) const {
    OwnedValue Published = OwnedValue::Table();
    Published.Append(OwnedValue::Number(Source.X));
    Published.Append(OwnedValue::Number(Source.Y));
    Published.Append(OwnedValue::Number(Source.Z));
    const WriteResult Reserved =
        Context.Reserve(Published.RequiredReservation());
    if (!Reserved.IsSuccess())
      return Reserved;
    return Context.Publish(Published);
  }
};

} // namespace Luna

namespace {

[[nodiscard]] bool RegisterModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Body> Class =
      Studio.RegisterClass<Body>("Body", BodyKey());

  Luna::ClassBuilder<Body> &WithConstructor = Class.Constructor<>();
  Luna::ClassBuilder<Body> &WithProperty = WithConstructor.Property<Vector3>(
      "Position", &Body::GetPosition, &Body::SetPosition);
  Luna::ClassBuilder<Body> &WithField =
      WithProperty.Field<Vector3>("Velocity", &Body::Velocity);
  static_cast<void>(WithField.QualifiedName());
  return Studio.Commit().IsSuccess();
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "converted member source failed: " << Diagnostic->Message()
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

void CheckConvertedPropertyReadsAndWrites() {
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "Value = Studio.Body.New()"),
        "a script constructs one object of the class");
  Check(Succeeds(Owner, "local P = Value.Position\n"
                        "assert(P[1] == 0 and P[2] == 0 and P[3] == 0)"),
        "a converted property reads through its declared getter as a table");
  Check(Succeeds(Owner, "Value.Position = {1, 2, 3}"),
        "a converted property writes through its declared setter from a "
        "table");
  Check(Succeeds(Owner, "local P = Value.Position\n"
                        "assert(P[1] == 1 and P[2] == 2 and P[3] == 3)"),
        "the write reached the native object the getter reads back");

  Check(Succeeds(Owner, "local V = Value.Velocity\n"
                        "assert(V[1] == 1 and V[2] == 2 and V[3] == 3)"),
        "a converted field reads its native value as a table");
  Check(Succeeds(Owner, "Value.Velocity = {5, 6, 7}"),
        "a converted field writes through its own TypeConverter setter");
  Check(Succeeds(Owner, "local V = Value.Velocity\n"
                        "assert(V[1] == 5 and V[2] == 6 and V[3] == 7)"),
        "the converted field write reached the object its getter reads");

  const std::string Mistyped = Refusal(Owner, "Value.Position = {1, 2}");
  Check(!Mistyped.empty(),
        "a table of the wrong shape is refused by the value's own probe");

  const std::string WrongType = Refusal(Owner, "Value.Position = 5");
  Check(!WrongType.empty(),
        "a non-table value is refused before the native setter runs");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every converted read, write, and refusal restores the entry stack "
        "depth");
}

} // namespace

int RunConvertedMemberTests();

int RunConvertedMemberTests() {
  FailureCount = 0;
  CheckConvertedPropertyReadsAndWrites();
  return FailureCount == 0 ? 0 : 1;
}
