// clang-format off
#include "state/transaction/capture.hpp"

#include "state/transaction/generation_set.hpp"

#include <memory>
#include <thread>
// clang-format on

namespace Luna::Detail {

bool TransactionCapture::IsOwnerThread() const noexcept {
  return std::this_thread::get_id() == OwnerThread;
}

bool TransactionCapture::AllowsMutation() const noexcept {
  return VirtualMachineIsReady && !IsFrozen() && IsOwnerThread();
}

std::shared_ptr<const GenerationSet>
TransactionCapture::SharedGenerations() const {
  return Generations ? Generations : GenerationSet::Initial();
}

TransactionCapture UnavailableCapture() {
  TransactionCapture Capture;
  Capture.OwnerThread = std::this_thread::get_id();
  Capture.VirtualMachineIsReady = false;
  Capture.Generations = GenerationSet::Initial();
  return Capture;
}

} // namespace Luna::Detail
