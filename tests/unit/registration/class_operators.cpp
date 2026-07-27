// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_operator.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/testing/test_hooks.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;
std::uint64_t Lifetime = 1;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "class operator check failed: " << Description << '\n';
}

[[nodiscard]] bool Contains(std::string_view Text, std::string_view Needle) {
  return Text.find(Needle) != std::string_view::npos;
}

struct OperatorValue final {
  int Stored = 5;
  int Assigned = 0;

  [[nodiscard]] std::pair<int, std::string> Call(int Added) const {
    return {Stored + Added, "called"};
  }
  [[nodiscard]] int Length() const { return Stored; }
  [[nodiscard]] bool Equal(int Other) const { return Stored == Other; }
  [[nodiscard]] bool Less(int Other) const { return Stored < Other; }
  [[nodiscard]] bool LessEqual(int Other) const { return Stored <= Other; }
  [[nodiscard]] int AddInteger(int Other) const { return Stored + Other; }
  [[nodiscard]] double AddNumber(double Other) const {
    return static_cast<double>(Stored) + Other;
  }
  [[nodiscard]] int Subtract(int Other) const { return Stored - Other; }
  [[nodiscard]] int Multiply(int Other) const { return Stored * Other; }
  [[nodiscard]] double Divide(double Other) const {
    return static_cast<double>(Stored) / Other;
  }
  [[nodiscard]] int Modulo(int Other) const { return Stored % Other; }
  [[nodiscard]] double Power(double Other) const {
    double Result = 1.0;
    for (int Index = 0; Index < static_cast<int>(Other); ++Index)
      Result *= static_cast<double>(Stored);
    return Result;
  }
  [[nodiscard]] int Negate() const { return -Stored; }
  [[nodiscard]] std::string Concatenate(std::string Other) const {
    return std::to_string(Stored) + Other;
  }
  [[nodiscard]] std::string ToText() const { return std::to_string(Stored); }
  [[nodiscard]] int Index(std::string) const { return Stored; }
  void Assign(std::string, int Value) { Assigned = Value; }

  void BadValueOperator(int) {}
  [[nodiscard]] int BadAssignment(std::string, int) { return 1; }
};

[[nodiscard]] Luna::StableTypeKey OperatorKey() {
  return Luna::StableTypeKey("Studio.OperatorValue");
}

[[nodiscard]] Luna::RegistrationResult RegisterOperators(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<OperatorValue> Class =
      Studio.RegisterClass<OperatorValue>("OperatorValue", OperatorKey());
  Class.Operator(Luna::ClassOperator::Call, &OperatorValue::Call)
      .Operator(Luna::ClassOperator::Length, &OperatorValue::Length)
      .Operator(Luna::ClassOperator::Equal, &OperatorValue::Equal)
      .Operator(Luna::ClassOperator::Less, &OperatorValue::Less)
      .Operator(Luna::ClassOperator::LessEqual, &OperatorValue::LessEqual)
      .Operator(Luna::ClassOperator::Add, &OperatorValue::AddInteger)
      .Operator(Luna::ClassOperator::Add, &OperatorValue::AddNumber)
      .Operator(Luna::ClassOperator::Subtract, &OperatorValue::Subtract)
      .Operator(Luna::ClassOperator::Multiply, &OperatorValue::Multiply)
      .Operator(Luna::ClassOperator::Divide, &OperatorValue::Divide)
      .Operator(Luna::ClassOperator::Modulo, &OperatorValue::Modulo)
      .Operator(Luna::ClassOperator::Power, &OperatorValue::Power)
      .Operator(Luna::ClassOperator::Negate, &OperatorValue::Negate)
      .Operator(Luna::ClassOperator::Concatenate, &OperatorValue::Concatenate)
      .Operator(Luna::ClassOperator::ToText, &OperatorValue::ToText)
      .Operator(Luna::ClassOperator::Index, &OperatorValue::Index)
      .Operator(Luna::ClassOperator::Assign, &OperatorValue::Assign);
  return Studio.Commit();
}
void CheckEveryOperatorUsesOrdinaryDispatch() {
  Luna::State Owner;
  Check(RegisterOperators(Owner).IsSuccess(),
        "all supported operators and two add overloads register together");
  Check(Hooks::ClassOperatorCount(Owner, "Studio.OperatorValue") == 16,
        "one canonical mapping is published for every supported operator");

  OperatorValue Value;
  Luna::Detail::ClassExposureRequest Exposure;
  Exposure.QualifiedName = "Studio.OperatorValue";
  Exposure.Path = "Value";
  Exposure.Storage = &Value;
  Exposure.Ownership = Luna::Detail::OwnershipModel::Borrowed;
  Exposure.Access = Luna::Detail::ConstAccess::Mutable;
  Exposure.LifetimeGeneration = &Lifetime;
  Check(Hooks::ExposeClassUserdata(Owner, Exposure).Status == "created",
        "the operator receiver is exposed through its registered class");

  const auto Executed =
      Owner.Execute("assert(Value + 2 == 7, 'add integer')\n"
                    "assert(Value + 2.5 == 7.5, 'add number')\n"
                    "assert(Value - 2 == 3, 'subtract')\n"
                    "assert(Value * 3 == 15, 'multiply')\n"
                    "assert(Value / 2 == 2.5, 'divide')\n"
                    "assert(Value % 3 == 2, 'modulo')\n"
                    "assert(Value ^ 2 == 25, 'power')\n"
                    "assert(-Value == -5, 'negate')\n"
                    "assert(#Value == 5, 'length')\n"
                    "assert(Value .. '!' == '5!', 'concatenate')\n"
                    "assert(tostring(Value) == '5', 'to text')\n"
                    "local First, Second = Value(3)\n"
                    "assert(First == 8 and Second == 'called', 'call')\n"
                    "assert(Value.Missing == 5, 'index')\n"
                    "Value.Missing = 19\n");
  if (!Executed.IsSuccess() && Executed.Diagnostic() != nullptr)
    std::cerr << "operator execution diagnostic: "
              << Executed.Diagnostic()->Message() << '\n';
  Check(Executed.IsSuccess(),
        "operators execute through receiver validation and ordinary overloads");
  Check(Value.Assigned == 19,
        "reserved assignment dispatch invokes the selected native target");
}

