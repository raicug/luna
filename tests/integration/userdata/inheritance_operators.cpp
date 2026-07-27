// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;
std::uint64_t Lifetime = 1;
std::size_t TaggedCalls = 0;
std::size_t DerivedCalls = 0;
std::size_t AddCalls = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "inheritance/operator integration check failed: " << Description
            << '\n';
}

struct PolymorphicBase {
  virtual ~PolymorphicBase() = default;
  int Value = 10;
};

struct TaggedBase {
  int Tag = 7;
  [[nodiscard]] int ReadTag() const {
    ++TaggedCalls;
    return Tag;
  }
};

struct DerivedValue final : PolymorphicBase, TaggedBase {
  [[nodiscard]] int Specific(int Added) const {
    ++DerivedCalls;
    return Value + Added;
  }
};
struct OperatorValue final {
  int Value = 4;
  [[nodiscard]] int Add(int Other) const {
    ++AddCalls;
    return Value + Other;
  }
  [[nodiscard]] int Negate() const { return -Value; }
  [[nodiscard]] std::string Text() const { return std::to_string(Value); }
};

[[nodiscard]] Luna::StableTypeKey BaseKey() {
  return Luna::StableTypeKey("Integration.InheritanceBase");
}
[[nodiscard]] Luna::StableTypeKey TaggedKey() {
  return Luna::StableTypeKey("Integration.InheritanceTagged");
}
[[nodiscard]] Luna::StableTypeKey DerivedKey() {
  return Luna::StableTypeKey("Integration.InheritanceDerived");
}
[[nodiscard]] Luna::StableTypeKey OperatorKey() {
  return Luna::StableTypeKey("Integration.OperatorValue");
}

[[nodiscard]] Luna::RegistrationResult RegisterModel(Luna::State &Owner) {
  Luna::NamespaceBuilder Integration =
      Owner.Bindings().RegisterNamespace("Integration");
  Luna::ClassBuilder<PolymorphicBase> Base =
      Integration.RegisterClass<PolymorphicBase>("Base", BaseKey());
  static_cast<void>(Base.QualifiedName());
  Luna::ClassBuilder<TaggedBase> Tagged =
      Integration.RegisterClass<TaggedBase>("Tagged", TaggedKey());
  static_cast<void>(
      Tagged.Method("ReadTag", &TaggedBase::ReadTag).QualifiedName());
  Luna::ClassBuilder<DerivedValue> Derived =
      Integration.RegisterClass<DerivedValue>("Derived", DerivedKey());
  static_cast<void>(Derived.Method("Specific", &DerivedValue::Specific)
                        .Base<TaggedBase>(TaggedKey())
                        .Base<PolymorphicBase>(BaseKey())
                        .Cast<PolymorphicBase>(BaseKey())
                        .QualifiedName());
  Luna::ClassBuilder<OperatorValue> Operators =
      Integration.RegisterClass<OperatorValue>("OperatorValue", OperatorKey());
  static_cast<void>(
      Operators.Operator(Luna::ClassOperator::Add, &OperatorValue::Add)
          .Operator(Luna::ClassOperator::Negate, &OperatorValue::Negate)
          .Operator(Luna::ClassOperator::ToText, &OperatorValue::Text)
          .QualifiedName());
  return Integration.Commit();
}

void Expose(Luna::State &Owner, std::string Path, void *Storage,
            std::string QualifiedName) {
  Luna::Detail::ClassExposureRequest Request;
  Request.Path = std::move(Path);
  Request.Storage = Storage;
  Request.QualifiedName = std::move(QualifiedName);
  Request.Ownership = Luna::Detail::OwnershipModel::Borrowed;
  Request.Access = Luna::Detail::ConstAccess::Mutable;
  Request.LifetimeGeneration = &Lifetime;
  Check(Hooks::ExposeClassUserdata(Owner, Request).Status == "created",
        "the integration value is exposed exactly once");
}

