// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_operator.hpp>
#include <luna/binding/enum_builder.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "reflection metadata check failed: " << Description << '\n';
}

[[nodiscard]] Luna::ReflectionRecord
FindOfKind(const Luna::ReflectionSnapshot &Snapshot, Luna::SymbolKind Kind,
           std::string_view QualifiedName) {
  const Luna::ReflectionRecordRange Range = Snapshot.Symbols(Kind);
  for (std::size_t Index = 0; Index < Range.Size(); ++Index) {
    const Luna::ReflectionRecord Record = Range.At(Index);
    if (Record.QualifiedName() == QualifiedName)
      return Record;
  }
  return Luna::ReflectionRecord();
}

[[nodiscard]] bool HasAttribute(const Luna::ReflectionRecord &Record,
                                std::string_view Name, std::string_view Value) {
  for (std::size_t Index = 0; Index < Record.AttributeCount(); ++Index) {
    const Luna::AttributeRecord Attribute = Record.Attribute(Index);
    if (Attribute.Name() == Name && Attribute.Value() == Value)
      return true;
  }
  return false;
}

enum class Palette : int { Red = 1, Green = 2 };

struct Gadget final {
  int Charge = 3;

  [[nodiscard]] int Level() const { return Charge * 2; }
  [[nodiscard]] int Length() const { return Charge; }
};

[[nodiscard]] Luna::SemanticVersion Version(std::string_view Text) {
  const auto Parsed = Luna::SemanticVersion::TryParse(Text);
  return Parsed ? *Parsed : Luna::SemanticVersion();
}

[[nodiscard]] Luna::ModuleManifest Manifest(std::string Identity,
                                            std::string_view VersionText) {
  auto Created = Luna::ModuleManifest::TryCreate(
      std::move(Identity), Version(VersionText), {}, std::string(), {});
  return Created ? std::move(*Created) : Luna::ModuleManifest();
}

void CheckScopeDeclarationsPublishTheirDocumentationSurface() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  static_cast<void>(
      Studio.RegisterFunction("Double", [](int Value) { return Value * 2; }));
  static_cast<void>(Studio.RegisterConstant("Gravity", 10));
  static_cast<void>(Studio.RegisterNamespace("Math"));

  static_cast<void>(
      Studio.Documentation("The Studio surface.")
          .Attribute("Stability", "stable")
          .Example("local Doubled = Studio.Double(21)")
          .Documentation("Double", "Doubles one integer.")
          .Attribute("Double", "Pure", "true")
          .Example("Double", "Studio.Double(21)")
          .Documentation("Gravity", "Gravity in metres per second squared.")
          .Attribute("Gravity", "Unit", "m/s^2")
          .Example("Gravity", "print(Studio.Gravity)")
          .Documentation("Math", "Mathematics helpers."));
  Check(Studio.Commit().IsSuccess(),
        "one plan commits its declarations together with their documentation");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();

  const Luna::ReflectionRecord Scope = Snapshot.Find("Studio");
  Check(Scope.Kind() == Luna::SymbolKind::Namespace &&
            Scope.Documentation() == "The Studio surface.",
        "a namespace publishes its own documentation text");
  Check(HasAttribute(Scope, "Stability", "stable") &&
            Scope.AttributeCount() == 1,
        "a namespace publishes exactly the attributes it declared");
  Check(Scope.ExampleCount() == 1 &&
            Scope.Example(0) == "local Doubled = Studio.Double(21)",
        "a namespace publishes its usage examples in declaration order");
  Check(Scope.Example(1).empty(), "an example index past the end is empty");

  const Luna::ReflectionRecord Candidate = FindOfKind(
      Snapshot, Luna::SymbolKind::FunctionCandidate, "Studio.Double");
  Check(Candidate.IsValid() &&
            Candidate.Documentation() == "Doubles one integer.",
        "a function candidate publishes its documentation text");
  Check(HasAttribute(Candidate, "Pure", "true") &&
            Candidate.ExampleCount() == 1 &&
            Candidate.Example(0) == "Studio.Double(21)",
        "a function candidate publishes its attributes and examples");

  const Luna::ReflectionRecord Set = Snapshot.Find("Studio.Double");
  Check(Set.Kind() == Luna::SymbolKind::OverloadSet &&
            Set.Documentation().empty() && Set.ExampleCount() == 0,
        "an overload set carries no documentation of its candidates");

  const Luna::ReflectionRecord Constant = Snapshot.Find("Studio.Gravity");
  Check(Constant.Kind() == Luna::SymbolKind::Constant &&
            Constant.Documentation() ==
                "Gravity in metres per second squared." &&
            HasAttribute(Constant, "Unit", "m/s^2") &&
            Constant.ExampleCount() == 1 &&
            Constant.Example(0) == "print(Studio.Gravity)",
        "a constant publishes its whole documentation surface");
  Check(Constant.HasValue() && Constant.ValueText() == "10",
        "documenting a constant leaves its canonical value untouched");

  const Luna::ReflectionRecord Nested = Snapshot.Find("Studio.Math");
  Check(Nested.Kind() == Luna::SymbolKind::Namespace &&
            Nested.Documentation() == "Mathematics helpers.",
        "a nested namespace is documented from the scope that declared it");

  Check(Owner
            .Execute("assert(Studio.Double(21) == 42)\n"
                     "assert(Studio.Gravity == 10)")
            .IsSuccess(),
        "annotated declarations remain invocable through the real machine");
}

