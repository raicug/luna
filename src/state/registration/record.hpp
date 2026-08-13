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
#include <span>
#include <string>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {

class FaultInjector;
class ProfilingRegistry;

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

struct PrimitiveRootInvocation final {
  const PrimitiveInvocationPlan *Plan = nullptr;
  const ReturnMetadata *Return = nullptr;
  std::span<const ValueKind> Parameters;
  SymbolId Symbol;

  [[nodiscard]] bool IsAvailable() const noexcept {
    return Plan != nullptr && Return != nullptr;
  }
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
                DispatchTable &DispatchValue, DispatchSlotId SlotValue,
                ProfilingRegistry *ProfilingValue)
      : GlobalNameStorage(std::move(GlobalNameValue)),
        FaultsStorage(&FaultsValue), TypesStorage(&TypesValue),
        DispatchStorage(&DispatchValue), SlotStorage(SlotValue),
        ProfilingStorage(ProfilingValue) {
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

  [[nodiscard]] ProfilingRegistry *Profiling() const noexcept {
    return ProfilingStorage;
  }

  [[nodiscard]] const PrimitiveRootInvocation *PrimitiveRoot() const noexcept {
    return PrimitiveRootStorage.IsAvailable() ? &PrimitiveRootStorage : nullptr;
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
    PrimitiveRootStorage = {};
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
    RefreshPrimitiveRoot();
  }

  void RefreshPrimitiveRoot() noexcept {
    PrimitiveRootStorage = {};
    if (CommittedCandidateCount() != 1)
      return;

    OverloadCandidate *Candidate = PrimaryCandidate();
    if (!Candidate || !Candidate->IsCommitted)
      return;

    const PrimitiveInvocationPlan *Plan =
        Candidate->Descriptor.PrimitiveInvocation();
    const CallableMetadata &Metadata = Candidate->Descriptor.Metadata();
    const ReturnMetadata &Return = Metadata.ReturnType();
    const std::span<const ValueKind> Parameters = Metadata.ParameterTypes();
    if (!Plan || !Plan->IsAvailable() || Metadata.HasReceiver() ||
        Candidate->Signature.ReceiverType || Metadata.HasRichParameters() ||
        Return.IsAsynchronous() || Parameters.size() > 4)
      return;
    if (Return.Disposition() != ReturnDisposition::Void &&
        Return.Disposition() != ReturnDisposition::Value &&
        Return.Disposition() != ReturnDisposition::Pack)
      return;
    if (Return.Disposition() == ReturnDisposition::Value &&
        (!Return.Kind() || !IsPrimitive(*Return.Kind())))
      return;
    if (Return.Disposition() == ReturnDisposition::Pack &&
        (Return.HasDeclaredPackShape() || !Plan->HasPackInvoke()))
      return;
    if (Return.Disposition() != ReturnDisposition::Pack &&
        !Plan->HasScalarInvoke())
      return;
    for (const ValueKind Parameter : Parameters) {
      if (!IsPrimitive(Parameter))
        return;
    }

    PrimitiveRootStorage = {.Plan = Plan,
                            .Return = &Return,
                            .Parameters = Parameters,
                            .Symbol = Candidate->Identity};
  }

  [[nodiscard]] static bool IsPrimitive(ValueKind Kind) noexcept {
    return Kind == ValueKind::Boolean || Kind == ValueKind::Integer ||
           Kind == ValueKind::Number;
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
  PrimitiveRootInvocation PrimitiveRootStorage;
  FaultInjector *FaultsStorage = nullptr;
  const TypeGenerationSource *TypesStorage = nullptr;
  DispatchTable *DispatchStorage = nullptr;
  DispatchSlotId SlotStorage;
  ProfilingRegistry *ProfilingStorage = nullptr;
};

} // namespace Luna::Detail
