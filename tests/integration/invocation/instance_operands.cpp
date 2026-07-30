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
  std::cerr << "instance operand check failed: " << Description << '\n';
}

struct Bead final {
  int Weight = 0;
};

struct Point final {
  double X = 0.0;
  double Y = 0.0;

  [[nodiscard]] double Dot(const Point &Other) const {
    return X * Other.X + Y * Other.Y;
  }

  [[nodiscard]] Point Plus(const Point &Other) const {
    return Point{X + Other.X, Y + Other.Y};
  }

  [[nodiscard]] static Point Between(const Point &First, const Point &Second) {
    return Point{(First.X + Second.X) / 2.0, (First.Y + Second.Y) / 2.0};
  }
};

struct Trailing final {
  int Weight = 0;
};

struct Leading final {
  [[nodiscard]] int Take(const Trailing &Other) const { return Other.Weight; }
};

struct Slot final {
  int Total = 0;

  [[nodiscard]] int Measure(const Bead &Placed) const {
    return Total + Placed.Weight;
  }

  void Absorb(Bead *Placed) {
    if (!Placed)
      return;
    Total += Placed->Weight;
    Placed->Weight = 0;
  }

  [[nodiscard]] int Combine(const Slot &Other) const {
    return Total + Other.Total;
  }
};

} // namespace

template <> struct Luna::RegisteredClassTrait<Bead> : std::true_type {};
template <> struct Luna::RegisteredClassTrait<Leading> : std::true_type {};
template <> struct Luna::RegisteredClassTrait<Point> : std::true_type {};
template <> struct Luna::RegisteredClassTrait<Trailing> : std::true_type {};
template <> struct Luna::RegisteredClassTrait<Slot> : std::true_type {};

namespace {

[[nodiscard]] int WeighBoth(const Bead &First, const Bead &Second) {
  return First.Weight * 100 + Second.Weight;
}

[[nodiscard]] Luna::StableTypeKey BeadKey() {
  return Luna::StableTypeKey("Studio.OperandBead");
}

[[nodiscard]] Luna::StableTypeKey SlotKey() {
  return Luna::StableTypeKey("Studio.OperandSlot");
}

[[nodiscard]] bool RegisterModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

  Luna::ClassBuilder<Bead> Beads =
      Studio.RegisterClass<Bead>("Bead", BeadKey());
  Luna::ClassBuilder<Bead> &DeclaredBead =
      Beads.Constructor<>().Field("Weight", &Bead::Weight);
  static_cast<void>(DeclaredBead.QualifiedName());

  Luna::ClassBuilder<Slot> Slots =
      Studio.RegisterClass<Slot>("Slot", SlotKey());
  Luna::ClassBuilder<Slot> &DeclaredSlot =
      Slots.Constructor<>()
          .Field("Total", &Slot::Total)
          .Method("Measure", &Slot::Measure)
          .Method("Absorb", &Slot::Absorb)
          .Operator(Luna::ClassOperator::Add, &Slot::Combine)
          .StaticMethod("WeighBoth", &WeighBoth);
  static_cast<void>(DeclaredSlot.QualifiedName());

  const Luna::RegistrationResult Committed = Studio.Commit();
  if (!Committed.IsSuccess()) {
    if (const Luna::ErrorDiagnostic *Diagnostic = Committed.Diagnostic())
      std::cerr << "instance operand model refused: " << Diagnostic->Message()
                << '\n';
  }
  return Committed.IsSuccess();
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "instance operand source failed: " << Diagnostic->Message()
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

void CheckCrossClassOperand() {
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "S = Studio.Slot.New()\nS.Total = 5\n"
                        "B = Studio.Bead.New()\nB.Weight = 4"),
        "one instance of each class constructs");
  Check(Succeeds(Owner, "assert(S:Measure(B) == 9)"),
        "a method reads an operand that is an instance of another class");
  Check(Succeeds(Owner, "assert(Studio.Slot.WeighBoth(B, B) == 404)"),
        "a static method takes two instance operands");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every instance-operand call restores the entry stack depth");
}

void CheckSameClassOperandAndOperator() {
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");

  Check(Succeeds(Owner, "A = Studio.Slot.New()\nA.Total = 3\n"
                        "C = Studio.Slot.New()\nC.Total = 8"),
        "two instances of the same class construct");
  Check(Succeeds(Owner, "assert((A + C) == 11)"),
        "an operator takes an instance of its own class on the right");
  Check(Succeeds(Owner, "assert((C + A) == 11)"),
        "the operator reads either instance as its operand");
}

void CheckMutableOperandWrites() {
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");

  Check(Succeeds(Owner, "S = Studio.Slot.New()\nB = Studio.Bead.New()\n"
                        "B.Weight = 7"),
        "one instance of each class constructs");
  Check(Succeeds(Owner, "S:Absorb(B)\n"
                        "assert(S.Total == 7)\n"
                        "assert(B.Weight == 0)"),
        "a mutable pointer operand reaches the caller's own object");
}