void CheckUnresolvableAnnotationsPublishNothing() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.Register("Existing", [] { return 3; }).IsSuccess(),
        "a committed callable exists before the failing plans");
  const std::uint64_t Published = Registry.Reflection().Generation();

  {
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    static_cast<void>(
        Studio.RegisterFunction("Double", [](int Value) { return Value * 2; }));
    static_cast<void>(Studio.Documentation("Missing", "nothing declares this"));
    const Luna::RegistrationResult Result = Studio.Commit();
    Check(!Result.IsSuccess(),
          "documenting a declaration the plan never staged fails the commit");
  }
  {
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    static_cast<void>(
        Studio.RegisterFunction("Double", [](int Value) { return Value * 2; }));
    static_cast<void>(Studio.Attribute("Double", std::string_view(), "true"));
    Check(!Studio.Commit().IsSuccess(),
          "an empty attribute name fails the commit");
  }
  {
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    static_cast<void>(Studio.Documentation("Studio."));
    Check(Studio.Commit().IsSuccess(),
          "documenting a staged namespace itself stays available");
  }

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  Check(Snapshot.Find("Studio").Documentation() == "Studio.",
        "only the accepted plan published its documentation");
  Check(Snapshot.Generation() == Published + 1,
        "each rejected plan published no generation at all");
  Check(Owner
            .Execute("assert(Existing() == 3)\n"
                     "assert(Studio.Double == nil)")
            .IsSuccess(),
        "the State stays usable and publishes nothing from a rejected plan");
}

void CheckClassAndEnumerationDocumentationSurface() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Gadget> Class = Studio.RegisterClass<Gadget>(
      "Gadget", Luna::StableTypeKey("Studio.MetadataGadget"));
  static_cast<void>(
      Class.Property("Level", &Gadget::Level)
          .Field("Charge", &Gadget::Charge)
          .Operator(Luna::ClassOperator::Length, &Gadget::Length)
          .Documentation("One documented gadget.")
          .Example("local Value = Gadget.Level")
          .Documentation("Level", "Twice the stored charge.")
          .Attribute("Level", "Access", "read-only")
          .Example("Level", "print(Value.Level)")
          .Documentation("Charge", "The stored charge.")
          .Documentation(Luna::ClassOperator::Length, "The stored charge.")
          .Attribute(Luna::ClassOperator::Length, "Cost", "constant")
          .Example(Luna::ClassOperator::Length, "print(#Value)"));

  Luna::EnumBuilder<Palette> Colours = Studio.RegisterEnum<Palette>(
      "Palette", Luna::StableTypeKey("Studio.MetadataPalette"));
  static_cast<void>(Colours.Value("Red", Palette::Red)
                        .Value("Green", Palette::Green)
                        .Alias("Primary", "Red")
                        .Documentation("The palette.")
                        .Example("print(Studio.Palette.Red)")
                        .Documentation("Red", "The red enumerator.")
                        .Example("Red", "print(Studio.Palette.Red)")
                        .Example("Primary", "print(Studio.Palette.Primary)"));

  Check(Studio.Commit().IsSuccess(),
        "a class, its members, its operator, and an enumeration commit "
        "together with their documentation");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();

  const Luna::ReflectionRecord GadgetRecord = Snapshot.Find("Studio.Gadget");
  Check(GadgetRecord.Kind() == Luna::SymbolKind::Class &&
            GadgetRecord.Documentation() == "One documented gadget." &&
            GadgetRecord.ExampleCount() == 1 &&
            GadgetRecord.Example(0) == "local Value = Gadget.Level",
        "a class publishes its documentation text and examples");

  const Luna::ReflectionRecord Level = Snapshot.Find("Studio.Gadget.Level");
  Check(Level.Documentation() == "Twice the stored charge." &&
            HasAttribute(Level, "Access", "read-only") &&
            Level.ExampleCount() == 1 &&
            Level.Example(0) == "print(Value.Level)",
        "a property publishes its whole documentation surface");
  Check(Snapshot.Find("Studio.Gadget.Charge").Documentation() ==
            "The stored charge.",
        "a field publishes its documentation text");

  const Luna::ReflectionRecordRange Operators =
      Snapshot.Symbols(Luna::SymbolKind::Operator);
  const Luna::ReflectionRecord OperatorRecord =
      Operators.Size() == 1 ? Operators.At(0) : Luna::ReflectionRecord();
  Check(OperatorRecord.IsValid() &&
            OperatorRecord.Documentation() == "The stored charge." &&
            HasAttribute(OperatorRecord, "Cost", "constant") &&
            OperatorRecord.ExampleCount() == 1 &&
            OperatorRecord.Example(0) == "print(#Value)",
        "an operator publishes its documentation surface, named by the "
        "operator it answers");

  const Luna::ReflectionRecord PaletteRecord = Snapshot.Find("Studio.Palette");
  Check(PaletteRecord.Kind() == Luna::SymbolKind::Enumeration &&
            PaletteRecord.Documentation() == "The palette." &&
            PaletteRecord.ExampleCount() == 1,
        "an enumeration publishes its documentation text and examples");
  const Luna::ReflectionRecord Red = Snapshot.Find("Studio.Palette.Red");
  Check(Red.Kind() == Luna::SymbolKind::Enumerator &&
            Red.Documentation() == "The red enumerator." &&
            Red.ExampleCount() == 1 &&
            Red.Example(0) == "print(Studio.Palette.Red)",
        "an enumerator publishes its documentation text and examples");
  const Luna::ReflectionRecord Alias = Snapshot.Find("Studio.Palette.Primary");
  Check(Alias.Kind() == Luna::SymbolKind::EnumeratorAlias &&
            Alias.ExampleCount() == 1 &&
            Alias.Example(0) == "print(Studio.Palette.Primary)",
        "an enumerator alias publishes its own examples");
}

