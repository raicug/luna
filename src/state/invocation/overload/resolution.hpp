#pragma once

// clang-format off
#include <luna/binding/conversion.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

enum class SignatureShapeRank {
  ExactArity,

  OmittedParameters,

  VariadicConsumption
};

[[nodiscard]] std::string_view
SignatureShapeRankText(SignatureShapeRank Shape) noexcept;

[[nodiscard]] std::string_view ConversionRankText(ConversionRank Rank) noexcept;

struct CandidateRankSequence final {
  std::vector<ConversionRank> Positions;
  SignatureShapeRank Shape = SignatureShapeRank::ExactArity;
};

struct ViableCandidate final {
  std::size_t Candidate = 0;
  CandidateRankSequence Ranks;
};

enum class DominanceOrdering { Better, Worse, Equivalent, Incomparable };

[[nodiscard]] std::string_view
DominanceOrderingText(DominanceOrdering Ordering) noexcept;

[[nodiscard]] DominanceOrdering
CompareRankSequences(const CandidateRankSequence &Left,
                     const CandidateRankSequence &Right);

[[nodiscard]] bool Dominates(const CandidateRankSequence &Left,
                             const CandidateRankSequence &Right);

enum class OverloadSelectionStatus { Selected, NoViableCandidate, Ambiguous };

[[nodiscard]] std::string_view
OverloadSelectionStatusText(OverloadSelectionStatus Status) noexcept;

struct OverloadSelection final {
  OverloadSelectionStatus Status = OverloadSelectionStatus::NoViableCandidate;

  std::size_t SelectedCandidate = 0;

  std::vector<std::size_t> Frontier;

  [[nodiscard]] bool HasSelection() const noexcept {
    return Status == OverloadSelectionStatus::Selected;
  }
};

[[nodiscard]] OverloadSelection
SelectByDominance(std::span<const ViableCandidate> Viable);

} // namespace Luna::Detail
