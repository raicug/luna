// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/generation/declaration.hpp>
#include <luna/generation/documentation.hpp>
#include <luna/generation/publication.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>

#include "state/generation/publication.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
// clang-format on

namespace {

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "artifact publication check failed: " << Description << '\n';
}

class ScratchDirectory final {
public:
  explicit ScratchDirectory(std::string_view Name) {
    static unsigned Counter = 0;
    std::string Leaf("luna-publication-");
    Leaf.append(Name);
    Leaf.push_back('-');
    Leaf.append(std::to_string(++Counter));
    PathValue = std::filesystem::temp_directory_path() / Leaf;
    std::error_code Error;
    std::filesystem::remove_all(PathValue, Error);
    Check(std::filesystem::create_directories(PathValue, Error) && !Error,
          "the scratch directory is created");
  }

  ScratchDirectory(const ScratchDirectory &) = delete;
  ScratchDirectory &operator=(const ScratchDirectory &) = delete;

  ~ScratchDirectory() {
    std::error_code Error;
    std::filesystem::remove_all(PathValue, Error);
  }

  [[nodiscard]] const std::filesystem::path &Path() const { return PathValue; }

  [[nodiscard]] std::filesystem::path File(std::string_view Name) const {
    return PathValue / std::filesystem::path(std::string(Name));
  }

  [[nodiscard]] std::vector<std::string> Names() const {
    std::vector<std::string> Found;
    std::error_code Error;
    for (const auto &Entry :
         std::filesystem::directory_iterator(PathValue, Error))
      Found.push_back(Entry.path().filename().string());
    return Found;
  }

private:
  std::filesystem::path PathValue;
};

void WriteBytes(const std::filesystem::path &Path, std::string_view Bytes) {
  std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
  Stream.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
  Stream.close();
  Check(!Stream.fail(), "the prior destination content is written");
}

[[nodiscard]] std::optional<std::string>
ReadBytes(const std::filesystem::path &Path) {
  std::ifstream Stream(Path, std::ios::binary);
  if (!Stream.is_open())
    return std::nullopt;
  return std::string((std::istreambuf_iterator<char>(Stream)),
                     std::istreambuf_iterator<char>());
}

[[nodiscard]] std::string Text(const std::filesystem::path &Path) {
  return Path.string();
}

[[nodiscard]] bool Exists(const std::filesystem::path &Path) {
  std::error_code Error;
  return std::filesystem::exists(Path, Error);
}

[[nodiscard]] Luna::GeneratedArtifact Artifact(std::string Bytes) {
  return Luna::GeneratedArtifact::Complete(std::move(Bytes));
}

[[nodiscard]] Luna::ReflectionSnapshot Surface(Luna::State &Owner) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  static_cast<void>(
      Studio.RegisterFunction("Double", [](int Value) { return Value * 2; }));
  static_cast<void>(Studio.Documentation("Double", "Doubles one value."));
  Check(Studio.Commit().IsSuccess(), "the published surface commits");
  return Registry.Reflection();
}

void CheckCompleteArtifactReplacesTheDestinationExactly() {
  const ScratchDirectory Scratch("complete");
  const std::filesystem::path Destination = Scratch.File("api.md");
  const std::string First = "# One\nline\n";
  const std::string Second = "# Two\nother\nlines\n";

  const Luna::ArtifactPublication Created =
      Luna::PublishArtifact(Artifact(First), Text(Destination));
  Check(Created.IsPublished() &&
            Created.Status() == Luna::PublicationStatus::Published &&
            Created.Diagnostic() == nullptr,
        "publishing to a new destination reports no diagnostic");
  Check(Created.Size() == First.size(),
        "the published size is the whole artifact");
  Check(ReadBytes(Destination) == std::optional<std::string>(First),
        "the destination holds exactly the artifact bytes");

  const Luna::ArtifactPublication Replaced =
      Luna::PublishArtifact(Artifact(Second), Text(Destination));
  Check(Replaced.IsPublished() && Replaced.Size() == Second.size(),
        "publishing over a prior destination succeeds");
  Check(ReadBytes(Destination) == std::optional<std::string>(Second),
        "the destination holds exactly the replacement bytes");
  Check(Scratch.Names() == std::vector<std::string>{"api.md"},
        "no unpublished file survives a successful publication");

  const Luna::ArtifactPublication Empty =
      Luna::PublishArtifact(Artifact(std::string()), Text(Destination));
  Check(Empty.IsPublished() && Empty.Size() == 0 &&
            ReadBytes(Destination) == std::optional<std::string>(std::string()),
        "an empty but complete artifact publishes as an empty destination");
}

