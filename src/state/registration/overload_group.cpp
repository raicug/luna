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
  // A receiver is part of the call shape: an instance member and a static one
  // of the same name are told apart by the receiver alone.
  if (Left.ReceiverType.has_value() != Right.ReceiverType.has_value())
    return true;
  if (Left.ReceiverType && !(*Left.ReceiverType == *Right.ReceiverType))
    return true;
  if (Left.ReceiverType && Left.ReceiverIsConst != Right.ReceiverIsConst)
    return true;

  // The arity window and the variadic tail are shape rules, so a difference in
  // either lets some received argument count select between the two.
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

  // Everything a call site can express is identical. The return type is
  // deliberately not compared: a Luau call site never names it.
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

    // A callable symbol without a canonical signature cannot be compared, so
    // the declaration is treated as indistinguishable rather than admitted.
    if (!Entry.Symbol || !Entry.Symbol->Signature)
      return OverloadJoinStatus::IndistinguishableCandidate;

    if (!SignaturesAreDistinguishable(Candidate, *Entry.Symbol->Signature))
      return OverloadJoinStatus::IndistinguishableCandidate;
  }

  return OverloadJoinStatus::JoinsOverloadSet;
}

} // namespace Luna::Detail
