#pragma once

// Caller-facing publication of one generated artifact. Every generator builds
// and validates its whole artifact in an unpublished owned byte buffer first,
// so this service never assembles output itself: it revalidates the complete
// bytes it was handed, writes them to one unpublished file beside the requested
// destination, verifies that file byte-for-byte, and only then replaces the
// destination in one step.
//
// Nothing here refers to a State, a virtual machine, a stack index, or a native
// object, because publication reads only an already complete artifact.
//
// On any refusal - an incomplete artifact, non-canonical bytes, an unusable
// destination, or a failure while writing, verifying, or replacing - no partial
// artifact is exposed, no unpublished file survives, and any prior destination
// keeps its exact previous bytes.

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

// Deterministic reason one publication attempt is accepted or refused.
// `IncompleteArtifact` means the artifact carried a generation rejection and
// therefore no bytes at all, `NonCanonicalArtifact` that the handed bytes are
// not canonical UTF-8 without a byte-order mark and with LF line endings,
// `InvalidDestination` that the named destination cannot name a replaceable
// file, and `DestinationUnavailable` that the unpublished file could not be
// created, written, verified, or moved over the destination.
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

// The outcome of one publication attempt. A default constructed value is the
// reserved unspecified outcome: nothing was published and it says so, so a
// caller can never mistake it for a replaced destination.
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

  // The exact number of bytes the destination now holds. A refused attempt
  // published nothing and reports zero.
  [[nodiscard]] std::size_t Size() const noexcept { return SizeValue; }

  // The non-empty diagnostic of a refused attempt, or nothing when the
  // destination was replaced.
  [[nodiscard]] const ErrorDiagnostic *Diagnostic() const noexcept {
    return RefusedValue ? &DiagnosticValue : nullptr;
  }

private:
  PublicationStatus StatusValue = PublicationStatus::Unspecified;
  bool RefusedValue = true;
  std::size_t SizeValue = 0;
  ErrorDiagnostic DiagnosticValue;
};

// Atomically replaces `DestinationPath` with the complete bytes of `Artifact`.
// The artifact is revalidated first, the bytes are written and verified in one
// unpublished file beside the destination, and the destination is replaced in
// one step only after that file matches the artifact exactly. Any refusal
// leaves the prior destination byte-for-byte unchanged and removes the
// unpublished file.
[[nodiscard]] ArtifactPublication
PublishArtifact(const GeneratedArtifact &Artifact,
                std::string_view DestinationPath);

// Generates the documentation of one captured reflection generation and
// publishes it atomically. A generation rejection refuses publication before
// the destination is touched at all.
[[nodiscard]] ArtifactPublication
PublishDocumentation(const ReflectionSnapshot &Snapshot,
                     const DocumentationOptions &Options,
                     std::string_view DestinationPath);

// Generates the Luau declarations of one captured reflection generation and
// publishes them atomically, under the same all-or-nothing rules.
[[nodiscard]] ArtifactPublication
PublishDeclarations(const ReflectionSnapshot &Snapshot,
                    const DeclarationOptions &Options,
                    std::string_view DestinationPath);

} // namespace Luna
