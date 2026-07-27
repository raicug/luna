// clang-format off
#include <luna/binding/argument_pack.hpp>
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_operator.hpp>
#include <luna/binding/enum_builder.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/binding/parameter_descriptor.hpp>
#include <luna/generation/declaration.hpp>
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace {

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "declaration generation check failed: " << Description << '\n';
}

[[nodiscard]] bool Contains(const std::string &Text, std::string_view Needle) {
  return Text.find(Needle) != std::string::npos;
}

enum class Palette : int { Red = 1, Green = 2 };

struct Shape {
  int Sides = 3;

  [[nodiscard]] int Corners() const { return Sides; }
};

struct Gadget final : Shape {
  int Charge = 3;

  [[nodiscard]] int Level() const { return Charge * 2; }
  [[nodiscard]] int Length() const { return Charge; }
};

[[nodiscard]] int Trim(int Value, std::optional<int> Limit) {
  return Limit ? (Value < *Limit ? Value : *Limit) : Value;
}

[[nodiscard]] int Offset(int Value, int Amount) { return Value + Amount; }

[[nodiscard]] int Total(Luna::ArgumentView Arguments) {
  return static_cast<int>(Arguments.Size());
}

[[nodiscard]] std::pair<int, std::string> Split(std::string Text) {
  return {static_cast<int>(Text.size()), std::move(Text)};
}

[[nodiscard]] Luna::StableTypeKey ShapeKey() {
  return Luna::StableTypeKey("Studio.DeclaredShape");
}

[[nodiscard]] Luna::StableTypeKey GadgetKey() {
  return Luna::StableTypeKey("Studio.DeclaredGadget");
}

[[nodiscard]] Luna::StableTypeKey PaletteKey() {
  return Luna::StableTypeKey("Studio.DeclaredPalette");
}

[[nodiscard]] Luna::ModuleManifest Manifest(std::string Identity,
                                            std::string_view VersionText) {
  const auto Version = Luna::SemanticVersion::TryParse(VersionText);
  auto Created = Luna::ModuleManifest::TryCreate(
      std::move(Identity), Version ? *Version : Luna::SemanticVersion(), {},
      "The declared module.", {});
  return Created ? std::move(*Created) : Luna::ModuleManifest();
}

void RegisterSurface(Luna::BindingRegistry &Registry, bool Reversed) {
  const auto RegisterModule = [&Registry] {
    return Registry.RegisterModule(
        Manifest("studio.physics", "2.1.0"),
        [](Luna::NamespaceBuilder &Builder) {
          Luna::NamespaceBuilder Physics = Builder.RegisterNamespace("Physics");
          static_cast<void>(Physics.RegisterConstant("Gravity", 10));
          static_cast<void>(Physics.RegisterFunction(
              "Scale", [](int Value) { return Value * 2; }));
          static_cast<void>(Physics.RegisterFunction(
              "Blend", [](int Left, int Right) { return Left + Right; }));
          static_cast<void>(Physics.RegisterFunction(
              "Blend", [](std::string Text) { return Text; }));
          static_cast<void>(Physics.RegisterFunction("Reset", [] {}));
          static_cast<void>(Physics.RegisterFunction("Trim", &Trim));
          static_cast<void>(Physics.RegisterFunction(
              "Offset", Luna::WithDefaults(&Offset, 5)));
          static_cast<void>(Physics.RegisterFunction("Total", &Total));
          static_cast<void>(Physics.RegisterFunction("Split", &Split));
          static_cast<void>(Physics.Documentation("Physics helpers.")
                                .Documentation("Scale", "Doubles one value."));
        });
  };

  const auto RegisterStudio = [&Registry] {
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    Luna::ClassBuilder<Shape> Base =
        Studio.RegisterClass<Shape>("Shape", ShapeKey());
    static_cast<void>(
        Base.Constructor<>().Property("Corners", &Shape::Corners));
    Luna::ClassBuilder<Gadget> Class =
        Studio.RegisterClass<Gadget>("Gadget", GadgetKey());
    static_cast<void>(
        Class.Constructor<>()
            .Base<Shape>(ShapeKey())
            .Property("Level", &Gadget::Level)
            .Field("Charge", &Gadget::Charge)
            .Operator(Luna::ClassOperator::Length, &Gadget::Length)
            .Documentation("One declared gadget."));
    Luna::EnumBuilder<Palette> Colours =
        Studio.RegisterEnum<Palette>("Palette", PaletteKey());
    static_cast<void>(Colours.Value("Red", Palette::Red)
                          .Value("Green", Palette::Green)
                          .Alias("Primary", "Red"));
    return Studio.Commit();
  };

  const auto RegisterRoot = [&Registry] {
    Check(
        Registry
            .RegisterFunction("Describe", [](std::string Text) { return Text; })
            .IsSuccess(),
        "one root function commits");
    Check(Registry.RegisterConstant("Version", "1.0").IsSuccess(),
          "one root constant commits");
  };

  if (Reversed) {
    RegisterRoot();
    Check(RegisterStudio().IsSuccess(), "the class plan commits");
    Check(RegisterModule().IsSuccess(), "the module plan commits");
    return;
  }
  Check(RegisterModule().IsSuccess(), "the module plan commits");
  Check(RegisterStudio().IsSuccess(), "the class plan commits");
  RegisterRoot();
}

