// clang-format off
#include "state/generation/publication.hpp"

#include "state/generation/writer.hpp"

#include <luna/generation/artifact.hpp>
#include <luna/generation/declaration.hpp>
#include <luna/generation/documentation.hpp>
#include <luna/generation/publication.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

thread_local ArtifactPublicationFault PendingFaultValue =
    ArtifactPublicationFault::None;
thread_local bool ConsumedFaultValue = false;

[[nodiscard]] bool ConsumePublicationFault(ArtifactPublicationFault Fault) {
  if (Fault == ArtifactPublicationFault::None || PendingFaultValue != Fault)
    return false;
  PendingFaultValue = ArtifactPublicationFault::None;
  ConsumedFaultValue = true;
  return true;
}

} // namespace

ScopedArtifactPublicationFault::ScopedArtifactPublicationFault(
    ArtifactPublicationFault Fault) noexcept
    : PreviousFault(PendingFaultValue), PreviousConsumed(ConsumedFaultValue) {
  PendingFaultValue = Fault;
  ConsumedFaultValue = false;
}

ScopedArtifactPublicationFault::~ScopedArtifactPublicationFault() {
  PendingFaultValue = PreviousFault;
  ConsumedFaultValue = PreviousConsumed;
}

bool ScopedArtifactPublicationFault::WasConsumed() const noexcept {
  return ConsumedFaultValue;
}

} // namespace Luna::Detail