[[nodiscard]] std::string DiagnosticOf(const Luna::ExecutionResult &Result) {
  return Result.Diagnostic() == nullptr
             ? std::string()
             : std::string(Result.Diagnostic()->Message());
}
void CheckRealVmPipelineAndRecovery() {
  TaggedCalls = 0;
  DerivedCalls = 0;
  AddCalls = 0;

  Luna::State Owner;
  const auto InitialDepth = Hooks::ObserveRootStackDepth(Owner);
  Check(RegisterModel(Owner).IsSuccess(),
        "the representative relationship and operator model registers");
  Check(Hooks::ObserveRootStackDepth(Owner) == InitialDepth,
        "registration restores the root stack exactly");

  DerivedValue Derived;
  Derived.Value = 12;
  Derived.Tag = 9;
  DerivedValue Compatible;
  Compatible.Value = 12;
  PolymorphicBase Plain;
  OperatorValue Operators;
  Expose(Owner, "DerivedObject", &Derived, "Integration.Derived");
  Expose(Owner, "CompatibleBase", static_cast<PolymorphicBase *>(&Compatible),
         "Integration.Base");
  Expose(Owner, "PlainBase", &Plain, "Integration.Base");
  Expose(Owner, "OperatorObject", &Operators, "Integration.OperatorValue");

  const void *TaggedView = static_cast<TaggedBase *>(&Derived);
  Check(TaggedView != static_cast<const void *>(&Derived),
        "the exercised base receiver requires a real pointer adjustment");
  const Luna::ExecutionResult Positive = Owner.Execute(
      "assert(Integration.Tagged.ReadTag(DerivedObject) == 9)\n"
      "assert(Integration.Derived.Specific(CompatibleBase, 3) == 15)\n"
      "assert(OperatorObject + 2 == 6)\n"
      "assert(-OperatorObject == -4)\n"
      "assert(tostring(OperatorObject) == '4')\n");
  Check(Positive.IsSuccess(),
        "adjusted base calls, safe downcasts, and operators execute in Luau");
  Check(TaggedCalls == 1 && DerivedCalls == 1 && AddCalls == 1,
        "each successful native target runs exactly once");
  Check(Hooks::ObserveRootStackDepth(Owner) == InitialDepth,
        "successful inherited and operator calls restore the root stack");

  const std::size_t DerivedBefore = DerivedCalls;
  const Luna::ExecutionResult BadCastFirst =
      Owner.Execute("return Integration.Derived.Specific(PlainBase, 1)");
  const Luna::ExecutionResult BadCastSecond =
      Owner.Execute("return Integration.Derived.Specific(PlainBase, 1)");
  Check(!BadCastFirst.IsSuccess() && !BadCastSecond.IsSuccess(),
        "an incompatible dynamic object is rejected by the safe cast");
  Check(DerivedCalls == DerivedBefore,
        "safe-cast prevalidation rejects before native invocation");
  Check(!DiagnosticOf(BadCastFirst).empty() &&
            DiagnosticOf(BadCastFirst) == DiagnosticOf(BadCastSecond),
        "safe-cast failure diagnostics are deterministic");
  Check(Hooks::ObserveRootStackDepth(Owner) == InitialDepth,
        "safe-cast failures restore the exact root stack");

  const std::size_t AddBefore = AddCalls;
  const Luna::ExecutionResult BadOperatorFirst =
      Owner.Execute("return OperatorObject + 'wrong'");
  const Luna::ExecutionResult BadOperatorSecond =
      Owner.Execute("return OperatorObject + 'wrong'");
  Check(!BadOperatorFirst.IsSuccess() && !BadOperatorSecond.IsSuccess(),
        "an invalid operator operand is rejected");
  Check(AddCalls == AddBefore,
        "operator prevalidation rejects before native invocation");
  Check(!DiagnosticOf(BadOperatorFirst).empty() &&
            DiagnosticOf(BadOperatorFirst) == DiagnosticOf(BadOperatorSecond),
        "operator failure diagnostics are deterministic");
  Check(Hooks::ObserveRootStackDepth(Owner) == InitialDepth,
        "operator failures restore the exact root stack");

  const auto Callback = Hooks::ObserveLastCallbackStackRestoration(Owner);
  Check(Callback && Callback->EntryDepth == Callback->RestoredDepth,
        "the failing operator restores its callback checkpoint exactly");

  const Luna::ExecutionResult Reused = Owner.Execute(
      "assert(Integration.Tagged.ReadTag(DerivedObject) == 9)\n"
      "assert(Integration.Derived.Specific(CompatibleBase, 1) == 13)\n"
      "assert(OperatorObject + 5 == 9)\n");
  Check(Owner.IsReady() && Reused.IsSuccess(),
        "the State remains reusable after cast and operator failures");
  Check(TaggedCalls == 2 && DerivedCalls == 2 && AddCalls == 2,
        "recovered native targets still run exactly once");
  Check(Hooks::ObserveRootStackDepth(Owner) == InitialDepth,
        "State reuse also restores the root stack exactly");
}

} // namespace

int RunInheritanceOperatorIntegrationTests() {
  FailureCount = 0;
  CheckRealVmPipelineAndRecovery();
  return FailureCount == 0 ? 0 : 1;
}
