#pragma once

// clang-format off
#include "state/transaction/generation_set.hpp"
#include "state/transaction/lifecycle.hpp"

#include <cstdint>
#include <memory>
#include <thread>
// clang-format on

namespace Luna::Detail {

struct TransactionCapture final {
  std::thread::id OwnerThread;
  bool VirtualMachineIsReady = false;
  LifecyclePhase Phase = LifecyclePhase::Ready;

  int EntryStackDepth = 0;

  StateIdentity Identity;
  std::uint64_t OwnerEpoch = 0;
  std::uint64_t LifecycleGeneration = 0;

  std::shared_ptr<const GenerationSet> Generations;

  [[nodiscard]] bool IsOwnerThread() const noexcept;

  [[nodiscard]] bool IsFrozen() const noexcept {
    return Phase == LifecyclePhase::Frozen;
  }

  [[nodiscard]] bool AllowsMutation() const noexcept;

  [[nodiscard]] std::shared_ptr<const GenerationSet> SharedGenerations() const;
};

[[nodiscard]] TransactionCapture UnavailableCapture();

} // namespace Luna::Detail
