// clang-format off
#include "state/transaction/lifecycle.hpp"

#include <atomic>
#include <cstdint>
// clang-format on

namespace Luna::Detail {

StateIdentity StateIdentity::Next() noexcept {
  static std::atomic<std::uint64_t> Counter{0};
  StateIdentity Identity;
  Identity.ValueStorage = Counter.fetch_add(1, std::memory_order_relaxed) + 1;
  return Identity;
}

} // namespace Luna::Detail
