#pragma once

// clang-format off
#include "state/transaction/lifecycle.hpp"

#include <cstdint>
// clang-format on

namespace Luna {
class State;
}

namespace Luna::Detail {

struct StateHandleToken final {
  State *Owner = nullptr;

  StateIdentity Identity;
  std::uint64_t OwnerEpoch = 0;
};

} // namespace Luna::Detail