[[nodiscard]] Luna::ReflectionSnapshot Surface(Luna::State &Owner,
                                               bool Reversed) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  RegisterSurface(Registry, Reversed);
  return Registry.Reflection();
}

void CheckRepeatedGenerationIsByteIdenticalAndCanonical() {
  Luna::State Owner;
  const Luna::ReflectionSnapshot Snapshot = Surface(Owner, false);
  const Luna::DeclarationOptions Options;

  const Luna::GeneratedArtifact First =
      Luna::GenerateDeclarations(Snapshot, Options);
  const Luna::GeneratedArtifact Second =
      Luna::GenerateDeclarations(Snapshot, Options);

  Check(First.IsComplete() && First.Status() == Luna::GenerationStatus::Valid &&
            First.Diagnostic() == nullptr,
        "a complete artifact reports no diagnostic");
  Check(!First.Bytes().empty() && First.Size() == First.Bytes().size(),
        "the artifact owns its whole byte buffer");
  Check(First.Bytes() == Second.Bytes(),
        "repeated generation from equal content and options is byte-identical");

  Check(First.Bytes().find('\r') == std::string::npos,
        "generated bytes use LF line endings only");
  Check(!First.Bytes().starts_with("\xef\xbb\xbf") &&
            First.Bytes().find("\xef\xbb\xbf") == std::string::npos,
        "generated bytes carry no byte-order mark");
  Check(First.Bytes().starts_with("--!strict\n"),
        "the artifact opens with its canonical mode line");
  Check(First.Bytes().back() == '\n',
        "every generated line, including the last, is LF terminated");
}

void CheckGeneratedOrderIgnoresRegistrationOrder() {
  Luna::State Forward;
  Luna::State Backward;
  const Luna::ReflectionSnapshot Ordered = Surface(Forward, false);
  const Luna::ReflectionSnapshot Permuted = Surface(Backward, true);
  const Luna::DeclarationOptions Options;

  const Luna::GeneratedArtifact FromOrdered =
      Luna::GenerateDeclarations(Ordered, Options);
  const Luna::GeneratedArtifact FromPermuted =
      Luna::GenerateDeclarations(Permuted, Options);
  Check(FromOrdered.IsComplete() && FromPermuted.IsComplete(),
        "both permutations generate complete artifacts");
  Check(FromOrdered.Bytes() == FromPermuted.Bytes(),
        "generated declarations are independent of registration order");
}

