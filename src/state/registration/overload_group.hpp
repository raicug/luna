#pragma once

// Grouping callable candidates into one overload set at registration time.
//
// Candidates that share one canonical qualified name form one overload set. A
// second declaration of the same name is therefore not automatically a
// duplicate: it is a duplicate only when no call could ever tell it apart from
// a candidate the name already owns.
//
// Two candidate signatures are distinguishable when a call can select between
// them - their receiver, their arity window, or an ordered parameter type
// differs. The return type alone never distinguishes them, because a Luau call
// site supplies arguments and never names the return type it wants.

// clang-format off
#include "state/identity/symbol_descriptor.hpp"
#include "state/transaction/generation_set.hpp"

#include <string_view>
// clang-format on

namespace Luna::Detail {

// How one callable declaration relates to the symbols its qualified name
// already owns.
enum class OverloadJoinStatus {
  // The name owns no symbol yet, so the declaration is the first candidate.
  FirstCandidate,

  // The name owns callable candidates and this signature differs from every one
  // of them, so the declaration joins their overload set.
  JoinsOverloadSet,

  // A candidate of the name accepts exactly the same call shape, so no call
  // could select between them.
  IndistinguishableCandidate,

  // The name is owned by a symbol of another category.
  OtherCategory
};

[[nodiscard]] std::string_view
OverloadJoinStatusText(OverloadJoinStatus Status) noexcept;

// Two candidate signatures can be told apart by a call.
[[nodiscard]] bool
SignaturesAreDistinguishable(const CallableSignatureDescriptor &Left,
                             const CallableSignatureDescriptor &Right);

// Classifies one callable declaration against the committed and pending symbols
// of the active transaction.
[[nodiscard]] OverloadJoinStatus
ClassifyOverloadJoin(const SymbolView &Symbols, std::string_view QualifiedName,
                     const CallableSignatureDescriptor &Candidate);

} // namespace Luna::Detail
