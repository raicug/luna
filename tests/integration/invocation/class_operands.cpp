// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

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
  std::cerr << "class operand check failed: " << Description << '\n';
}

// A registered class that also declares its own converter, which is how a
// class instance reaches an operand position: the operand is converted, and
// the converter recovers the native object from the userdata value it is
// handed rather than rebuilding one from a table.
struct Vec final {
  double X = 0.0;
  double Y = 0.0;
  double Z = 0.0;

  [[nodiscard]] double Dot(const Vec &Other) const {
    return X * Other.X + Y * Other.Y + Z * Other.Z;
  }

  [[nodiscard]] double Combine(const Vec &Other) const { return Dot(Other); }
};

// A second registered class, used only to confirm that an instance of the
// wrong class is refused rather than reinterpreted.
struct Tag final {
  int Number = 0;
};

[[nodiscard]] Luna::StableTypeKey VecKey() {
  return Luna::StableTypeKey("Studio.OperandVec");
}

[[nodiscard]] Luna::StableTypeKey TagKey() {
  return Luna::StableTypeKey("Studio.OperandTag");
}

constexpr std::string_view VecClassName = "Studio.Vec";

} // namespace

namespace Luna {

template <> class TypeConverter<Vec> {
public:
  [[nodiscard]] ConversionProbe Probe(ValueView Source,
                                      const ConversionContext &Context) const {
    if (Source.IsUserdata()) {
      if (Source.UserdataClassName() != VecClassName)
        return RejectedProbe(
            Context.Describe("expected a Studio.Vec instance, received a " +
                             std::string(Source.UserdataClassName())));
      if (Source.UserdataStorage() == nullptr)
        return RejectedProbe(
            Context.Describe("the Studio.Vec instance is no longer usable"));
      return ViableProbe(ConversionRank::Exact);
    }
    if (Source.IsTable() && Source.Size() == 3)
      return ViableProbe(ConversionRank::User);
    return RejectedProbe(
        Context.Describe("expected a Studio.Vec instance or a table of three"));
  }

  [[nodiscard]] ConversionResult<Vec> Read(ValueView Source,
                                           ConversionContext &Context) const {
    ConversionResult<Vec> Result;
    if (Source.IsUserdata()) {
      if (Source.UserdataClassName() != VecClassName) {
        Result.Status = ConversionStatus::TypeMismatch;
        Result.Diagnostic = Context.Describe("expected a Studio.Vec instance");
        return Result;
      }
      const void *Object = Source.UserdataStorage();
      if (Object == nullptr) {
        Result.Status = ConversionStatus::Rejected;
        Result.Diagnostic =
            Context.Describe("the Studio.Vec instance is no longer usable");
        return Result;
      }
      Result.Status = ConversionStatus::Success;
      Result.ConvertedValue = *static_cast<const Vec *>(Object);
      return Result;
    }

    const std::optional<double> X = Source.Element(0).ToNumber();
    const std::optional<double> Y = Source.Element(1).ToNumber();
    const std::optional<double> Z = Source.Element(2).ToNumber();
    if (!X || !Y || !Z) {
      Result.Status = ConversionStatus::TypeMismatch;
      Result.Diagnostic = Context.Describe("expected three numeric elements");
      return Result;
    }
    Result.Status = ConversionStatus::Success;
    Result.ConvertedValue = Vec{*X, *Y, *Z};
    return Result;
  }

  [[nodiscard]] WriteResult Write(const Vec &Source,
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

[[nodiscard]] Vec *AnchorVec() {
  static Vec Anchor{7.0, 0.0, 0.0};
  return &Anchor;
}

[[nodiscard]] bool RegisterModel(Luna::State &Owner,
                                 const Luna::LifetimeHandle &Anchor) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

  Luna::ClassBuilder<Vec> Vectors = Studio.RegisterClass<Vec>("Vec", VecKey());
  Luna::ClassBuilder<Vec> &Declared =
      Vectors.Constructor<double, double, double>()
          .Field("X", &Vec::X)
          .Field("Y", &Vec::Y)
          .Field("Z", &Vec::Z)
          .Method("Dot", &Vec::Dot)
          .Operator(Luna::ClassOperator::Add, &Vec::Combine)
          .Singleton("Anchor", &AnchorVec,
                     Luna::OwnershipPolicy::Borrowed(Anchor));
  static_cast<void>(Declared.QualifiedName());

  Luna::ClassBuilder<Tag> Tags = Studio.RegisterClass<Tag>("Tag", TagKey());
  Luna::ClassBuilder<Tag> &WithTag = Tags.Constructor<>();
  static_cast<void>(WithTag.QualifiedName());

  return Studio.Commit().IsSuccess();
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "class operand source failed: " << Diagnostic->Message()
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

void CheckMethodAndOperatorTakeInstanceOperands() {
  Luna::LifetimeHandle Anchor;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Anchor), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "A = Studio.Vec.New(1, 2, 3)\n"
                        "B = Studio.Vec.New(4, 5, 6)"),
        "two instances construct");
  Check(Succeeds(Owner, "assert(A:Dot(B) == 1 * 4 + 2 * 5 + 3 * 6)"),
        "a method takes another instance of the same class as its operand");
  Check(Succeeds(Owner, "assert((A + B) == 32)"),
        "an operator takes an instance on both sides");
  Check(Succeeds(Owner, "assert(A:Dot({4, 5, 6}) == 32)"),
        "the same operand still accepts the converter's table form");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every instance-operand call restores the entry stack depth");
}

void CheckWrongClassOperandIsRefused() {
  Luna::LifetimeHandle Anchor;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Anchor), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "A = Studio.Vec.New(1, 2, 3)\n"
                        "T = Studio.Tag.New()"),
        "one instance of each class constructs");

  const std::string Mismatched = Refusal(Owner, "A:Dot(T)");
  Check(Mismatched.find("Studio.Tag") != std::string::npos,
        "an instance of the wrong class is refused, naming the class it "
        "actually received");

  const std::string Operated = Refusal(Owner, "local V = A + T");
  Check(!Operated.empty(),
        "an operator refuses an instance of the wrong class too");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every refusal restores the entry stack depth");
}

void CheckStaleBorrowedOperandIsRefused() {
  Luna::LifetimeHandle Anchor;
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner, Anchor), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "A = Studio.Vec.New(1, 2, 3)\n"
                        "Borrowed = Studio.Vec.Anchor()"),
        "a borrowed singleton is reachable while its lifetime is live");
  Check(Succeeds(Owner, "assert(A:Dot(Borrowed) == 7)"),
        "a live borrowed instance is usable as an operand");

  Anchor.Invalidate();

  const std::string Stale = Refusal(Owner, "A:Dot(Borrowed)");
  Check(!Stale.empty(),
        "an operand whose borrowed lifetime has been invalidated is refused "
        "rather than dereferenced");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "the stale-operand refusal restores the entry stack depth");
}

} // namespace

int RunClassOperandTests();

int RunClassOperandTests() {
  FailureCount = 0;
  CheckMethodAndOperatorTakeInstanceOperands();
  CheckWrongClassOperandIsRefused();
  CheckStaleBorrowedOperandIsRefused();
  return FailureCount == 0 ? 0 : 1;
}
