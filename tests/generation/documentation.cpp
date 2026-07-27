// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_operator.hpp>
#include <luna/binding/enum_builder.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/generation/documentation.hpp>
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
  std::cerr << "documentation generation check failed: " << Description << '\n';
}

[[nodiscard]] bool Contains(const std::string &Text, std::string_view Needle) {
  return Text.find(Needle) != std::string::npos;
}

enum class Palette : int { Red = 1, Green = 2 };

struct Gadget final {
  int Charge = 3;

  [[nodiscard]] int Level() const { return Charge * 2; }
  [[nodiscard]] int Length() const { return Charge; }
};

[[nodiscard]] Luna::ModuleManifest Manifest(std::string Identity,
                                            std::string_view VersionText) {
  const auto Version = Luna::SemanticVersion::TryParse(VersionText);
  auto Created = Luna::ModuleManifest::TryCreate(
      std::move(Identity), Version ? *Version : Luna::SemanticVersion(), {},
      "The documented module.", {});
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
          static_cast<void>(
              Physics.Documentation("Physics helpers.")
                  .Attribute("Stability", "stable")
                  .Documentation("Gravity", "Metres per second squared.")
                  .Attribute("Gravity", "Unit", "m/s^2")
                  .Example("Gravity", "print(Physics.Gravity)")
                  .Documentation("Scale", "Doubles one value.")
                  .Example("Scale", "local Scaled = Physics.Scale(21)"));
        });
  };

  const auto RegisterStudio = [&Registry] {
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    Luna::ClassBuilder<Gadget> Class = Studio.RegisterClass<Gadget>(
        "Gadget", Luna::StableTypeKey("Studio.DocumentedGadget"));
    static_cast<void>(
        Class.Constructor<>()
            .Property("Level", &Gadget::Level)
            .Field("Charge", &Gadget::Charge)
            .Operator(Luna::ClassOperator::Length, &Gadget::Length)
            .Documentation("One documented gadget.")
            .Example("local Value = Gadget.new()")
            .Documentation("Level", "Twice the stored charge.")
            .Attribute("Level", "Access", "read-only"));
    Luna::EnumBuilder<Palette> Colours = Studio.RegisterEnum<Palette>(
        "Palette", Luna::StableTypeKey("Studio.DocumentedPalette"));
    static_cast<void>(Colours.Value("Red", Palette::Red)
                          .Value("Green", Palette::Green)
                          .Alias("Primary", "Red")
                          .Documentation("The palette.")
                          .Documentation("Red", "The red enumerator."));
    return Studio.Commit();
  };

  if (Reversed) {
    Check(RegisterStudio().IsSuccess(), "the class plan commits");
    Check(RegisterModule().IsSuccess(), "the module plan commits");
    return;
  }
  Check(RegisterModule().IsSuccess(), "the module plan commits");
  Check(RegisterStudio().IsSuccess(), "the class plan commits");
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
  const Luna::DocumentationOptions Options;

  const Luna::GeneratedArtifact First =
      Luna::GenerateDocumentation(Snapshot, Options);
  const Luna::GeneratedArtifact Second =
      Luna::GenerateDocumentation(Snapshot, Options);

  Check(First.IsComplete() && First.Status() == Luna::GenerationStatus::Valid &&
            First.Diagnostic() == nullptr,
        "a complete artifact reports no diagnostic");
  Check(!First.Bytes().empty() && First.Size() == First.Bytes().size(),
        "the artifact owns its whole byte buffer");
  Check(First.Bytes() == Second.Bytes(),
        "repeated generation from equal content and options is byte-identical");

  Check(First.Bytes().find('\r') == std::string::npos,
        "generated bytes use LF line endings only");
  Check(!First.Bytes().starts_with("\xef\xbb\xbf"),
        "generated bytes carry no byte-order mark");
  Check(First.Bytes().find("\xef\xbb\xbf") == std::string::npos,
        "no byte-order mark appears anywhere in the artifact");
  Check(First.Bytes().starts_with("# Luna API Reference\n"),
        "the artifact opens with its canonical title line");
  Check(First.Bytes().back() == '\n',
        "every generated line, including the last, is LF terminated");
}

void CheckGeneratedOrderIgnoresRegistrationOrder() {
  Luna::State Forward;
  Luna::State Backward;
  const Luna::ReflectionSnapshot Ordered = Surface(Forward, false);
  const Luna::ReflectionSnapshot Permuted = Surface(Backward, true);
  const Luna::DocumentationOptions Options;

  const Luna::GeneratedArtifact FromOrdered =
      Luna::GenerateDocumentation(Ordered, Options);
  const Luna::GeneratedArtifact FromPermuted =
      Luna::GenerateDocumentation(Permuted, Options);
  Check(FromOrdered.IsComplete() && FromPermuted.IsComplete(),
        "both permutations generate complete artifacts");
  Check(FromOrdered.Bytes() == FromPermuted.Bytes(),
        "generated documentation is independent of registration order");
}

