#pragma once

// clang-format off
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna {

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

  [[nodiscard]] const std::string &Bytes() const noexcept { return BytesValue; }

  [[nodiscard]] std::size_t Size() const noexcept { return BytesValue.size(); }

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
