// clang-format off
#include <luna/luna.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
// clang-format on

namespace {

static_assert(
    std::is_same_v<decltype(Luna::GenerateDocumentation(
                       std::declval<const Luna::ReflectionSnapshot &>(),
                       std::declval<const Luna::DocumentationOptions &>())),
                   Luna::GeneratedArtifact>,
    "Documentation generation must keep answering one owned artifact.");
static_assert(
    std::is_same_v<decltype(Luna::GenerateDeclarations(
                       std::declval<const Luna::ReflectionSnapshot &>(),
                       std::declval<const Luna::DeclarationOptions &>())),
                   Luna::GeneratedArtifact>,
    "Declaration generation must keep answering one owned artifact.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::GeneratedArtifact &>().Bytes()),
        const std::string &>,
    "A generated artifact must expose its bytes as an owned string.");
static_assert(
    std::is_same_v<decltype(Luna::PublishArtifact(
                       std::declval<const Luna::GeneratedArtifact &>(),
                       std::declval<std::string_view>())),
                   Luna::ArtifactPublication>,
    "Publication must keep answering one ordinary Luna outcome value.");
static_assert(std::is_copy_constructible_v<Luna::DocumentationOptions> &&
                  std::is_copy_constructible_v<Luna::DeclarationOptions>,
              "Generator options must stay copyable immutable values.");

struct ConsumerGauge final {
  int Charge = 3;
  [[nodiscard]] int Level() const { return Charge * 2; }
};

enum class ConsumerMode { Off = 0, On = 1 };

[[nodiscard]] int ConsumerIncrement(int Value) { return Value + 1; }

void ConfigureConsumerUnits(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Units = Builder.RegisterNamespace("Units");
  static_cast<void>(Units.RegisterConstant("Scale", 2));
}

} // namespace

void VerifyGenerationConsumerBoundaryCompiles() {
  Luna::ReflectionSnapshot Retained;
  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();

    [[maybe_unused]] const Luna::RegistrationResult Function =
        Registry.RegisterFunction("Increment", &ConsumerIncrement);

    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    static_cast<void>(Studio.RegisterConstant("Version", 7));
    Luna::EnumBuilder<ConsumerMode> Modes = Studio.RegisterEnum<ConsumerMode>(
        "Mode", Luna::StableTypeKey("consumer.generation.Mode"));
    static_cast<void>(Modes.Value("Off", ConsumerMode::Off)
                          .Value("On", ConsumerMode::On)
                          .Documentation("The consumer mode."));
    Luna::ClassBuilder<ConsumerGauge> Gauge =
        Studio.RegisterClass<ConsumerGauge>(
            "Gauge", Luna::StableTypeKey("consumer.generation.Gauge"));
    static_cast<void>(Gauge.Constructor<>()
                          .Field("Charge", &ConsumerGauge::Charge)
                          .Property("Level", &ConsumerGauge::Level)
                          .Documentation("One consumer gauge."));
    [[maybe_unused]] const Luna::RegistrationResult Committed = Studio.Commit();

    const std::optional<Luna::ModuleManifest> Units =
        Luna::ModuleManifest::TryCreate(
            "consumer.generation.units",
            Luna::SemanticVersion::TryParse("1.0.0").value_or(
                Luna::SemanticVersion()),
            {}, std::string(), {});
    if (Units) {
      [[maybe_unused]] const Luna::RegistrationResult Loaded =
          Registry.RegisterModule(*Units, &ConfigureConsumerUnits);
    }

    Retained = Registry.Reflection();
  }

  const Luna::DocumentationOptions Documentation =
      Luna::DocumentationOptions::Create("Consumer Reference", true, true, true)
          .WithExamples(false);
  const Luna::DeclarationOptions Declarations =
      Luna::DeclarationOptions().WithBanner("Consumer declarations.");

  const Luna::GeneratedArtifact Text =
      Luna::GenerateDocumentation(Retained, Documentation);
  const Luna::GeneratedArtifact Lua =
      Luna::GenerateDeclarations(Retained, Declarations);
  [[maybe_unused]] const bool Generated =
      Text.IsComplete() && Lua.IsComplete() &&
      Text.Status() == Luna::GenerationStatus::Valid &&
      Lua.Size() == Lua.Bytes().size() &&
      (Text.Diagnostic() == nullptr ||
       Text.Diagnostic()->Category() == Luna::ErrorCategory::Internal) &&
      !Luna::GenerationStatusText(Lua.Status()).empty();

  const Luna::ArtifactPublication PublishedText =
      Luna::PublishArtifact(Text, "consumer-generation.md");
  const Luna::ArtifactPublication PublishedLua = Luna::PublishDeclarations(
      Retained, Declarations, "consumer-generation.d.lua");
  [[maybe_unused]] const bool Published =
      (PublishedText.IsPublished() || PublishedText.Diagnostic() != nullptr) &&
      (PublishedLua.IsPublished() || PublishedLua.Diagnostic() != nullptr) &&
      !Luna::PublicationStatusText(PublishedText.Status()).empty() &&
      PublishedLua.Size() <= Lua.Size();
}
