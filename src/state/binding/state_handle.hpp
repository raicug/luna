#pragma once

// Shared liveness and ownership token of one State implementation.
//
// A builder must be able to fail safely rather than dereference a State that no
// longer exists, so it never keeps a bare pointer to its owner. The token lives
// with the implementation: it dies when the implementation dies, and it records
// which owner object currently holds the implementation together with the
// owner-object epoch and logical State identity of that ownership.
//
// A builder captures a weak reference to the token plus the owner object,
// epoch, identity, and lifecycle generation it was created with. An expired
// token means the implementation is gone; a different owner object or epoch
// means the implementation moved to another owner. Both are detected before the
// builder touches the State, which is what makes use after destruction and use
// after a move deterministic instead of undefined.

// clang-format off
#include "state/transaction/lifecycle.hpp"

#include <cstdint>
// clang-format on

namespace Luna {
class State;
}

namespace Luna::Detail {

struct StateHandleToken final {
  // The owner object that currently holds the implementation.
  State *Owner = nullptr;

  // Logical identity of the implementation and the epoch of the current
  // ownership. Both are refreshed whenever a new owner object takes over.
  StateIdentity Identity;
  std::uint64_t OwnerEpoch = 0;
};

} // namespace Luna::Detail
