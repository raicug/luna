#pragma once

// Private failure injection for artifact publication. Publication touches a
// real destination, so the only reliable way to prove that every stage leaves a
// prior destination byte-for-byte unchanged is to fail each stage on demand.
// The hook is thread-local and scoped, so one test never disturbs another and
// no injected failure outlives the scope that stated it.

namespace Luna::Detail {

// The stages of one publication that can fail after the artifact itself has
// been accepted. Each one is reached only while an unpublished file is being
// produced, so none of them can ever have replaced the destination.
enum class ArtifactPublicationFault {
  None,
  // Creating the unpublished file beside the destination.
  UnpublishedCreation,
  // Writing the complete artifact bytes into that file.
  UnpublishedWrite,
  // Flushing and closing that file.
  UnpublishedFlush,
  // Reading that file back and comparing it with the artifact.
  Verification,
  // Moving the verified file over the destination.
  Replacement
};

// States the one stage the next publication on this thread must fail at, and
// restores the previous injection when it goes out of scope.
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

  // Whether publication actually reached the stated stage.
  [[nodiscard]] bool WasConsumed() const noexcept;

private:
  ArtifactPublicationFault PreviousFault;
  bool PreviousConsumed;
};

} // namespace Luna::Detail