void CheckGeneratedContentMapsTheReflectedSurface() {
  Luna::State Owner;
  const Luna::ReflectionSnapshot Snapshot = Surface(Owner, false);
  const Luna::GeneratedArtifact Artifact =
      Luna::GenerateDeclarations(Snapshot, Luna::DeclarationOptions());
  Check(Artifact.IsComplete(), "the declared surface generates completely");
  const std::string &Text = Artifact.Bytes();

  Check(Contains(Text, "-- Module: studio.physics@2.1.0\n"),
        "the header names every module of the captured generation");
  Check(Contains(Text, "-- studio.physics@2.1.0\n"),
        "a declaration a module contributed names its provenance");

  Check(Contains(Text, "declare class Studio_Shape\n"),
        "a class is declared as one Luau class type");
  Check(Text.find("declare class Studio_Shape") <
            Text.find("declare class Studio_Gadget"),
        "a base class is declared before the class that extends it");
  Check(Contains(Text, "declare class Studio_Gadget extends Studio_Shape"),
        "an inherited base is declared as Luau inheritance");
  Check(Contains(Text, "  Charge: number\n") &&
            Contains(Text, "  read Level: number\n") &&
            Contains(Text, "  read Corners: number\n"),
        "a field declares both directions and a read-only property declares "
        "only its own");
  Check(Contains(Text, "  function __len(self: Studio_Gadget): number\n"),
        "an operator is declared as the metamethod it answers");
  Check(Contains(Text, "    New: () -> Studio_Gadget,\n"),
        "a construction candidate is declared on the class table");

  Check(Contains(Text, "declare Physics: {") &&
            Contains(Text, "declare Studio: {"),
        "a namespace is declared as one Luau table type");
  Check(Contains(Text, "  Gravity: number,") &&
            Contains(Text, "declare Version: string\n"),
        "a constant is declared with its canonical Luau type");
  Check(Contains(Text, "  Scale: (number) -> number,"),
        "one candidate of a name is declared as one function type");
  Check(
      Contains(Text,
               "  Blend: ((number, number) -> number) & ((string) -> string),"),
      "several candidates of one name are declared as their intersection");
  Check(Contains(Text, "  Reset: () -> (),"),
        "a callable that publishes no value declares the empty return list");
  Check(Contains(Text, "  Split: (string) -> (number, string),"),
        "an ordered return pack declares its multiple values");
  Check(Contains(Text, "  Trim: (number, number?) -> number,") &&
            Contains(Text, "  Offset: (number, number?) -> number,"),
        "an optional and a defaulted parameter both declare an omittable type");
  Check(Contains(Text, "  Total: (...any) -> number,"),
        "a variadic parameter declares an unbounded argument list");
  Check(Contains(Text, "declare function Describe(Argument1: string): string"),
        "a root-scope function declares its named parameters");
  Check(Contains(Text, "  Palette: {") && Contains(Text, "    Red: number,") &&
            Contains(Text, "    Primary: number,"),
        "an enumeration declares its enumerators and its aliases");
  Check(Contains(Text, "-- One declared gadget.\n") &&
            Contains(Text, "-- Physics helpers.\n"),
        "declared prose reaches the artifact as Luau comments");
}

void CheckGenerationReadsOnlyTheCapturedSnapshot() {
  const Luna::DeclarationOptions Options;
  std::string Captured;
  std::string Later;
  {
    auto Owner = std::make_unique<Luna::State>();
    Luna::BindingRegistry Registry = Owner->Bindings();
    RegisterSurface(Registry, false);
    const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
    Captured = Luna::GenerateDeclarations(Snapshot, Options).Bytes();

    Check(Registry.Register("Extra", [] { return 1; }).IsSuccess(),
          "a later registration commits after the snapshot was captured");
    Later = Luna::GenerateDeclarations(Registry.Reflection(), Options).Bytes();
    Check(!Contains(Captured, "declare function Extra"),
          "the captured generation never declares the later symbol");
    Check(Contains(Later, "declare function Extra(): number"),
          "a newly captured generation does declare it");

    Owner.reset();
    Check(Luna::GenerateDeclarations(Snapshot, Options).Bytes() == Captured,
          "generation from a retained snapshot survives State destruction");
  }
  Check(!Captured.empty() && Captured != Later,
        "the two captured generations declare different surfaces");
}

void CheckOptionsSelectTheArtifactDeterministically() {
  Luna::State Owner;
  const Luna::ReflectionSnapshot Snapshot = Surface(Owner, false);

  const Luna::DeclarationOptions Full;
  const Luna::DeclarationOptions Trimmed = Luna::DeclarationOptions::Create(
      "Studio declarations.", false, false, false);
  Check(Trimmed.Banner() == "Studio declarations." &&
            !Trimmed.IncludesStrictMode() && !Trimmed.IncludesProvenance() &&
            !Trimmed.IncludesDocumentation(),
        "created options report exactly what they were given");
  Check(Full == Luna::DeclarationOptions() && !(Full == Trimmed) &&
            Full == Trimmed.WithBanner(std::string())
                        .WithStrictMode(true)
                        .WithProvenance(true)
                        .WithDocumentation(true),
        "options compare by value and every wither leaves the source alone");

  const std::string FullText =
      Luna::GenerateDeclarations(Snapshot, Full).Bytes();
  const std::string TrimmedText =
      Luna::GenerateDeclarations(Snapshot, Trimmed).Bytes();
  Check(TrimmedText.starts_with("-- Studio declarations.\n"),
        "the stated banner opens the artifact when no mode line is included");
  Check(Contains(FullText, "-- Module: studio.physics@2.1.0\n") &&
            !Contains(TrimmedText, "-- Module: "),
        "module provenance appears only when the options include it");
  Check(Contains(FullText, "-- One declared gadget.\n") &&
            !Contains(TrimmedText, "-- One declared gadget."),
        "documentation appears only when the options include it");
  Check(Luna::GenerateDeclarations(Snapshot, Trimmed).Bytes() == TrimmedText,
        "repeated generation with the same options stays byte-identical");
}

