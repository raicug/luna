#pragma once

// Pareto selection over candidate rank sequences.
//
// One viable candidate is described by one ordered rank sequence: one rank per
// supplied argument, plus one final shape element that says whether the
// candidate matched the received count exactly, left optional or defaulted
// slots omitted, or consumed a variadic tail. Every element is compared as its
// own dimension; nothing is ever summed into a score.
//
// Candidate A dominates candidate B only when A is no worse at every compared
// position and strictly better in at least one. A candidate is selected only
// when it dominates every other viable candidate, so two candidates whose rank
// sequences are equal or merely incomparable stay ambiguous even where an
// arbitrary weighted sum would have picked one. There is no registration-order
// tie break and no candidate-count limit.

// clang-format off
#include <luna/binding/conversion.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

// The shape dimension of one rank sequence, ordered from the most specific
// match to the least.
enum class SignatureShapeRank {
  // The candidate's fixed arity is exactly the received count.
  ExactArity,

  // The candidate accepted the call by leaving trailing optional or defaulted
  // parameters omitted.
  OmittedParameters,

  // The candidate accepted the call by consuming a variadic tail.
  VariadicConsumption
};

[[nodiscard]] std::string_view
SignatureShapeRankText(SignatureShapeRank Shape) noexcept;

[[nodiscard]] std::string_view ConversionRankText(ConversionRank Rank) noexcept;

// The ordered rank sequence of one viable candidate.
struct CandidateRankSequence final {
  std::vector<ConversionRank> Positions;
  SignatureShapeRank Shape = SignatureShapeRank::ExactArity;
};

// One viable candidate: its position in the canonical candidate order and its
// rank sequence.
struct ViableCandidate final {
  std::size_t Candidate = 0;
  CandidateRankSequence Ranks;
};

// Pareto relation between two rank sequences.
enum class DominanceOrdering {
  // No worse everywhere and strictly better somewhere.
  Better,
  // Strictly worse somewhere and no better anywhere.
  Worse,
  // Identical at every compared position.
  Equivalent,
  // Better in one position and worse in another, so neither dominates.
  Incomparable
};

[[nodiscard]] std::string_view
DominanceOrderingText(DominanceOrdering Ordering) noexcept;

[[nodiscard]] DominanceOrdering
CompareRankSequences(const CandidateRankSequence &Left,
                     const CandidateRankSequence &Right);

// `Left` is Pareto-better than `Right`.
[[nodiscard]] bool Dominates(const CandidateRankSequence &Left,
                             const CandidateRankSequence &Right);

enum class OverloadSelectionStatus {
  // Exactly one viable candidate dominates every other viable candidate.
  Selected,
  // No candidate is viable for the received arguments.
  NoViableCandidate,
  // Several viable candidates are non-dominated.
  Ambiguous
};

[[nodiscard]] std::string_view
OverloadSelectionStatusText(OverloadSelectionStatus Status) noexcept;

struct OverloadSelection final {
  OverloadSelectionStatus Status = OverloadSelectionStatus::NoViableCandidate;

  // The selected candidate position, meaningful only when one was selected.
  std::size_t SelectedCandidate = 0;

  // The non-dominated viable candidates, in the canonical order they were
  // supplied. A selection reports exactly one; an ambiguity reports the whole
  // frontier.
  std::vector<std::size_t> Frontier;

  [[nodiscard]] bool HasSelection() const noexcept {
    return Status == OverloadSelectionStatus::Selected;
  }
};

// Selects the candidate that dominates every other viable candidate, or reports
// the non-dominated frontier. `Viable` must be supplied in canonical candidate
// order; the selection never depends on that order, but the reported frontier
// preserves it.
[[nodiscard]] OverloadSelection
SelectByDominance(std::span<const ViableCandidate> Viable);

} // namespace Luna::Detail