namespace Luna {
namespace {

using Luna::Detail::ArtifactPublicationFault;

[[nodiscard]] std::string RefusalMessage(std::string_view Destination,
                                         PublicationStatus Status,
                                         std::string_view Reason) {
  std::string Message("Artifact publication to \"");
  Message.append(Destination);
  Message.append("\" failed: ");
  Message.append(PublicationStatusText(Status));
  Message.append(" (");
  Message.append(Reason);
  Message.append(").");
  return Message;
}

[[nodiscard]] ArtifactPublication Refuse(std::string_view Destination,
                                         PublicationStatus Status,
                                         std::string_view Reason) {
  return ArtifactPublication::Refused(
      Status, RefusalMessage(Destination, Status, Reason));
}

[[nodiscard]] std::filesystem::path
UnpublishedPathFor(const std::filesystem::path &Destination) {
  static std::atomic<std::uint64_t> Ticket{0};
  const std::filesystem::path Parent = Destination.parent_path();
  for (int Attempt = 0; Attempt < 64; ++Attempt) {
    std::string Name = Destination.filename().string();
    Name.append(".luna-");
    Name.append(std::to_string(Ticket.fetch_add(1) + 1));
    Name.append(".unpublished");
    const std::filesystem::path Candidate =
        Parent.empty() ? std::filesystem::path(Name) : Parent / Name;
    std::error_code Error;
    if (!std::filesystem::exists(Candidate, Error))
      return Candidate;
  }
  return std::filesystem::path();
}

} // namespace

ArtifactPublication PublishArtifact(const GeneratedArtifact &Artifact,
                                    std::string_view DestinationPath) {
  if (!Artifact.IsComplete())
    return Refuse(
        DestinationPath, PublicationStatus::IncompleteArtifact,
        "the generator rejected the artifact, so it carries no bytes");

  const std::string &Bytes = Artifact.Bytes();
  if (const GenerationStatus Reason = Detail::ClassifyGeneratedText(Bytes);
      Reason != GenerationStatus::Valid)
    return Refuse(DestinationPath, PublicationStatus::NonCanonicalArtifact,
                  GenerationStatusText(Reason));
  if (Bytes.find('\r') != std::string::npos)
    return Refuse(DestinationPath, PublicationStatus::NonCanonicalArtifact,
                  "the artifact carries a carriage return");

  if (DestinationPath.empty())
    return Refuse(DestinationPath, PublicationStatus::InvalidDestination,
                  "no destination was named");

  const std::filesystem::path Destination{std::string(DestinationPath)};
  if (Destination.filename().empty())
    return Refuse(DestinationPath, PublicationStatus::InvalidDestination,
                  "the destination does not name a file");

  std::error_code Error;
  const std::filesystem::file_status Existing =
      std::filesystem::status(Destination, Error);
  if (std::filesystem::exists(Existing) &&
      !std::filesystem::is_regular_file(Existing))
    return Refuse(DestinationPath, PublicationStatus::InvalidDestination,
                  "the destination does not name a regular file");

  const std::filesystem::path Parent = Destination.parent_path();
  if (!Parent.empty() && !std::filesystem::is_directory(Parent, Error))
    return Refuse(DestinationPath, PublicationStatus::InvalidDestination,
                  "the destination directory does not exist");

  const std::filesystem::path Unpublished = UnpublishedPathFor(Destination);
  if (Unpublished.empty())
    return Refuse(DestinationPath, PublicationStatus::DestinationUnavailable,
                  "no unpublished file name was available");

  const auto Abandon = [&](std::string_view Reason) {
    std::error_code Ignored;
    static_cast<void>(std::filesystem::remove(Unpublished, Ignored));
    return Refuse(DestinationPath, PublicationStatus::DestinationUnavailable,
                  Reason);
  };

  std::string_view Failure;
  {
    std::ofstream Stream(Unpublished, std::ios::binary | std::ios::trunc);
    if (!Stream.is_open() ||
        Detail::ConsumePublicationFault(
            ArtifactPublicationFault::UnpublishedCreation)) {
      Failure = "the unpublished artifact file could not be created";
    } else {
      if (!Bytes.empty())
        Stream.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
      if (!Stream.good() || Detail::ConsumePublicationFault(
                                ArtifactPublicationFault::UnpublishedWrite)) {
        Failure = "the complete artifact could not be written to the "
                  "unpublished file";
      } else {
        Stream.flush();
        if (!Stream.good() || Detail::ConsumePublicationFault(
                                  ArtifactPublicationFault::UnpublishedFlush)) {
          Failure = "the unpublished artifact file could not be flushed";
        } else {
          Stream.close();
          if (Stream.fail())
            Failure = "the unpublished artifact file could not be closed";
        }
      }
    }
  }
  if (!Failure.empty())
    return Abandon(Failure);

  {
    std::ifstream Reader(Unpublished, std::ios::binary);
    if (!Reader.is_open()) {
      Failure = "the unpublished artifact file could not be reread";
    } else {
      const std::string Written((std::istreambuf_iterator<char>(Reader)),
                                std::istreambuf_iterator<char>());
      if (Reader.bad()) {
        Failure = "the unpublished artifact file could not be reread";
      } else if (Written != Bytes ||
                 Detail::ConsumePublicationFault(
                     ArtifactPublicationFault::Verification)) {
        Failure = "the unpublished artifact file does not match the artifact";
      }
    }
  }
  if (!Failure.empty())
    return Abandon(Failure);

  if (Detail::ConsumePublicationFault(ArtifactPublicationFault::Replacement))
    return Abandon("the verified artifact could not replace the destination");

  std::error_code ReplacementError;
  std::filesystem::rename(Unpublished, Destination, ReplacementError);
  if (ReplacementError)
    return Abandon("the verified artifact could not replace the destination");

  return ArtifactPublication::Complete(Bytes.size());
}

ArtifactPublication PublishDocumentation(const ReflectionSnapshot &Snapshot,
                                         const DocumentationOptions &Options,
                                         std::string_view DestinationPath) {
  return PublishArtifact(GenerateDocumentation(Snapshot, Options),
                         DestinationPath);
}

ArtifactPublication PublishDeclarations(const ReflectionSnapshot &Snapshot,
                                        const DeclarationOptions &Options,
                                        std::string_view DestinationPath) {
  return PublishArtifact(GenerateDeclarations(Snapshot, Options),
                         DestinationPath);
}

} // namespace Luna