void CheckOperatorReflectionIsCanonicalAndOwning() {
  Luna::State Owner;
  Check(RegisterOperators(Owner).IsSuccess(),
        "the reflected operator model registers");
  const Luna::ReflectionSnapshot Snapshot = Owner.Bindings().Reflection();
  const Luna::ReflectionRecord Class = Snapshot.Find("Studio.OperatorValue");
  Check(Class.IsValid() && Class.Kind() == Luna::SymbolKind::Class,
        "the reflected class record is available");

  std::size_t MappingCount = 0;
  std::string Previous;
  for (std::size_t Index = 0; Index < Class.RelationCount(); ++Index) {
    const Luna::TypeRelation Relation = Class.Relation(Index);
    if (Relation.Kind() != Luna::TypeRelationKind::Operand)
      continue;
    ++MappingCount;
    Check(Relation.Declaration().IsValid(),
          "each operator mapping retains its overload-set declaration");
    Check(Previous.empty() || Previous < Relation.Note(),
          "operator mappings enumerate in canonical order");
    Previous = std::string(Relation.Note());
  }
  Check(MappingCount == 16,
        "reflection exposes one mapping for every supported operator");
  Check(Snapshot.Symbols(Luna::SymbolKind::Operator).Size() == 17,
        "reflection retains both add overload declarations without cloning");
}
void CheckReservedMetamethodRolesCannotBeReplaced() {
  const std::vector<std::pair<std::string_view, std::string_view>> Reserved = {
      {"__type", "type identity"},
      {"__index", "member dispatch"},
      {"__metatable", "metatable protection"},
      {"__mode", "lifetime"},
      {"__gc", "garbage collection"},
  };

  for (const auto &[Name, Role] : Reserved) {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<OperatorValue> Class =
        Registry.RegisterClass<OperatorValue>("OperatorValue", OperatorKey());
    const Luna::RegistrationResult Result =
        Class.Method(Name, &OperatorValue::Length).Commit();
    Check(!Result.IsSuccess(),
          "a Luna-owned metamethod cannot be replaced by a method");
    const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
    Check(Diagnostic != nullptr && Contains(Diagnostic->Message(), Role),
          "the reserved-name diagnostic identifies the protected role");
  }
}

void CheckInvalidOperatorShapesAreTransactional() {
  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<OperatorValue> Class =
        Registry.RegisterClass<OperatorValue>("OperatorValue", OperatorKey());
    const auto Result = Class
                            .Operator(Luna::ClassOperator::Add,
                                      &OperatorValue::BadValueOperator)
                            .Commit();
    Check(!Result.IsSuccess(),
          "a value-producing operator cannot declare a void target");
  }
  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<OperatorValue> Class =
        Registry.RegisterClass<OperatorValue>("OperatorValue", OperatorKey());
    const auto Result = Class
                            .Operator(Luna::ClassOperator::Assign,
                                      &OperatorValue::BadAssignment)
                            .Commit();
    Check(!Result.IsSuccess(), "an assignment operator cannot publish a value");
  }
  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<OperatorValue> Class =
        Registry.RegisterClass<OperatorValue>("OperatorValue", OperatorKey());
    Class.Operator(Luna::ClassOperator::Add, &OperatorValue::AddInteger)
        .Operator(Luna::ClassOperator::Add, &OperatorValue::AddInteger);
    Check(!Class.Commit().IsSuccess(),
          "indistinguishable operator overloads remain duplicates");
  }
}

} // namespace

int RunClassOperatorTests();

int RunClassOperatorTests() {
  FailureCount = 0;
  CheckEveryOperatorUsesOrdinaryDispatch();
  CheckOperatorReflectionIsCanonicalAndOwning();
  CheckReservedMetamethodRolesCannotBeReplaced();
  CheckInvalidOperatorShapesAreTransactional();
  return FailureCount == 0 ? 0 : 1;
}