void CheckUnrepresentableMetadataIsRejectedWithoutPartialBytes() {
  Luna::State Reserved;
  {
    Luna::BindingRegistry Registry = Reserved.Bindings();
    Check(Registry.RegisterFunction("end", [] { return 1; }).IsSuccess(),
          "a Luau keyword is a valid Luna declaration name");
    const Luna::GeneratedArtifact Artifact = Luna::GenerateDeclarations(
        Registry.Reflection(), Luna::DeclarationOptions());
    Check(!Artifact.IsComplete() &&
              Artifact.Status() ==
                  Luna::GenerationStatus::UnsupportedDeclaration,
          "a name Luau reserves has no representable declaration");
    Check(Artifact.Bytes().empty() && Artifact.Size() == 0,
          "a rejected attempt exposes no partial artifact");
    Check(Artifact.Diagnostic() != nullptr &&
              Contains(Artifact.Diagnostic()->Message(), "end") &&
              Contains(Artifact.Diagnostic()->Message(),
                       "unsupported-declaration"),
          "the rejection names the declaration and the deterministic reason");
  }

  Luna::State Colliding;
  {
    Luna::BindingRegistry Registry = Colliding.Bindings();
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    static_cast<void>(Studio.RegisterClass<Gadget>("Gadget", GadgetKey()));
    Check(Studio.Commit().IsSuccess(), "the nested class commits");
    Luna::ClassBuilder<Shape> Flattened =
        Registry.RegisterClass<Shape>("Studio_Gadget", ShapeKey());
    Check(Flattened.Commit().IsSuccess(),
          "one root class may be named exactly as another class flattens to");
    const Luna::GeneratedArtifact Artifact = Luna::GenerateDeclarations(
        Registry.Reflection(), Luna::DeclarationOptions());
    Check(!Artifact.IsComplete() &&
              Artifact.Status() ==
                  Luna::GenerationStatus::InconsistentMetadata &&
              Artifact.Bytes().empty(),
          "two classes that claim one Luau type name are a contradiction");
  }

  const auto Documented = [](std::string_view Documentation,
                             bool IncludeDocumentation) {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    static_cast<void>(
        Studio.RegisterFunction("Double", [](int Value) { return Value * 2; }));
    static_cast<void>(Studio.Documentation("Double", Documentation));
    Check(Studio.Commit().IsSuccess(),
          "documentation text is metadata, so the plan still commits");
    return Luna::GenerateDeclarations(
        Registry.Reflection(),
        Luna::DeclarationOptions().WithDocumentation(IncludeDocumentation));
  };

  const Luna::GeneratedArtifact Malformed = Documented("bad \xff byte", true);
  Check(!Malformed.IsComplete() &&
            Malformed.Status() == Luna::GenerationStatus::InvalidEncoding &&
            Malformed.Bytes().empty(),
        "text that is not canonical UTF-8 rejects generation");
  Check(Documented("bad \xff byte", false).IsComplete(),
        "metadata the options exclude cannot reject the artifact");
  Check(Documented("plain text", true).IsComplete(),
        "encodable metadata still generates completely");
}

void CheckEmptySnapshotGeneratesACompleteArtifact() {
  const Luna::ReflectionSnapshot Empty;
  const Luna::GeneratedArtifact Artifact =
      Luna::GenerateDeclarations(Empty, Luna::DeclarationOptions());
  Check(Artifact.IsComplete() && !Artifact.Bytes().empty(),
        "an empty generation produces a complete artifact");
  Check(Contains(Artifact.Bytes(), "--!strict\n") &&
            Contains(Artifact.Bytes(),
                     "-- No module contributed a declaration.\n"),
        "the empty artifact still states its mode line and its provenance");
}

} // namespace

int RunDeclarationGenerationTests() {
  FailureCount = 0;
  CheckRepeatedGenerationIsByteIdenticalAndCanonical();
  CheckGeneratedOrderIgnoresRegistrationOrder();
  CheckGeneratedContentMapsTheReflectedSurface();
  CheckGenerationReadsOnlyTheCapturedSnapshot();
  CheckOptionsSelectTheArtifactDeterministically();
  CheckUnrepresentableMetadataIsRejectedWithoutPartialBytes();
  CheckEmptySnapshotGeneratesACompleteArtifact();
  return FailureCount == 0 ? 0 : 1;
}