void CheckModuleProvenanceReachesEveryContributedRecord() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.Register("Local", [] { return 1; }).IsSuccess(),
        "a declaration outside every module exists first");

  const Luna::RegistrationResult Loaded = Registry.RegisterModule(
      Manifest("studio.physics", "2.1.0"), [](Luna::NamespaceBuilder &Builder) {
        Luna::NamespaceBuilder Physics = Builder.RegisterNamespace("Physics");
        static_cast<void>(Physics.RegisterConstant("Gravity", 10));
        static_cast<void>(Physics.RegisterFunction(
            "Scale", [](int Value) { return Value * 2; }));
      });
  Check(Loaded.IsSuccess(), "the module graph loads");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();

  const auto Provenance = [](const Luna::ReflectionRecord &Record) {
    if (!Record.HasModule())
      return std::string();
    const Luna::ModuleRecord Module = Record.Module();
    return std::string(Module.Identity()) + "@" + std::string(Module.Version());
  };

  Check(Provenance(Snapshot.Find("Physics")) == "studio.physics@2.1.0",
        "a namespace a module declared names its module identity and version");
  Check(Provenance(Snapshot.Find("Physics.Gravity")) == "studio.physics@2.1.0",
        "a constant a module declared names its module provenance");
  Check(Provenance(FindOfKind(Snapshot, Luna::SymbolKind::FunctionCandidate,
                              "Physics.Scale")) == "studio.physics@2.1.0",
        "a callable candidate a module declared names its module provenance");
  Check(Provenance(Snapshot.Find("Physics.Scale")) == "studio.physics@2.1.0",
        "the overload set of a module callable names its module provenance");
  Check(Provenance(Snapshot.Find("studio.physics")) == "studio.physics@2.1.0",
        "the module symbol itself names its own identity and version");

  const Luna::ReflectionRecord Outside = Snapshot.Find("Local");
  Check(!Outside.HasModule() && !Outside.Module().IsValid(),
        "a declaration no module load contributed names no provenance");

  const Luna::ReflectionSnapshot Retained = Snapshot;
  Check(Provenance(Retained.Find("Physics.Gravity")) == "studio.physics@2.1.0",
        "module provenance is owned by the captured generation");
}

} // namespace

int RunReflectionMetadataTests() {
  FailureCount = 0;
  CheckScopeDeclarationsPublishTheirDocumentationSurface();
  CheckUnresolvableAnnotationsPublishNothing();
  CheckClassAndEnumerationDocumentationSurface();
  CheckModuleProvenanceReachesEveryContributedRecord();
  return FailureCount == 0 ? 0 : 1;
}
