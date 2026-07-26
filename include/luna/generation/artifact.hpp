#pragma once

// One generated artifact and the deterministic reason it was accepted or
// rejected. Generation always builds a complete owned byte buffer first: a
// rejected artifact therefore exposes no partial bytes at all, and an accepted
// artifact is canonical UTF-8 without a byte-order mark and with LF line
// endings on every host. Nothing here refers to a State, a virtual machine, a
// stack index, or a native object, because every generator reads only one
// captured immutable reflection snapshot.

// clang-format off
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna {

// Deterministic reason one generation attempt is accepted or rejected. The
// reason never depends on host platform, locale, or the order the reflected
// declarations were registered in.
// `UnsupportedDeclaration` means the captured metadata describes something the
// target artifact language cannot declare at all, `UnrepresentableType` that it
// names a type that language has no representation for, and
// `InconsistentMetadata` that it contradicts itself, so no single correct
// declaration exists to emit.
enum class GenerationStatus {
  Valid,
  Unspecified,
  InvalidEncoding,
  ForbiddenByteOrderMark,
  UnsupportedDeclaration,
  UnrepresentableType,
  InconsistentMetadata
};

[[nodiscard]] constexpr std::string_view
GenerationStatusText(GenerationStatus Status) noexcept {
  switch (Status) {
  case GenerationStatus::Valid:
    return "valid";
  case GenerationStatus::Unspecified:
    return "unspecified";
  case GenerationStatus::InvalidEncoding:
    return "invalid-encoding";
  case GenerationStatus::ForbiddenByteOrderMark:
    return "forbidden-byte-order-mark";
  case GenerationStatus::UnsupportedDeclaration:
    return "unsupported-declaration";
  case GenerationStatus::UnrepresentableType:
    return "unrepresentable-type";
  case GenerationStatus::InconsistentMetadata:
    return "inconsistent-metadata";
  }
  return "invalid";
}

// One complete generated artifact, or one deterministic rejection. A default
// constructed artifact is the reserved unspecified value: it holds no bytes and
// reports why nothing was generated, so a caller can never mistake it for
// published output.
class GeneratedArtifact final {
public:
  GeneratedArtifact()
      : DiagnosticValue(ErrorDiagnostic::Create(
            ErrorCategory::Internal, "No artifact has been generated.")) {}

  [[nodiscard]] static GeneratedArtifact Complete(std::string Bytes) {
    GeneratedArtifact Artifact;
    Artifact.StatusValue = GenerationStatus::Valid;
    Artifact.BytesValue = std::move(Bytes);
    Artifact.RejectedValue = false;
    return Artifact;
  }

  [[nodiscard]] static GeneratedArtifact Rejected(GenerationStatus Status,
                                                  std::string Message) {
    GeneratedArtifact Artifact;
    Artifact.StatusValue = Status == GenerationStatus::Valid
                               ? GenerationStatus::Unspecified
                               : Status;
    Artifact.DiagnosticValue =
        ErrorDiagnostic::Create(ErrorCategory::Internal, std::move(Message));
    return Artifact;
  }

  [[nodiscard]] GenerationStatus Status() const noexcept { return StatusValue; }

  [[nodiscard]] bool IsComplete() const noexcept { return !RejectedValue; }

  // The whole artifact. A rejected attempt exposes no bytes at all.
  [[nodiscard]] const std::string &Bytes() const noexcept { return BytesValue; }

  [[nodiscard]] std::size_t Size() const noexcept { return BytesValue.size(); }

  // The non-empty diagnostic of a rejected attempt, or nothing when the
  // artifact is complete.
  [[nodiscard]] const ErrorDiagnostic *Diagnostic() const noexcept {
    return RejectedValue ? &DiagnosticValue : nullptr;
  }

private:
  GenerationStatus StatusValue = GenerationStatus::Unspecified;
  bool RejectedValue = true;
  std::string BytesValue;
  ErrorDiagnostic DiagnosticValue;
};

} // namespace Luna