void CheckRefusedArtifactsPreserveThePriorDestination() {
  const ScratchDirectory Scratch("artifact");
  const std::filesystem::path Destination = Scratch.File("api.md");
  const std::string Prior = "# Prior\nbytes\n";
  WriteBytes(Destination, Prior);

  const auto Refused = [&](const Luna::GeneratedArtifact &Candidate,
                           Luna::PublicationStatus Expected,
                           std::string_view Description) {
    const Luna::ArtifactPublication Result =
        Luna::PublishArtifact(Candidate, Text(Destination));
    Check(!Result.IsPublished() && Result.Status() == Expected &&
              Result.Size() == 0,
          Description);
    Check(Result.Diagnostic() != nullptr &&
              !Result.Diagnostic()->Message().empty(),
          "a refused publication carries a non-empty diagnostic");
    Check(ReadBytes(Destination) == std::optional<std::string>(Prior),
          "the prior destination is preserved byte for byte");
    Check(Scratch.Names() == std::vector<std::string>{"api.md"},
          "a refused publication leaves no unpublished file behind");
  };

  Refused(Luna::GeneratedArtifact::Rejected(
              Luna::GenerationStatus::UnsupportedDeclaration, "unsupported"),
          Luna::PublicationStatus::IncompleteArtifact,
          "a rejected generation refuses publication");
  Refused(Luna::GeneratedArtifact(),
          Luna::PublicationStatus::IncompleteArtifact,
          "an unspecified artifact refuses publication");
  Refused(Artifact("bad \xff byte\n"),
          Luna::PublicationStatus::NonCanonicalArtifact,
          "bytes that are not canonical UTF-8 refuse publication");
  Refused(Artifact("\xef\xbb\xbf# Marked\n"),
          Luna::PublicationStatus::NonCanonicalArtifact,
          "a byte-order mark refuses publication");
  Refused(Artifact("# Windows\r\nendings\r\n"),
          Luna::PublicationStatus::NonCanonicalArtifact,
          "a carriage return refuses publication");

  Check(Luna::ArtifactPublication().Status() ==
                Luna::PublicationStatus::Unspecified &&
            !Luna::ArtifactPublication().IsPublished() &&
            Luna::ArtifactPublication().Diagnostic() != nullptr &&
            !Luna::ArtifactPublication().Diagnostic()->Message().empty(),
        "a default publication is the reserved unspecified outcome");
}

void CheckUnusableDestinationsAreRefused() {
  const ScratchDirectory Scratch("destination");
  const std::filesystem::path Directory = Scratch.File("nested");
  std::error_code Error;
  Check(std::filesystem::create_directory(Directory, Error) && !Error,
        "the nested directory is created");

  const auto Refused = [](std::string_view Destination,
                          std::string_view Description) {
    const Luna::ArtifactPublication Result =
        Luna::PublishArtifact(Artifact("# Body\n"), Destination);
    Check(!Result.IsPublished() &&
              Result.Status() == Luna::PublicationStatus::InvalidDestination &&
              Result.Diagnostic() != nullptr &&
              !Result.Diagnostic()->Message().empty(),
          Description);
  };

  Refused(std::string_view(), "an empty destination is refused");
  Refused(Text(Directory), "a destination that names a directory is refused");
  Refused(Text(Scratch.File("missing") / "api.md"),
          "a destination inside a missing directory is refused");

  Check(Scratch.Names() == std::vector<std::string>{"nested"},
        "no refused destination created a file");
  Check(std::filesystem::is_directory(Directory, Error),
        "the directory that was named as a destination is untouched");
  Check(!Exists(Scratch.File("missing")),
        "a missing destination directory is never created");
}

void CheckEveryDestinationStageFailurePreservesThePriorDestination() {
  using Luna::Detail::ArtifactPublicationFault;
  using Luna::Detail::ScopedArtifactPublicationFault;

  const std::string Prior = "# Prior\nbytes\n";
  const auto Failing = [&Prior](ArtifactPublicationFault Fault,
                                std::string_view Description) {
    const ScratchDirectory Scratch("stage");
    const std::filesystem::path Destination = Scratch.File("api.md");
    WriteBytes(Destination, Prior);

    const ScopedArtifactPublicationFault Injected(Fault);
    const Luna::ArtifactPublication Result =
        Luna::PublishArtifact(Artifact("# Next\nbytes\n"), Text(Destination));
    Check(!Result.IsPublished() &&
              Result.Status() ==
                  Luna::PublicationStatus::DestinationUnavailable &&
              Result.Size() == 0,
          Description);
    Check(Injected.WasConsumed(), "publication reached the failing stage");
    Check(Result.Diagnostic() != nullptr &&
              !Result.Diagnostic()->Message().empty(),
          "a failed destination stage carries a non-empty diagnostic");
    Check(ReadBytes(Destination) == std::optional<std::string>(Prior),
          "the prior destination is preserved byte for byte");
    Check(Scratch.Names() == std::vector<std::string>{"api.md"},
          "no unpublished file survives a failed destination stage");
  };

  Failing(ArtifactPublicationFault::UnpublishedCreation,
          "a refused unpublished file refuses publication");
  Failing(ArtifactPublicationFault::UnpublishedWrite,
          "a failed write refuses publication");
  Failing(ArtifactPublicationFault::UnpublishedFlush,
          "a failed flush refuses publication");
  Failing(ArtifactPublicationFault::Verification,
          "an unverified unpublished file refuses publication");
  Failing(ArtifactPublicationFault::Replacement,
          "a failed replacement refuses publication");

  const ScratchDirectory Scratch("absent");
  const std::filesystem::path Destination = Scratch.File("api.md");
  const ScopedArtifactPublicationFault Injected(
      ArtifactPublicationFault::Replacement);
  const Luna::ArtifactPublication Result =
      Luna::PublishArtifact(Artifact("# Body\n"), Text(Destination));
  Check(!Result.IsPublished() && Injected.WasConsumed(),
        "the injected replacement failure refuses publication");
  Check(!Exists(Destination) && Scratch.Names().empty(),
        "a failed publication exposes no partial artifact at all");
}

