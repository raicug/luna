#pragma once

// clang-format off
#include <luna/binding/callable_descriptor.hpp>
#include <luna/reflection/ids.hpp>

#include "state/dispatch/generation.hpp"
#include "state/identity/symbol_descriptor.hpp"
#include "state/type/type_generation.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {

class FaultInjector;

struct OverloadCandidate final {
  ErasedCallableDescriptor Descriptor;
  CallableSignatureDescriptor Signature;
  SymbolId Identity;

  bool IsCommitted = false;

  OverloadCandidate(ErasedCallableDescriptor DescriptorValue,
                    CallableSignatureDescriptor SignatureValue,
                    SymbolId IdentityValue)
      : Descriptor(std::move(DescriptorValue)),
        Signature(std::move(SignatureValue)), Identity(IdentityValue) {}
};

[[nodiscard]] inline bool
OverloadCandidatePrecedes(const OverloadCandidate &Left,
                          const OverloadCandidate &Right) {
  const auto Order = CompareSignature(Left.Signature, Right.Signature);
  if (Order != std::strong_ordering::equal)
    return Order == std::strong_ordering::less;
  return Left.Identity < Right.Identity;
}

class BindingRecord final {
public:
  BindingRecord(std::string GlobalNameValue,
                ErasedCallableDescriptor DescriptorValue,
                CallableSignatureDescriptor SignatureValue,
                SymbolId IdentityValue, FaultInjector &FaultsValue,
                const TypeGenerationSource &TypesValue,
                DispatchTable &DispatchValue, DispatchSlotId SlotValue)
      : GlobalNameStorage(std::move(GlobalNameValue)),
        FaultsStorage(&FaultsValue), TypesStorage(&TypesValue),
        DispatchStorage(&DispatchValue), SlotStorage(SlotValue) {
    AppendCandidate(std::move(DescriptorValue), std::move(SignatureValue),
                    IdentityValue);
  }

  BindingRecord(const BindingRecord &) = delete;
  BindingRecord &operator=(const BindingRecord &) = delete;

  [[nodiscard]] const std::string &GlobalName() const noexcept {
    return GlobalNameStorage;
  }

  [[nodiscard]] bool IsCommitted() const noexcept {
    for (const auto &Candidate : Candidates) {
      if (Candidate && Candidate->IsCommitted)
        return true;
    }
    return false;
  }

  [[nodiscard]] bool HasStagedCandidate() const noexcept {
    for (const auto &Candidate : Candidates) {
      if (Candidate && !Candidate->IsCommitted)
        return true;
    }
    return false;
  }

  [[nodiscard]] std::size_t CandidateCount() const noexcept {
    return Candidates.size();
  }

  [[nodiscard]] std::size_t CommittedCandidateCount() const noexcept {
    std::size_t Result = 0;
    for (const auto &Candidate : Candidates) {
      if (Candidate && Candidate->IsCommitted)
        ++Result;
    }
    return Result;
  }

  [[nodiscard]] OverloadCandidate *CandidateAt(std::size_t Index) noexcept {
    return Index < Candidates.size() ? Candidates[Index].get() : nullptr;
  }

  [[nodiscard]] const OverloadCandidate *
  CandidateAt(std::size_t Index) const noexcept {
    return Index < Candidates.size() ? Candidates[Index].get() : nullptr;
  }

  [[nodiscard]] OverloadCandidate *PrimaryCandidate() noexcept {
    for (auto &Candidate : Candidates) {
      if (Candidate && Candidate->IsCommitted)
        return Candidate.get();
    }
    return Candidates.empty() ? nullptr : Candidates.front().get();
  }

  [[nodiscard]] const OverloadCandidate *PrimaryCandidate() const noexcept {
    for (const auto &Candidate : Candidates) {
      if (Candidate && Candidate->IsCommitted)
        return Candidate.get();
    }
    return Candidates.empty() ? nullptr : Candidates.front().get();
  }

  [[nodiscard]] FaultInjector *Faults() const noexcept { return FaultsStorage; }

  [[nodiscard]] DispatchSlotId Slot() const noexcept { return SlotStorage; }

  [[nodiscard]] DispatchTable *Dispatch() const noexcept {
    return DispatchStorage;
  }

  [[nodiscard]] const TypeGenerationSource *Types() const noexcept {
    return TypesStorage;
  }

  [[nodiscard]] std::shared_ptr<const TypeGeneration>
  CaptureTypeGeneration() const {
    return TypesStorage ? TypesStorage->Capture() : nullptr;
  }

private:
  friend class BindingStore;

  OverloadCandidate *AppendCandidate(ErasedCallableDescriptor DescriptorValue,
                                     CallableSignatureDescriptor SignatureValue,
                                     SymbolId IdentityValue) {
    Candidates.push_back(std::make_unique<OverloadCandidate>(
        std::move(DescriptorValue), std::move(SignatureValue), IdentityValue));
    return Candidates.back().get();
  }

  void CommitStagedCandidates() {
    for (auto &Candidate : Candidates) {
      if (Candidate)
        Candidate->IsCommitted = true;
    }
    std::stable_sort(Candidates.begin(), Candidates.end(),
                     [](const std::unique_ptr<OverloadCandidate> &Left,
                        const std::unique_ptr<OverloadCandidate> &Right) {
                       if (!Left || !Right)
                         return Left != nullptr;
                       return OverloadCandidatePrecedes(*Left, *Right);
                     });
  }

  bool DiscardStagedCandidates() noexcept {
    const std::size_t Before = Candidates.size();
    Candidates.erase(std::remove_if(Candidates.begin(), Candidates.end(),
                                    [](const auto &Candidate) {
                                      return !Candidate ||
                                             !Candidate->IsCommitted;
                                    }),
                     Candidates.end());
    return Candidates.size() != Before;
  }

  std::string GlobalNameStorage;
  std::vector<std::unique_ptr<OverloadCandidate>> Candidates;
  FaultInjector *FaultsStorage = nullptr;
  const TypeGenerationSource *TypesStorage = nullptr;
  DispatchTable *DispatchStorage = nullptr;
  DispatchSlotId SlotStorage;
};

} // namespace Luna::Detail
