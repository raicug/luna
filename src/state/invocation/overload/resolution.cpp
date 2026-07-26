// clang-format off
#include "state/invocation/overload/resolution.hpp"

#include <luna/binding/conversion.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

// Ordered rank position of one category. A smaller value is a better match, and
// the order is exactly the required one: exact, then safe built-in, then
// registered user conversion.
[[nodiscard]] constexpr int RankOrder(ConversionRank Rank) noexcept {
  switch (Rank) {
  case ConversionRank::Exact:
    return 0;
  case ConversionRank::SafeBuiltIn:
    return 1;
  case ConversionRank::User:
    return 2;
  }
  return 3;
}

[[nodiscard]] constexpr int ShapeOrder(SignatureShapeRank Shape) noexcept {
  switch (Shape) {
  case SignatureShapeRank::ExactArity:
    return 0;
  case SignatureShapeRank::OmittedParameters:
    return 1;
  case SignatureShapeRank::VariadicConsumption:
    return 2;
  }
  return 3;
}

// Accumulates one compared dimension into the running Pareto relation.
void Accumulate(int Left, int Right, bool &SawBetter, bool &SawWorse) {
  if (Left < Right)
    SawBetter = true;
  else if (Left > Right)
    SawWorse = true;
}

} // namespace

std::string_view SignatureShapeRankText(SignatureShapeRank Shape) noexcept {
  switch (Shape) {
  case SignatureShapeRank::ExactArity:
    return "exact_arity";
  case SignatureShapeRank::OmittedParameters:
    return "omitted_parameters";
  case SignatureShapeRank::VariadicConsumption:
    return "variadic_consumption";
  }
  return "unknown";
}

std::string_view ConversionRankText(ConversionRank Rank) noexcept {
  switch (Rank) {
  case ConversionRank::Exact:
    return "exact";
  case ConversionRank::SafeBuiltIn:
    return "safe_built_in";
  case ConversionRank::User:
    return "user";
  }
  return "unknown";
}

std::string_view DominanceOrderingText(DominanceOrdering Ordering) noexcept {
  switch (Ordering) {
  case DominanceOrdering::Better:
    return "better";
  case DominanceOrdering::Worse:
    return "worse";
  case DominanceOrdering::Equivalent:
    return "equivalent";
  case DominanceOrdering::Incomparable:
    return "incomparable";
  }
  return "unknown";
}

DominanceOrdering CompareRankSequences(const CandidateRankSequence &Left,
                                       const CandidateRankSequence &Right) {
  // Two candidates ranked against one call always describe the same received
  // arguments. A sequence of another length describes another call, so nothing
  // about it can be compared.
  if (Left.Positions.size() != Right.Positions.size())
    return DominanceOrdering::Incomparable;

  bool SawBetter = false;
  bool SawWorse = false;
  for (std::size_t Index = 0; Index < Left.Positions.size(); ++Index) {
    Accumulate(RankOrder(Left.Positions[Index]),
               RankOrder(Right.Positions[Index]), SawBetter, SawWorse);
  }

  // The shape element is one more dimension, never a summed score.
  Accumulate(ShapeOrder(Left.Shape), ShapeOrder(Right.Shape), SawBetter,
             SawWorse);

  if (SawBetter && SawWorse)
    return DominanceOrdering::Incomparable;
  if (SawBetter)
    return DominanceOrdering::Better;
  if (SawWorse)
    return DominanceOrdering::Worse;
  return DominanceOrdering::Equivalent;
}

bool Dominates(const CandidateRankSequence &Left,
               const CandidateRankSequence &Right) {
  return CompareRankSequences(Left, Right) == DominanceOrdering::Better;
}

std::string_view
OverloadSelectionStatusText(OverloadSelectionStatus Status) noexcept {
  switch (Status) {
  case OverloadSelectionStatus::Selected:
    return "selected";
  case OverloadSelectionStatus::NoViableCandidate:
    return "no_viable_candidate";
  case OverloadSelectionStatus::Ambiguous:
    return "ambiguous";
  }
  return "unknown";
}

OverloadSelection SelectByDominance(std::span<const ViableCandidate> Viable) {
  OverloadSelection Selection;
  if (Viable.empty()) {
    Selection.Status = OverloadSelectionStatus::NoViableCandidate;
    return Selection;
  }

  // The frontier is every viable candidate no other viable candidate dominates.
  for (std::size_t Index = 0; Index < Viable.size(); ++Index) {
    bool IsDominated = false;
    for (std::size_t Other = 0; Other < Viable.size() && !IsDominated;
         ++Other) {
      if (Other == Index)
        continue;
      IsDominated = Dominates(Viable[Other].Ranks, Viable[Index].Ranks);
    }
    if (!IsDominated)
      Selection.Frontier.push_back(Viable[Index].Candidate);
  }

  // Selection is stricter than being on the frontier: the candidate must be
  // better than every other viable candidate, so an equivalent or incomparable
  // rival prevents it.
  for (std::size_t Index = 0; Index < Viable.size(); ++Index) {
    bool DominatesEveryOther = true;
    for (std::size_t Other = 0; Other < Viable.size() && DominatesEveryOther;
         ++Other) {
      if (Other == Index)
        continue;
      DominatesEveryOther = Dominates(Viable[Index].Ranks, Viable[Other].Ranks);
    }
    if (!DominatesEveryOther)
      continue;
    Selection.Status = OverloadSelectionStatus::Selected;
    Selection.SelectedCandidate = Viable[Index].Candidate;
    Selection.Frontier.assign(1, Viable[Index].Candidate);
    return Selection;
  }

  Selection.Status = OverloadSelectionStatus::Ambiguous;
  return Selection;
}

} // namespace Luna::Detail
