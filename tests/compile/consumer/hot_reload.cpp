// clang-format off
#include <luna/luna.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
// clang-format on

namespace {

template <typename T, typename = void>
struct ExposesGenerationSet : std::false_type {};
template <typename T>
struct ExposesGenerationSet<
    T, std::void_t<decltype(std::declval<const T &>().Generations())>>
    : std::true_type {};

template <typename T, typename = void>
struct ExposesDispatchGeneration : std::false_type {};
template <typename T>
struct ExposesDispatchGeneration<
    T, std::void_t<decltype(std::declval<const T &>().DispatchGeneration())>>
    : std::true_type {};

template <typename T, typename = void>
struct ExposesUnload : std::false_type {};
template <typename T>
struct ExposesUnload<T, std::void_t<decltype(std::declval<T &>().UnloadModule(
                            std::declval<std::string_view>()))>>
    : std::true_type {};

template <typename T, typename = void>
struct ExposesReplace : std::false_type {};
template <typename T>
struct ExposesReplace<T, std::void_t<decltype(std::declval<T &>().ReplaceModule(
                             std::declval<Luna::ModuleManifest>()))>>
    : std::true_type {};

static_assert(!ExposesGenerationSet<Luna::State>::value &&
                  !ExposesGenerationSet<Luna::BindingRegistry>::value &&
                  !ExposesGenerationSet<Luna::ReflectionSnapshot>::value,
              "No lifecycle-facing declaration may expose a generation set.");
static_assert(!ExposesDispatchGeneration<Luna::State>::value &&
                  !ExposesDispatchGeneration<Luna::BindingRegistry>::value &&
                  !ExposesDispatchGeneration<Luna::ReflectionSnapshot>::value,
              "No lifecycle-facing declaration may expose a dispatch "
              "generation.");
static_assert(!ExposesUnload<Luna::State>::value &&
                  !ExposesUnload<Luna::BindingRegistry>::value &&
                  !ExposesReplace<Luna::State>::value &&
                  !ExposesReplace<Luna::BindingRegistry>::value,
              "Unload and replacement stay unavailable instead of being "
              "advertised.");

static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::ReflectionSnapshot &>().Generation()),
        std::uint64_t>,
    "A snapshot must report only its own reflection generation.");
static_assert(std::is_copy_constructible_v<Luna::ReflectionSnapshot> &&
                  std::is_copy_constructible_v<Luna::ModuleManifest> &&
                  std::is_copy_constructible_v<Luna::ModuleRecord>,
              "Lifecycle-facing values must stay ordinary consumer values.");
static_assert(std::is_same_v<decltype(std::declval<const Luna::ModuleRecord &>()
                                          .Version()),
                             std::string_view>,
              "Module provenance must stay plain text a consumer can read.");
static_assert(
    std::is_same_v<decltype(Luna::GenerateDeclarations(
                       std::declval<const Luna::ReflectionSnapshot &>(),
                       std::declval<const Luna::DeclarationOptions &>())),
                   Luna::GeneratedArtifact>,
    "Artifact generation must answer one owning consumer value.");

struct ReloadProbe final {
  int Charge = 4;

  [[nodiscard]] int Level() const { return Charge * 2; }
};

[[nodiscard]] int ReloadScale(int Value) { return Value * 3; }

void ConfigureReloadSurface(Luna::NamespaceBuilder &Builder) {
  Luna::NamespaceBuilder Scope = Builder.RegisterNamespace("Reload");
  static_cast<void>(Scope.RegisterConstant("Version", 1));
  static_cast<void>(Scope.RegisterFunction("Scale", &ReloadScale));
  Luna::ClassBuilder<ReloadProbe> Probe = Scope.RegisterClass<ReloadProbe>(
      "Probe", Luna::StableTypeKey("consumer.Probe"));
  static_cast<void>(Probe.Constructor<>()
                        .Field("Charge", &ReloadProbe::Charge)
                        .Property("Level", &ReloadProbe::Level));
}

[[nodiscard]] std::optional<Luna::ModuleManifest>
ReloadManifest(std::string_view VersionText) {
  return Luna::ModuleManifest::TryCreate(
      std::string("consumer.reload"),
      Luna::SemanticVersion::TryParse(VersionText)
          .value_or(Luna::SemanticVersion()),
      {}, std::string("One reloadable module."), {});
}

} // namespace

void VerifyHotReloadConsumerBoundaryCompiles() {
  Luna::ReflectionSnapshot Retained;
  std::string Generated;

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();

    if (const std::optional<Luna::ModuleManifest> First =
            ReloadManifest("1.0.0")) {
      [[maybe_unused]] const Luna::RegistrationResult Loaded =
          Registry.RegisterModule(*First, &ConfigureReloadSurface);
    }

    Retained = Registry.Reflection();
    Generated = Luna::GenerateDeclarations(Retained, Luna::DeclarationOptions())
                    .Bytes();

    if (const std::optional<Luna::ModuleManifest> Second =
            ReloadManifest("2.0.0")) {

      [[maybe_unused]] const Luna::RegistrationResult Again =
          Registry.RegisterModule(*Second, &ConfigureReloadSurface);
    }

    [[maybe_unused]] const Luna::ExecutionResult Executed =
        Owner.Execute("return Reload.Scale(7)");
  }

  [[maybe_unused]] const std::uint64_t Generation = Retained.Generation();
  [[maybe_unused]] const std::size_t Modules = Retained.Modules().Size();
  [[maybe_unused]] const bool Reflected =
      Retained.Find("Reload.Scale").IsValid();
  [[maybe_unused]] const bool Stable =
      Generated ==
      Luna::GenerateDeclarations(Retained, Luna::DeclarationOptions()).Bytes();
}