void CheckInjectedFailuresDoNotOutliveTheirScope() {
  using Luna::Detail::ArtifactPublicationFault;
  using Luna::Detail::ScopedArtifactPublicationFault;

  const ScratchDirectory Scratch("scope");
  const std::filesystem::path Destination = Scratch.File("api.md");
  {
    const ScopedArtifactPublicationFault Injected(
        ArtifactPublicationFault::Replacement);
    Check(!Luna::PublishArtifact(Artifact("# One\n"), Text(Destination))
               .IsPublished(),
          "the injected failure applies inside its scope");
    Check(Injected.WasConsumed(), "the injected failure was consumed once");
    Check(Luna::PublishArtifact(Artifact("# One\n"), Text(Destination))
              .IsPublished(),
          "one injection fails exactly one stage once");
  }
  Check(Luna::PublishArtifact(Artifact("# Two\n"), Text(Destination))
            .IsPublished(),
        "publication succeeds again after the injection scope ends");
  Check(ReadBytes(Destination) == std::optional<std::string>("# Two\n"),
        "the destination holds the last published artifact");
}

void CheckSnapshotPublicationMatchesGeneration() {
  const ScratchDirectory Scratch("snapshot");
  Luna::State Owner;
  const Luna::ReflectionSnapshot Snapshot = Surface(Owner);
  const Luna::DocumentationOptions Documentation;
  const Luna::DeclarationOptions Declarations;

  const std::filesystem::path DocumentationPath = Scratch.File("api.md");
  const Luna::ArtifactPublication PublishedDocumentation =
      Luna::PublishDocumentation(Snapshot, Documentation,
                                 Text(DocumentationPath));
  Check(PublishedDocumentation.IsPublished(),
        "documentation publishes from one captured snapshot");
  Check(ReadBytes(DocumentationPath) ==
            std::optional<std::string>(
                Luna::GenerateDocumentation(Snapshot, Documentation).Bytes()),
        "the destination holds exactly the generated documentation");

  const std::filesystem::path DeclarationPath = Scratch.File("api.d.lua");
  const Luna::ArtifactPublication PublishedDeclarations =
      Luna::PublishDeclarations(Snapshot, Declarations, Text(DeclarationPath));
  Check(PublishedDeclarations.IsPublished(),
        "declarations publish from one captured snapshot");
  Check(ReadBytes(DeclarationPath) ==
            std::optional<std::string>(
                Luna::GenerateDeclarations(Snapshot, Declarations).Bytes()),
        "the destination holds exactly the generated declarations");

  const std::string Prior = *ReadBytes(DocumentationPath);
  Luna::State Malformed;
  Luna::BindingRegistry Registry = Malformed.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  static_cast<void>(
      Studio.RegisterFunction("Double", [](int Value) { return Value * 2; }));
  static_cast<void>(Studio.Documentation("Double", "bad \xff byte"));
  Check(Studio.Commit().IsSuccess(), "documentation text is metadata only");

  const Luna::ArtifactPublication Rejected = Luna::PublishDocumentation(
      Registry.Reflection(), Documentation, Text(DocumentationPath));
  Check(!Rejected.IsPublished() &&
            Rejected.Status() == Luna::PublicationStatus::IncompleteArtifact,
        "a rejected generation refuses publication");
  Check(ReadBytes(DocumentationPath) == std::optional<std::string>(Prior),
        "the prior destination survives a rejected generation byte for byte");
}

} // namespace

int RunArtifactPublicationTests() {
  FailureCount = 0;
  CheckCompleteArtifactReplacesTheDestinationExactly();
  CheckRefusedArtifactsPreserveThePriorDestination();
  CheckUnusableDestinationsAreRefused();
  CheckEveryDestinationStageFailurePreservesThePriorDestination();
  CheckInjectedFailuresDoNotOutliveTheirScope();
  CheckSnapshotPublicationMatchesGeneration();
  return FailureCount == 0 ? 0 : 1;
}
