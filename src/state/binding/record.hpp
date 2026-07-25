#pragma once

// clang-format off
#include <luna/binding/callable_descriptor.hpp>

#include <string>
#include <utility>
// clang-format on

namespace Luna::Detail {

class FaultInjector;

enum class BindingRecordState { Pending, Committed };

class BindingRecord final {
public:
  BindingRecord(std::string GlobalNameValue,
                ErasedCallableDescriptor DescriptorValue,
                FaultInjector &FaultsValue)
      : GlobalNameStorage(std::move(GlobalNameValue)),
        DescriptorStorage(std::move(DescriptorValue)),
        FaultsStorage(&FaultsValue) {}

  [[nodiscard]] const std::string &GlobalName() const noexcept {
    return GlobalNameStorage;
  }

  [[nodiscard]] bool IsCommitted() const noexcept {
    return State == BindingRecordState::Committed;
  }

  [[nodiscard]] ErasedCallableDescriptor &Descriptor() noexcept {
    return DescriptorStorage;
  }

  [[nodiscard]] FaultInjector *Faults() const noexcept { return FaultsStorage; }

private:
  friend class BindingStore;

  std::string GlobalNameStorage;
  ErasedCallableDescriptor DescriptorStorage;
  FaultInjector *FaultsStorage = nullptr;
  BindingRecordState State = BindingRecordState::Pending;
};

} // namespace Luna::Detail
