// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <cstddef>
#include <iostream>
#include <optional>
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
  std::cerr << "converted operand check failed: " << Description << '\n';
}

// A plain aggregate whose only connection to the type system is a
// Luna::TypeConverter<T> specialization, exactly like the converted member
// value tests already exercise. Here it also appears as a Method operand
// and as a class operator's non-receiver operand.
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

  [[nodiscard]] double Dot(Vector3 Other) const {
    return Position.X * Other.X + Position.Y * Other.Y + Position.Z * Other.Z;
  }

  [[nodiscard]] double DistanceTo(const Vector3 &Other) const {
    const double DeltaX = Position.X - Other.X;
    const double DeltaY = Position.Y - Other.Y;
    const double DeltaZ = Position.Z - Other.Z;
    return DeltaX * DeltaX + DeltaY * DeltaY + DeltaZ * DeltaZ;
  }
};

[[nodiscard]] Luna::StableTypeKey BodyKey() {
  return Luna::StableTypeKey("Studio.OperandBody");
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

[[nodiscard]] Vector3 GetPosition(const Body &Self) {
  return Self.Position;
}

void SetPosition(Body &Self, Vector3 Updated) {
  Self.Position = std::move(Updated);
}

// A factory whose operands are both converted values. The natural shape for
// this boundary is "two vectors in, one instance out", and it is what used to
// compile cleanly and then fail preparation because a construction entry never
// staged its parameters' pending type records.
[[nodiscard]] Body Span(Vector3 From, const Vector3 &To) {
  Body Produced;
  Produced.Position = Vector3{To.X - From.X, To.Y - From.Y, To.Z - From.Z};
  return Produced;
}

[[nodiscard]] bool RegisterModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Body> Class =
      Studio.RegisterClass<Body>("Body", BodyKey());

  Luna::ClassBuilder<Body> &WithConstructor = Class.Constructor<>();
  Luna::ClassBuilder<Body> &WithProperty =
      WithConstructor.Property<Vector3>("Position", &GetPosition, &SetPosition);
  Luna::ClassBuilder<Body> &WithDot = WithProperty.Method("Dot", &Body::Dot);
  Luna::ClassBuilder<Body> &WithAdd =
      WithDot.Operator(Luna::ClassOperator::Add, &Body::DistanceTo);
  Luna::ClassBuilder<Body> &WithFactory = WithAdd.Factory("Span", &Span);
  static_cast<void>(WithFactory.QualifiedName());
  return Studio.Commit().IsSuccess();
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "converted operand source failed: " << Diagnostic->Message()
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

void CheckMethodTakesSameClassOperand() {
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "Value = Studio.Body.New()\n"
                        "Value.Position = {1, 2, 3}"),
        "a script constructs one object and sets its position");
  Check(Succeeds(Owner, "local D = Value:Dot({4, 5, 6})\n"
                        "assert(D == 1 * 4 + 2 * 5 + 3 * 6)"),
        "a method reads a converted operand from a table literal");

  const std::string Mistyped = Refusal(Owner, "Value:Dot({1, 2})");
  Check(!Mistyped.empty(),
        "a table of the wrong shape is refused before the native method runs");

  const std::string WrongType = Refusal(Owner, "Value:Dot(5)");
  Check(!WrongType.empty(),
        "a non-table operand is refused before the native method runs");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every converted-operand call and refusal restores the entry stack "
        "depth");
}

void CheckOperatorTakesConvertedOperand() {
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "Value = Studio.Body.New()\n"
                        "Value.Position = {1, 2, 3}"),
        "a script constructs one object and sets its position");
  Check(Succeeds(Owner, "local D = Value + {1, 2, 4}\n"
                        "assert(D == 1)"),
        "an add operator reads a converted operand from a table literal");
  Check(Succeeds(Owner, "local D = Value + {1, 2, 3}\n"
                        "assert(D == 0)"),
        "the same operator reads a different converted operand correctly");

  const std::string Mistyped = Refusal(Owner, "local D = Value + {1, 2}");
  Check(!Mistyped.empty(),
        "an operator refuses a wrongly shaped converted operand");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every operator call and refusal restores the entry stack depth");
}

void CheckFactoryTakesConvertedOperands() {
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner),
        "a model whose factory declares converted parameters publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "local S = Studio.Body.Span({1, 1, 1}, {4, 5, 7})\n"
                        "local P = S.Position\n"
                        "assert(P[1] == 3 and P[2] == 4 and P[3] == 6)"),
        "a factory reads two converted operands and publishes an instance");

  const std::string Mistyped =
      Refusal(Owner, "Studio.Body.Span({1, 1, 1}, {4, 5})");
  Check(!Mistyped.empty(),
        "a factory refuses a wrongly shaped converted operand before the "
        "native producer runs");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every factory call and refusal restores the entry stack depth");
}

} // namespace

int RunConvertedOperandTests();

int RunConvertedOperandTests() {
  FailureCount = 0;
  CheckMethodTakesSameClassOperand();
  CheckOperatorTakesConvertedOperand();
  CheckFactoryTakesConvertedOperands();
  return FailureCount == 0 ? 0 : 1;
}
