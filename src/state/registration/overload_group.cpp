// clang-format off
#include "state/registration/overload_group.hpp"

#include "state/identity/symbol_descriptor.hpp"
#include "state/registration/plan.hpp"
#include "state/transaction/generation_set.hpp"

#include <cstddef>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

std::string_view OverloadJoinStatusText(OverloadJoinStatus Status) noexcept {
  switch (Status) {
  case OverloadJoinStatus::FirstCandidate:
    return "first_candidate";
  case OverloadJoinStatus::JoinsOverloadSet:
    return "joins_overload_set";
  case OverloadJoinStatus::IndistinguishableCandidate:
    return "indistinguishable_candidate";
  case OverloadJoinStatus::OtherCategory:
    return "other_category";
  }
  return "unknown";
}

bool SignaturesAreDistinguishable(const CallableSignatureDescriptor &Left,
                                  const CallableSignatureDescriptor &Right) {
  if (Left.ReceiverType.has_value() != Right.ReceiverType.has_value())
    return true;
  if (Left.ReceiverType && !(*Left.ReceiverType == *Right.ReceiverType))
    return true;
  if (Left.ReceiverType && Left.ReceiverIsConst != Right.ReceiverIsConst)
    return true;

  if (Left.IsVariadic != Right.IsVariadic)
    return true;
  if (Left.RequiredParameterCount != Right.RequiredParameterCount)
    return true;
  if (Left.ParameterTypes.size() != Right.ParameterTypes.size())
    return true;

  for (std::size_t Index = 0; Index < Left.ParameterTypes.size(); ++Index) {
    if (!(Left.ParameterTypes[Index] == Right.ParameterTypes[Index]))
      return true;
  }

  return false;
}

OverloadJoinStatus
ClassifyOverloadJoin(const SymbolView &Symbols, std::string_view QualifiedName,
                     const CallableSignatureDescriptor &Candidate) {
  const std::vector<SymbolViewEntry> Existing = Symbols.FindAll(QualifiedName);
  if (Existing.empty())
    return OverloadJoinStatus::FirstCandidate;

  for (const SymbolViewEntry &Entry : Existing) {
    if (Entry.Category != PlanEntryKind::Function)
      return OverloadJoinStatus::OtherCategory;

    if (!Entry.Symbol || !Entry.Symbol->Signature)
      return OverloadJoinStatus::IndistinguishableCandidate;

    if (!SignaturesAreDistinguishable(Candidate, *Entry.Symbol->Signature))
      return OverloadJoinStatus::IndistinguishableCandidate;
  }

  return OverloadJoinStatus::JoinsOverloadSet;
}

} // namespace Luna::Detail
