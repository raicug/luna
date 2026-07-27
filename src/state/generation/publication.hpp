#pragma once

namespace Luna::Detail {

enum class ArtifactPublicationFault {
  None,
  UnpublishedCreation,
  UnpublishedWrite,
  UnpublishedFlush,
  Verification,
  Replacement
};

class ScopedArtifactPublicationFault final {
public:
  explicit ScopedArtifactPublicationFault(
      ArtifactPublicationFault Fault) noexcept;

  ScopedArtifactPublicationFault(const ScopedArtifactPublicationFault &) =
      delete;
  ScopedArtifactPublicationFault &
  operator=(const ScopedArtifactPublicationFault &) = delete;
  ScopedArtifactPublicationFault(ScopedArtifactPublicationFault &&) = delete;
  ScopedArtifactPublicationFault &
  operator=(ScopedArtifactPublicationFault &&) = delete;

  ~ScopedArtifactPublicationFault();

  [[nodiscard]] bool WasConsumed() const noexcept;

private:
  ArtifactPublicationFault PreviousFault;
  bool PreviousConsumed;
};

} // namespace Luna::Detail