void CheckWrongOperandIsRefused() {
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "S = Studio.Slot.New()\nB = Studio.Bead.New()"),
        "one instance of each class constructs");

  const std::string WrongClass = Refusal(Owner, "S:Measure(S)");
  Check(!WrongClass.empty(),
        "an instance of the wrong class is refused before the native method "
        "runs");

  const std::string NotAnInstance = Refusal(Owner, "S:Measure(5)");
  Check(!NotAnInstance.empty(), "a scalar operand is refused");

  const std::string Missing = Refusal(Owner, "S:Measure()");
  Check(!Missing.empty(), "an omitted instance operand is refused");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every refusal restores the entry stack depth");
}

void CheckSelfReferentialDeclarationsInOneChain() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

  Luna::ClassBuilder<Point> Points = Studio.RegisterClass<Point>(
      "Point", Luna::StableTypeKey("Studio.OperandPoint"));
  Luna::ClassBuilder<Point> &Declared =
      Points.Constructor<double, double>()
          .Constructor<Point>("Copy")
          .Field("X", &Point::X)
          .Field("Y", &Point::Y)
          .Method("Dot", &Point::Dot)
          .Operator(Luna::ClassOperator::Add, &Point::Plus)
          .Factory("Between", &Point::Between);
  static_cast<void>(Declared.QualifiedName());

  const Luna::RegistrationResult Committed = Studio.Commit();
  Check(Committed.IsSuccess(),
        "a class declares a method, an operator, a constructor, and a factory "
        "taking its own instances in the chain that registers it");
  if (!Committed.IsSuccess()) {
    if (const Luna::ErrorDiagnostic *Diagnostic = Committed.Diagnostic())
      std::cerr << "self-referential model refused: " << Diagnostic->Message()
                << '\n';
    return;
  }

  Check(Succeeds(Owner, "local A = Studio.Point.New(3, 4)\n"
                        "local B = Studio.Point.New(1, 0)\n"
                        "assert(A:Dot(B) == 3)"),
        "a method resolves an operand of its own class at run time");
  Check(Succeeds(Owner, "local A = Studio.Point.New(3, 4)\n"
                        "local B = Studio.Point.New(1, 2)\n"
                        "local S = A + B\n"
                        "assert(S.X == 4 and S.Y == 6)"),
        "an operator takes and publishes its own class");
  Check(Succeeds(Owner, "local A = Studio.Point.New(2, 2)\n"
                        "local B = Studio.Point.New(4, 6)\n"
                        "local M = Studio.Point.Between(A, B)\n"
                        "assert(M.X == 3 and M.Y == 4)"),
        "a factory takes two operands of the class it constructs");
  Check(Succeeds(Owner, "local A = Studio.Point.New(5, 6)\n"
                        "local C = Studio.Point.Copy(A)\n"
                        "assert(C.X == 5 and C.Y == 6)\n"
                        "C.X = 9\n"
                        "assert(A.X == 5)"),
        "a constructor takes one instance of its own class by value");
}

void CheckStagingOrderWithinOnePlan() {
  {
    Luna::State Owner;
    Check(Owner.IsReady(), "the state is ready");
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

    Luna::ClassBuilder<Leading> Leaders = Studio.RegisterClass<Leading>(
        "Leading", Luna::StableTypeKey("Studio.OperandLeading"));
    static_cast<void>(Leaders.Constructor<>().Method("Take", &Leading::Take));

    Luna::ClassBuilder<Trailing> Trailers = Studio.RegisterClass<Trailing>(
        "Trailing", Luna::StableTypeKey("Studio.OperandTrailing"));
    static_cast<void>(
        Trailers.Constructor<>().Field("Weight", &Trailing::Weight));

    Check(!Studio.Commit().IsSuccess(),
          "an operand class staged later in the same plan is refused, because "
          "a plan is validated in staging order");
  }

  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

  Luna::ClassBuilder<Trailing> Trailers = Studio.RegisterClass<Trailing>(
      "Trailing", Luna::StableTypeKey("Studio.OperandTrailing"));
  static_cast<void>(
      Trailers.Constructor<>().Field("Weight", &Trailing::Weight));

  Luna::ClassBuilder<Leading> Leaders = Studio.RegisterClass<Leading>(
      "Leading", Luna::StableTypeKey("Studio.OperandLeading"));
  static_cast<void>(Leaders.Constructor<>().Method("Take", &Leading::Take));

  Check(
      Studio.Commit().IsSuccess(),
      "an operand class staged earlier in the same plan is accepted before it "
      "commits");
  Check(Succeeds(Owner, "local L = Studio.Leading.New()\n"
                        "local T = Studio.Trailing.New()\n"
                        "T.Weight = 6\n"
                        "assert(L:Take(T) == 6)"),
        "the operand resolves at run time once the plan commits");
}

} // namespace

int RunInstanceOperandTests();

int RunInstanceOperandTests() {
  FailureCount = 0;
  CheckStagingOrderWithinOnePlan();
  CheckCrossClassOperand();
  CheckSameClassOperandAndOperator();
  CheckMutableOperandWrites();
  CheckWrongOperandIsRefused();
  CheckSelfReferentialDeclarationsInOneChain();
  return FailureCount == 0 ? 0 : 1;
}
