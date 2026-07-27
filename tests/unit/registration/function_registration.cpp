// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/binding/overload.hpp>
#include <luna/binding/supported_callable.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/core/results/execution_result.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>

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
  std::cerr << "function registration check failed: " << Description << '\n';
}

[[nodiscard]] int AddIntegers(int Left, int Right) { return Left + Right; }

[[nodiscard]] int Measure(int Value) { return Value * 2; }
[[nodiscard]] int Measure(int Value, int Scale) { return Value * Scale; }
[[nodiscard]] double Measure(double Value) { return Value * 0.5; }

struct Scaling final {
  [[nodiscard]] double operator()(double Value) const { return Value * 2.0; }
  [[nodiscard]] int operator()(int Value) const { return Value * 3; }
};

struct Counter final {
  int Total = 0;
  void Add(int Amount) { Total += Amount; }
};

[[nodiscard]] std::string_view Trim(std::string_view Text) { return Text; }

[[nodiscard]] int StackDepth(const Luna::State &Owner) {
  const auto Depth = Hooks::ObserveRootStackDepth(Owner);
  return Depth ? *Depth : -1;
}

[[nodiscard]] std::string PathKind(Luna::State &Owner,
                                   const std::string &Path) {
  const auto Kind = Hooks::ObserveVmPathValueKind(Owner, Path);
  return Kind ? *Kind : std::string("<unavailable>");
}

[[nodiscard]] std::string
FailureMessage(const Luna::RegistrationResult &Result) {
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  return Diagnostic ? Diagnostic->Message() : std::string();
}

[[nodiscard]] bool HasCategory(const Luna::RegistrationResult &Result,
                               Luna::ErrorCategory Category) {
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  return Diagnostic != nullptr && Diagnostic->Category() == Category;
}

[[nodiscard]] bool ExecutionSucceeds(Luna::State &Owner,
                                     std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "function registration source failed: "
              << Diagnostic->Message() << '\n';
  return false;
}

static_assert(
    Luna::SupportedCallable<decltype(Luna::Overload<int(int)>(&Measure))>,
    "an overload selection with a supported signature is registrable");
static_assert(
    Luna::SupportedCallable<decltype(Luna::Overload<int(int, int)>(&Measure))>,
    "each overload of one C++ name is selectable by its own signature");
static_assert(!Luna::SupportedCallable<Scaling>,
              "a callable object with several signatures is ambiguous");
static_assert(
    Luna::SupportedCallable<
        decltype(Luna::Overload<double(double)>(Scaling()))>,
    "a callable object invocable with exactly the signature is registrable");
static_assert(
    !Luna::SupportedCallable<
        decltype(Luna::Overload<std::string_view(std::string_view)>(&Trim))>,
    "an overload selection never weakens the converter availability checks");
static_assert(
    !Luna::SupportedCallable<
        decltype(Luna::Overload<void(int), Counter>(&Counter::Add))>,
    "a member wrapper declares its receiver, which classes publish later");

void CheckRootFunctionsRegisterThroughBothSpellings() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  const auto Explicit = Registry.RegisterFunction("Add", &AddIntegers);
  Check(Explicit.IsSuccess(), "RegisterFunction accepts a function pointer");

  const auto Alias = Registry.Register(
      "Sub", [](int Left, int Right) { return Left - Right; });
  Check(Alias.IsSuccess(), "Register still accepts a concrete lambda");

  const auto Selected = Registry.RegisterFunction(
      "Scale", Luna::Overload<double(double)>(Scaling()));
  Check(Selected.IsSuccess(),
        "an explicit overload selection registers an ambiguous functor");

  const auto Free = Registry.RegisterFunction(
      "Half", Luna::Overload<double(double)>(&Measure));
  Check(Free.IsSuccess(),
        "an explicit overload selection registers one overloaded C++ name");

  Check(Hooks::BindingCount(Owner) == 4,
        "every accepted callable form stages exactly one binding");
  Check(Hooks::BindingIsCommitted(Owner, "Add") &&
            Hooks::BindingIsCommitted(Owner, "Scale"),
        "a published root callable is committed under its global name");
  Check(StackDepth(Owner) == EntryDepth,
        "root function registration restores the exact entry stack depth");

  Check(ExecutionSucceeds(Owner, "assert(Add(2, 3) == 5)\n"
                                 "assert(Sub(5, 3) == 2)\n"
                                 "assert(Scale(2.0) == 4.0)\n"
                                 "assert(Half(4.0) == 2.0)\n"),
        "every registered root callable is invocable from Luau");
}

void CheckAliasReportsIdenticalDiagnostics() {
  const auto DuplicateThrough = [](bool ExplicitFirst) {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    if (ExplicitFirst) {
      static_cast<void>(Registry.RegisterFunction("Add", &AddIntegers));
      return Registry.Register("Add", &AddIntegers);
    }
    static_cast<void>(Registry.Register("Add", &AddIntegers));
    return Registry.RegisterFunction("Add", &AddIntegers);
  };

  const auto First = DuplicateThrough(true);
  const auto Second = DuplicateThrough(false);
  Check(!First.IsSuccess() && !Second.IsSuccess(),
        "a duplicate root callable is refused through both spellings");
  Check(HasCategory(First, Luna::ErrorCategory::DuplicateGlobalName) &&
            HasCategory(Second, Luna::ErrorCategory::DuplicateGlobalName),
        "a duplicate keeps the foundation's diagnostic category");
  Check(FailureMessage(First) == FailureMessage(Second),
        "both spellings report one identical duplicate diagnostic");

  const auto InvalidThroughRegister = [] {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    return Registry.Register("1Bad", &AddIntegers);
  }();
  const auto InvalidThroughFunction = [] {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    return Registry.RegisterFunction("1Bad", &AddIntegers);
  }();
  Check(!InvalidThroughRegister.IsSuccess() &&
            !InvalidThroughFunction.IsSuccess(),
        "an invalid identifier is refused through both spellings");
  Check(HasCategory(InvalidThroughRegister,
                    Luna::ErrorCategory::InvalidGlobalName),
        "an invalid identifier keeps the foundation's diagnostic category");
  Check(FailureMessage(InvalidThroughRegister) ==
            FailureMessage(InvalidThroughFunction),
        "both spellings keep one identical identifier diagnostic");
}

