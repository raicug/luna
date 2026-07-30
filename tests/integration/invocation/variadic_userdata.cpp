// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <cstddef>
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
  std::cerr << "variadic userdata check failed: " << Description << '\n';
}

struct Marker final {
  int Tag = 0;
};

int DirectCalls = 0;
int NestedCalls = 0;

void ResetCounters() {
  DirectCalls = 0;
  NestedCalls = 0;
}

[[nodiscard]] std::size_t CountUserdataIn(const Luna::OwnedValue &Element) {
  if (Element.IsUserdata())
    return 1;
  if (!Element.IsTable())
    return 0;
  std::size_t Found = 0;
  for (std::size_t Index = 0; Index < Element.Size(); ++Index)
    Found += CountUserdataIn(Element.Element(Index));
  for (std::size_t Index = 0; Index < Element.FieldCount(); ++Index)
    Found += CountUserdataIn(Element.Field(Element.FieldName(Index)));
  return Found;
}

// Receives a class instance directly as one of its variadic elements.
[[nodiscard]] int CountUserdata(Luna::ArgumentView Arguments) {
  ++DirectCalls;
  int Found = 0;
  for (std::size_t Index = 0; Index < Arguments.Size(); ++Index) {
    if (Arguments.Kind(Index) == Luna::ValueCategory::Userdata)
      ++Found;
  }
  return Found;
}

// Receives a table that itself contains a class instance, still through the
// same variadic parameter.
[[nodiscard]] int CountNestedUserdata(Luna::ArgumentPack Arguments) {
  ++NestedCalls;
  int Found = 0;
  for (std::size_t Index = 0; Index < Arguments.Size(); ++Index)
    Found += static_cast<int>(CountUserdataIn(Arguments.At(Index)));
  return Found;
}

[[nodiscard]] bool RegisterModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Marker> Class = Studio.RegisterClass<Marker>(
      "Marker", Luna::StableTypeKey("Studio.VariadicMarker"));
  Luna::ClassBuilder<Marker> &WithConstructor = Class.Constructor<>();
  static_cast<void>(WithConstructor.QualifiedName());

  Luna::NamespaceBuilder &Staged =
      Studio.RegisterFunction("CountUserdata", &CountUserdata)
          .RegisterFunction("CountNestedUserdata", &CountNestedUserdata);
  static_cast<void>(Staged.QualifiedName());
  return Studio.Commit().IsSuccess();
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "variadic userdata source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

[[nodiscard]] int ScriptResult(Luna::State &Owner, std::string_view Source) {
  int Value = 0;
  Check(Succeeds(Owner, Source), "the source executes");
  Check(Hooks::ObserveIntegerGlobal(Owner, "Result").has_value(),
        "the source publishes a Result global");
  Value = Hooks::ObserveIntegerGlobal(Owner, "Result").value_or(0);
  return Value;
}

void CheckVariadicReceivesUserdataDirectly() {
  ResetCounters();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "M1 = Studio.Marker.New()\nM2 = Studio.Marker.New()"),
        "two marker instances construct");
  Check(ScriptResult(Owner, "Result = Studio.CountUserdata(1, M1, 'x', M2)") ==
            2,
        "a variadic function receives a userdata argument directly, "
        "without being rejected");
  Check(DirectCalls == 1, "the variadic call invokes native code exactly once");

  Check(ScriptResult(Owner, "Result = Studio.CountUserdata(1, 2, 3)") == 0,
        "a variadic call with no userdata elements still succeeds");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every variadic call restores the entry stack depth");
}

void CheckVariadicReceivesTableContainingUserdata() {
  ResetCounters();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "M1 = Studio.Marker.New()\nM2 = Studio.Marker.New()"),
        "two marker instances construct");
  Check(ScriptResult(Owner,
                     "Result = Studio.CountNestedUserdata({M1, 2}, {Owner "
                     "= M2}, 'plain')") == 2,
        "a variadic function receives a table containing a userdata value, "
        "and the nested value round-trips through the same Userdata "
        "category");
  Check(NestedCalls == 1, "the variadic call invokes native code exactly once");

  Check(ScriptResult(Owner, "Result = Studio.CountNestedUserdata({1, 2}, "
                            "{3, 4})") == 0,
        "a table with no nested userdata still succeeds");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every variadic call restores the entry stack depth");
}

} // namespace

int RunVariadicUserdataTests();

int RunVariadicUserdataTests() {
  FailureCount = 0;
  CheckVariadicReceivesUserdataDirectly();
  CheckVariadicReceivesTableContainingUserdata();
  return FailureCount == 0 ? 0 : 1;
}