void CheckGeneratedContentCoversTheReflectedSurface() {
  Luna::State Owner;
  const Luna::ReflectionSnapshot Snapshot = Surface(Owner, false);
  const Luna::GeneratedArtifact Artifact =
      Luna::GenerateDocumentation(Snapshot, Luna::DocumentationOptions());
  Check(Artifact.IsComplete(), "the documented surface generates completely");
  const std::string &Text = Artifact.Bytes();

  Check(Contains(Text, "## Modules\n") && Contains(Text, "## Types\n") &&
            Contains(Text, "## Symbols\n"),
        "the artifact publishes its module, type, and symbol sections");
  Check(Contains(Text, "### studio.physics@2.1.0\n") &&
            Contains(Text, "The documented module."),
        "one module names its identity, its version, and its documentation");
  Check(Contains(Text, "Module: studio.physics@2.1.0\n"),
        "a declaration a module contributed names its provenance");
  Check(Contains(Text, "### (no module)\n"),
        "declarations no module contributed form their own canonical group");

  Check(Contains(Text, "#### Physics\n") && Contains(Text, "Kind: namespace\n"),
        "a namespace is documented");
  Check(Contains(Text, "#### Physics.Gravity\n") &&
            Contains(Text, "Kind: constant\n") && Contains(Text, "Value: 10\n"),
        "a constant is documented with its canonical value");
  Check(Contains(Text, "Kind: overload_set\n") &&
            Contains(Text, "Kind: function_candidate\n") &&
            Contains(Text, "Signature: ") && Contains(Text, "Parameters:\n") &&
            Contains(Text, "Returns: scalar\n"),
        "an overload set, its candidate, its parameters, and its return shape "
        "are documented");
  Check(Contains(Text, "Doubles one value.") &&
            Contains(Text, "local Scaled = Physics.Scale(21)"),
        "declared prose and usage examples reach the artifact");
  Check(Contains(Text, "- Unit: m/s^2\n"),
        "declared attributes are documented");

  Check(Contains(Text, "#### Studio.Gadget\n") &&
            Contains(Text, "Kind: class\n"),
        "a class is documented");
  Check(Contains(Text, "Kind: constructor\n") && Contains(Text, "Ownership: "),
        "a construction candidate names its ownership result");
  Check(Contains(Text, "#### Studio.Gadget.Level\n") &&
            Contains(Text, "Kind: property\n") &&
            Contains(Text, "Receiver: ") && Contains(Text, "Readable: yes\n") &&
            Contains(Text, "Access: "),
        "a property names its receiver and its access policy");
  Check(Contains(Text, "Kind: field\n") && Contains(Text, "Kind: operator\n"),
        "a field and an operator are documented");
  Check(Contains(Text, "#### Studio.Palette\n") &&
            Contains(Text, "Kind: enum\n") &&
            Contains(Text, "Kind: enumerator\n") &&
            Contains(Text, "Kind: enumerator_alias\n"),
        "an enumeration, its enumerators, and its alias are documented");
  Check(Contains(Text, "Scope: (root)\n") && Contains(Text, "Scope: Studio\n"),
        "each symbol names the scope that owns it");
}

void CheckGenerationReadsOnlyTheCapturedSnapshot() {
  Luna::DocumentationOptions Options;
  std::string Captured;
  std::string Later;
  {
    auto Owner = std::make_unique<Luna::State>();
    Luna::BindingRegistry Registry = Owner->Bindings();
    RegisterSurface(Registry, false);
    const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
    Captured = Luna::GenerateDocumentation(Snapshot, Options).Bytes();

    Check(Registry.Register("Extra", [] { return 1; }).IsSuccess(),
          "a later registration commits after the snapshot was captured");
    Later = Luna::GenerateDocumentation(Registry.Reflection(), Options).Bytes();
    Check(!Contains(Captured, "#### Extra\n"),
          "the captured generation never observes the later declaration");
    Check(Contains(Later, "#### Extra\n"),
          "a newly captured generation does observe it");

    Owner.reset();
    Check(Luna::GenerateDocumentation(Snapshot, Options).Bytes() == Captured,
          "generation from a retained snapshot survives State destruction");
  }
  Check(!Captured.empty() && Captured != Later,
        "the two captured generations describe different surfaces");
}