void CheckScopedFunctionsPublishWithTheirNamespace() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::NamespaceBuilder &StagedStudio =
      Studio.RegisterFunction("Add", &AddIntegers);
  static_cast<void>(StagedStudio.RegisterFunction(
      "Double", Luna::Overload<int(int)>(&Measure)));

  Luna::NamespaceBuilder Math = Studio.RegisterNamespace("Math");
  static_cast<void>(
      Math.RegisterFunction("Scale", Luna::Overload<int(int, int)>(&Measure)));

  Check(PathKind(Owner, "Studio.Add") == "absent",
        "a staged scoped callable is invisible before its plan commits");
  Check(Hooks::BindingCount(Owner) == 0,
        "a staged scoped callable stages no binding record before commit");

  const auto Result = Studio.Commit();
  Check(Result.IsSuccess(), "one plan publishes namespaces and functions");
  Check(PathKind(Owner, "Studio.Add") == "function" &&
            PathKind(Owner, "Studio.Math.Scale") == "function",
        "a published scoped callable holds one closure at its exact path");
  Check(Hooks::BindingIsCommitted(Owner, "Studio.Add") &&
            Hooks::BindingIsCommitted(Owner, "Studio.Math.Scale"),
        "a published scoped callable is committed under its qualified name");
  Check(StackDepth(Owner) == EntryDepth,
        "publishing scoped callables restores the exact entry stack depth");

  Check(ExecutionSucceeds(Owner, "assert(Studio.Add(4, 5) == 9)\n"
                                 "assert(Studio.Double(4) == 8)\n"
                                 "assert(Studio.Math.Scale(4, 3) == 12)\n"),
        "every published scoped callable is invocable from Luau");

  const Luna::ExecutionResult Refused = Owner.Execute("return Studio.Add(1)");
  Check(!Refused.IsSuccess(),
        "a scoped callable keeps the foundation's argument validation");
  Check(ExecutionSucceeds(Owner, "assert(Studio.Add(1, 1) == 2)"),
        "the State stays usable after a refused scoped invocation");
  Check(StackDepth(Owner) == EntryDepth,
        "a refused scoped invocation restores the exact entry stack depth");
}

void CheckUncommittedScopedFunctionHasNoEffect() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  const int EntryDepth = StackDepth(Owner);

  {
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    static_cast<void>(Studio.RegisterFunction("Add", &AddIntegers));
  }

  Check(PathKind(Owner, "Studio") == "absent",
        "destroying an uncommitted builder installs no namespace");
  Check(Hooks::BindingCount(Owner) == 0,
        "destroying an uncommitted builder stages no binding record");
  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  Check(Snapshot.IsEmpty(),
        "destroying an uncommitted builder publishes no reflection record");
  Check(StackDepth(Owner) == EntryDepth,
        "destroying an uncommitted builder leaves the stack untouched");
}

void CheckDuplicateScopedFunctionPublishesNothing() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  static_cast<void>(Studio.RegisterFunction("Add", &AddIntegers));
  static_cast<void>(Studio.RegisterFunction("Add", &AddIntegers));

  const auto Result = Studio.Commit();
  Check(!Result.IsSuccess(), "one plan cannot declare one name twice");
  Check(HasCategory(Result, Luna::ErrorCategory::DuplicateGlobalName),
        "a duplicate scoped callable keeps the shared duplicate category");
  Check(FailureMessage(Result).find("Studio.Add") != std::string::npos,
        "the diagnostic names the canonical qualified name");
  Check(PathKind(Owner, "Studio") == "absent" &&
            Hooks::BindingCount(Owner) == 0,
        "a refused plan publishes neither the namespace nor the callable");

  const auto Retry = Registry.RegisterFunction("Add", &AddIntegers);
  Check(Retry.IsSuccess(), "the State stays reusable after a refused plan");
  Check(ExecutionSucceeds(Owner, "assert(Add(1, 2) == 3)"),
        "the callable registered after a refused plan is invocable");
}

void CheckScopedFunctionRequiresItsOwnScope() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  static_cast<void>(Registry.RegisterFunction("Studio", &AddIntegers));

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  static_cast<void>(Studio.RegisterFunction("Add", &AddIntegers));
  const auto Result = Studio.Commit();
  Check(!Result.IsSuccess(),
        "a namespace never adopts a path a callable already owns");
  Check(PathKind(Owner, "Studio") == "function",
        "the committed callable keeps its published path");
  Check(ExecutionSucceeds(Owner, "assert(Studio(1, 2) == 3)"),
        "the committed callable stays invocable after the refused plan");
}

} // namespace

int RunFunctionRegistrationTests() {
  FailureCount = 0;
  CheckRootFunctionsRegisterThroughBothSpellings();
  CheckAliasReportsIdenticalDiagnostics();
  CheckScopedFunctionsPublishWithTheirNamespace();
  CheckUncommittedScopedFunctionHasNoEffect();
  CheckDuplicateScopedFunctionPublishesNothing();
  CheckScopedFunctionRequiresItsOwnScope();
  return FailureCount == 0 ? 0 : 1;
}
