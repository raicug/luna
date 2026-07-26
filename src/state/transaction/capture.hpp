#pragma once

// What one registration transaction observes at entry. Capture is the first
// phase of every transaction: it takes the owner thread, the readiness and
// freeze phase, the entry stack depth, the logical State identity with its
// epochs, and the current immutable generation set once, so every later
// validation, preparation, and restoration decision of the attempt reads one
// consistent picture instead of re-reading mutable State.
//
// A capture holds no virtual-machine resource. It may be copied freely and it
// stays meaningful after the attempt ends, which is what lets rollback restore
// the exact entry stack depth and lets validation reject a stale handle.

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

  // Depth of the owner's root stack at entry. Restoration returns the stack to
  // exactly this depth.
  int EntryStackDepth = 0;

  StateIdentity Identity;
  std::uint64_t OwnerEpoch = 0;
  std::uint64_t LifecycleGeneration = 0;

  // The committed generation set the attempt validates and publishes against.
  std::shared_ptr<const GenerationSet> Generations;

  // True when the calling thread is the owner thread of the captured State.
  [[nodiscard]] bool IsOwnerThread() const noexcept;

  [[nodiscard]] bool IsFrozen() const noexcept {
    return Phase == LifecyclePhase::Frozen;
  }

  // A mutation is admissible only on the owner thread of a ready, unfrozen
  // State. Readiness and phase are separate rejections, so this is a summary
  // rather than the diagnostic order.
  [[nodiscard]] bool AllowsMutation() const noexcept;

  // Never null: a capture without a generation set observes the initial one.
  [[nodiscard]] std::shared_ptr<const GenerationSet> SharedGenerations() const;
};

// The capture of a State that is not ready, taken on the calling thread. It
// rejects every mutation, which is what an attempt against a moved-from or
// unavailable implementation observes.
[[nodiscard]] TransactionCapture UnavailableCapture();

} // namespace Luna::Detail
