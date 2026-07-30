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

struct Labelled final {
  int Tag = 0;

  [[nodiscard]] std::string ToText() const {
    return "Labelled(" + std::to_string(Tag) + ")";
  }
};

int DirectCalls = 0;
int NestedCalls = 0;
int DescribeCalls = 0;
std::string ObservedText;
std::string ObservedClassName;

void ResetCounters() {
  DirectCalls = 0;
  NestedCalls = 0;
  DescribeCalls = 0;
  ObservedText.clear();
  ObservedClassName.clear();
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

[[nodiscard]] int CountUserdata(Luna::ArgumentView Arguments) {
  ++DirectCalls;
  int Found = 0;
  for (std::size_t Index = 0; Index < Arguments.Size(); ++Index) {
    if (Arguments.Kind(Index) == Luna::ValueCategory::Userdata)
      ++Found;
  }
  return Found;
}

[[nodiscard]] int CountNestedUserdata(Luna::ArgumentPack Arguments) {
  ++NestedCalls;
  int Found = 0;
  for (std::size_t Index = 0; Index < Arguments.Size(); ++Index)
    Found += static_cast<int>(CountUserdataIn(Arguments.At(Index)));
  return Found;
}

[[nodiscard]] int DescribeFirstUserdata(Luna::ArgumentPack Arguments) {
  ++DescribeCalls;
  ObservedText.clear();
  ObservedClassName.clear();
  for (std::size_t Index = 0; Index < Arguments.Size(); ++Index) {
    const Luna::OwnedValue Element = Arguments.At(Index);
    if (!Element.IsUserdata())
      continue;
    ObservedText = std::string(Element.UserdataText());
    ObservedClassName = std::string(Element.UserdataClassName());
    break;
  }
  return static_cast<int>(Arguments.Size());
}

[[nodiscard]] bool RegisterModel(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Marker> Class = Studio.RegisterClass<Marker>(
      "Marker", Luna::StableTypeKey("Studio.VariadicMarker"));
  Luna::ClassBuilder<Marker> &WithConstructor = Class.Constructor<>();
  static_cast<void>(WithConstructor.QualifiedName());

  Luna::ClassBuilder<Labelled> Rendered = Studio.RegisterClass<Labelled>(
      "Labelled", Luna::StableTypeKey("Studio.VariadicLabelled"));
  Luna::ClassBuilder<Labelled> &WithText =
      Rendered.Constructor<>()
          .Field("Tag", &Labelled::Tag)
          .Operator(Luna::ClassOperator::ToText, &Labelled::ToText);
  static_cast<void>(WithText.QualifiedName());

  Luna::NamespaceBuilder &Staged =
      Studio.RegisterFunction("CountUserdata", &CountUserdata)
          .RegisterFunction("CountNestedUserdata", &CountNestedUserdata)
          .RegisterFunction("DescribeFirstUserdata", &DescribeFirstUserdata);
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

void CheckCapturedUserdataCarriesDisplayText() {
  ResetCounters();
  Luna::State Owner;
  Check(Owner.IsReady() && RegisterModel(Owner), "the model publishes");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "L = Studio.Labelled.New()\nL.Tag = 7"),
        "a labelled instance constructs");
  Check(ScriptResult(Owner, "Result = Studio.DescribeFirstUserdata(L)") == 1,
        "the describing call succeeds");
  Check(DescribeCalls == 1, "the describing call reaches native code once");
  Check(ObservedText == "Labelled(7)",
        "a captured userdata value carries the text its class's ToText "
        "operator renders");
  Check(ObservedClassName.find("Labelled") != std::string::npos,
        "the class name is still present alongside the display text");

  Check(Succeeds(Owner, "M = Studio.Marker.New()"),
        "a marker instance constructs");
  Check(ScriptResult(Owner, "Result = Studio.DescribeFirstUserdata(M)") == 1,
        "the describing call succeeds for a class with no ToText");
  Check(ObservedText.empty(),
        "a class declaring no ToText operator yields empty display text");
  Check(ObservedClassName.find("Marker") != std::string::npos,
        "the class name is present even when there is no display text");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "resolving display text restores the entry stack depth");
}

} // namespace

int RunVariadicUserdataTests();

int RunVariadicUserdataTests() {
  FailureCount = 0;
  CheckVariadicReceivesUserdataDirectly();
  CheckVariadicReceivesTableContainingUserdata();
  CheckCapturedUserdataCarriesDisplayText();
  return FailureCount == 0 ? 0 : 1;
}