void CheckOptionsSelectTheArtifactDeterministically() {
  Luna::State Owner;
  const Luna::ReflectionSnapshot Snapshot = Surface(Owner, false);

  const Luna::DocumentationOptions Full;
  const Luna::DocumentationOptions Trimmed = Luna::DocumentationOptions::Create(
      "Studio Reference", false, false, false);
  Check(Trimmed.Title() == "Studio Reference" &&
            !Trimmed.IncludesIdentities() && !Trimmed.IncludesAttributes() &&
            !Trimmed.IncludesExamples(),
        "created options report exactly what they were given");
  Check(Full == Luna::DocumentationOptions() && !(Full == Trimmed) &&
            Full == Trimmed.WithTitle(std::string())
                        .WithIdentities(true)
                        .WithAttributes(true)
                        .WithExamples(true),
        "options compare by value and every wither leaves the source alone");

  const std::string FullText =
      Luna::GenerateDocumentation(Snapshot, Full).Bytes();
  const std::string TrimmedText =
      Luna::GenerateDocumentation(Snapshot, Trimmed).Bytes();
  Check(TrimmedText.starts_with("# Studio Reference\n"),
        "the stated title opens the artifact");
  Check(Contains(FullText, "Identity: ") &&
            !Contains(TrimmedText, "Identity: "),
        "identities appear only when the options include them");
  Check(Contains(FullText, "Attributes:\n") &&
            !Contains(TrimmedText, "Attributes:\n"),
        "attributes appear only when the options include them");
  Check(Contains(FullText, "Examples:\n") &&
            !Contains(TrimmedText, "Examples:\n"),
        "examples appear only when the options include them");
  Check(Luna::GenerateDocumentation(Snapshot, Trimmed).Bytes() == TrimmedText,
        "repeated generation with the same options stays byte-identical");
}

void CheckUnencodableMetadataIsRejectedWithoutPartialBytes() {
  const auto Generate = [](std::string_view Documentation) {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    static_cast<void>(
        Studio.RegisterFunction("Double", [](int Value) { return Value * 2; }));
    static_cast<void>(Studio.Documentation("Double", Documentation));
    Check(Studio.Commit().IsSuccess(),
          "documentation text is metadata, so the plan still commits");
    return Luna::GenerateDocumentation(Registry.Reflection(),
                                       Luna::DocumentationOptions());
  };

  const Luna::GeneratedArtifact Malformed = Generate("bad \xff byte");
  Check(!Malformed.IsComplete() &&
            Malformed.Status() == Luna::GenerationStatus::InvalidEncoding,
        "text that is not canonical UTF-8 rejects generation");
  Check(Malformed.Bytes().empty() && Malformed.Size() == 0,
        "a rejected attempt exposes no partial artifact");
  Check(Malformed.Diagnostic() != nullptr &&
            !Malformed.Diagnostic()->Message().empty() &&
            Contains(Malformed.Diagnostic()->Message(), "Studio.Double") &&
            Contains(Malformed.Diagnostic()->Message(), "invalid-encoding"),
        "the rejection names the symbol and the deterministic reason");

  const Luna::GeneratedArtifact Marked = Generate("marked \xef\xbb\xbf text");
  Check(!Marked.IsComplete() &&
            Marked.Status() == Luna::GenerationStatus::ForbiddenByteOrderMark &&
            Marked.Bytes().empty(),
        "a byte-order mark inside metadata rejects generation");
  Check(Generate("plain text").IsComplete(),
        "encodable metadata still generates completely");
}

void CheckEmptySnapshotGeneratesACompleteArtifact() {
  const Luna::ReflectionSnapshot Empty;
  const Luna::GeneratedArtifact Artifact =
      Luna::GenerateDocumentation(Empty, Luna::DocumentationOptions());
  Check(Artifact.IsComplete() && !Artifact.Bytes().empty(),
        "an empty generation produces a complete artifact");
  Check(Contains(Artifact.Bytes(), "Symbols: 0\n") &&
            Contains(Artifact.Bytes(), "Types: 0\n") &&
            Contains(Artifact.Bytes(), "Modules: 0\n") &&
            Contains(Artifact.Bytes(), "None.\n"),
        "the empty artifact states that each section is empty");

  const Luna::GeneratedArtifact Unspecified;
  Check(!Unspecified.IsComplete() &&
            Unspecified.Status() == Luna::GenerationStatus::Unspecified &&
            Unspecified.Diagnostic() != nullptr &&
            !Unspecified.Diagnostic()->Message().empty(),
        "a default artifact is the reserved unspecified value");
}

} // namespace

int RunDocumentationGenerationTests() {
  FailureCount = 0;
  CheckRepeatedGenerationIsByteIdenticalAndCanonical();
  CheckGeneratedOrderIgnoresRegistrationOrder();
  CheckGeneratedContentCoversTheReflectedSurface();
  CheckGenerationReadsOnlyTheCapturedSnapshot();
  CheckOptionsSelectTheArtifactDeterministically();
  CheckUnencodableMetadataIsRejectedWithoutPartialBytes();
  CheckEmptySnapshotGeneratesACompleteArtifact();
  return FailureCount == 0 ? 0 : 1;
}
