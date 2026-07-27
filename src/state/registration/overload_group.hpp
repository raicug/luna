#pragma once

// clang-format off
#include "state/identity/symbol_descriptor.hpp"
#include "state/transaction/generation_set.hpp"

#include <string_view>
// clang-format on

namespace Luna::Detail {

enum class OverloadJoinStatus {
  FirstCandidate,

  JoinsOverloadSet,

  IndistinguishableCandidate,

  OtherCategory
};

[[nodiscard]] std::string_view
OverloadJoinStatusText(OverloadJoinStatus Status) noexcept;

[[nodiscard]] bool
SignaturesAreDistinguishable(const CallableSignatureDescriptor &Left,
                             const CallableSignatureDescriptor &Right);

[[nodiscard]] OverloadJoinStatus
ClassifyOverloadJoin(const SymbolView &Symbols, std::string_view QualifiedName,
                     const CallableSignatureDescriptor &Candidate);

} // namespace Luna::Detail
