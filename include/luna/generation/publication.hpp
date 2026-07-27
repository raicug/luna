#pragma once

// clang-format off
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/generation/artifact.hpp>
#include <luna/generation/declaration.hpp>
#include <luna/generation/documentation.hpp>
#include <luna/reflection/reflection_snapshot.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna {

enum class PublicationStatus {
  Published,
  Unspecified,
  IncompleteArtifact,
  NonCanonicalArtifact,
  InvalidDestination,
  DestinationUnavailable
};

[[nodiscard]] constexpr std::string_view
PublicationStatusText(PublicationStatus Status) noexcept {
  switch (Status) {
  case PublicationStatus::Published:
    return "published";
  case PublicationStatus::Unspecified:
    return "unspecified";
  case PublicationStatus::IncompleteArtifact:
    return "incomplete-artifact";
  case PublicationStatus::NonCanonicalArtifact:
    return "non-canonical-artifact";
  case PublicationStatus::InvalidDestination:
    return "invalid-destination";
  case PublicationStatus::DestinationUnavailable:
    return "destination-unavailable";
  }
  return "invalid";
}

class ArtifactPublication final {
public:
  ArtifactPublication()
      : DiagnosticValue(ErrorDiagnostic::Create(
            ErrorCategory::Internal, "No artifact has been published.")) {}

  [[nodiscard]] static ArtifactPublication Complete(std::size_t Size) {
    ArtifactPublication Result;
    Result.StatusValue = PublicationStatus::Published;
    Result.SizeValue = Size;
    Result.RefusedValue = false;
    return Result;
  }

  [[nodiscard]] static ArtifactPublication Refused(PublicationStatus Status,
                                                   std::string Message) {
    ArtifactPublication Result;
    Result.StatusValue = Status == PublicationStatus::Published
                             ? PublicationStatus::Unspecified
                             : Status;
    Result.DiagnosticValue =
        ErrorDiagnostic::Create(ErrorCategory::Internal, std::move(Message));
    return Result;
  }

  [[nodiscard]] PublicationStatus Status() const noexcept {
    return StatusValue;
  }

  [[nodiscard]] bool IsPublished() const noexcept { return !RefusedValue; }

  [[nodiscard]] std::size_t Size() const noexcept { return SizeValue; }

  [[nodiscard]] const ErrorDiagnostic *Diagnostic() const noexcept {
    return RefusedValue ? &DiagnosticValue : nullptr;
  }

private:
  PublicationStatus StatusValue = PublicationStatus::Unspecified;
  bool RefusedValue = true;
  std::size_t SizeValue = 0;
  ErrorDiagnostic DiagnosticValue;
};

[[nodiscard]] ArtifactPublication
PublishArtifact(const GeneratedArtifact &Artifact,
                std::string_view DestinationPath);

[[nodiscard]] ArtifactPublication
PublishDocumentation(const ReflectionSnapshot &Snapshot,
                     const DocumentationOptions &Options,
                     std::string_view DestinationPath);

[[nodiscard]] ArtifactPublication
PublishDeclarations(const ReflectionSnapshot &Snapshot,
                    const DeclarationOptions &Options,
                    std::string_view DestinationPath);

} // namespace Luna
