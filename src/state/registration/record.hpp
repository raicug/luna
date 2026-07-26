#pragma once

// One installed callable path and the overload set behind it.
//
// A binding record owns every candidate that shares one canonical qualified
// name. One candidate is one erased native target plus the canonical signature
// and candidate `SymbolId` its declaration derived, so the record is exactly
// the dispatch-side view of one reflected overload set.
//
// Candidate order is canonical - encoded signature first, candidate `SymbolId`
// last - and never registration order, so two States that registered the same
// candidates in different orders resolve calls through one identical sequence.
//
// A staged candidate belongs to an open transaction. It is invisible to
// dispatch until publication commits it, which is what keeps pending work out
// of every ordinary invocation while the transaction is still open.

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

// One candidate of one overload set.
struct OverloadCandidate final {
  ErasedCallableDescriptor Descriptor;
  CallableSignatureDescriptor Signature;
  SymbolId Identity;

  // The candidate belongs to the committed generation. A staged candidate is
  // still owned by an open transaction and no invocation may reach it.
  bool IsCommitted = false;

  OverloadCandidate(ErasedCallableDescriptor DescriptorValue,
                    CallableSignatureDescriptor SignatureValue,
                    SymbolId IdentityValue)
      : Descriptor(std::move(DescriptorValue)),
        Signature(std::move(SignatureValue)), Identity(IdentityValue) {}
};

// Canonical order of two candidates: the encoded canonical signature first, the
// candidate identity as the final stable key. Registration order never
// participates.
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

  // The record holds at least one candidate the committed generation published,
  // so the callable is reachable from the virtual machine.
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

  // The canonically first committed candidate. A single-candidate set resolves
  // to exactly the one declaration that installed the path, which is what keeps
  // one-candidate invocation behavior unchanged.
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

  // The permanent dispatch slot of this canonical callable path. It is the only
  // thing the installed closure of the path carries, and it stays the same for
  // the whole life of its State.
  [[nodiscard]] DispatchSlotId Slot() const noexcept { return SlotStorage; }

  // The dispatch indirection this record's slot belongs to. It is owned by the
  // State's callable store and outlives both the record and the virtual
  // machine, so a closure resolves through it rather than through any record.
  [[nodiscard]] DispatchTable *Dispatch() const noexcept {
    return DispatchStorage;
  }

  // The canonical type source this path's invocations capture at entry. The
  // dispatch generation carries it too, so a retained generation is enough to
  // run one call to completion.
  [[nodiscard]] const TypeGenerationSource *Types() const noexcept {
    return TypesStorage;
  }

  // One invocation captures the current immutable type generation exactly once
  // at entry, so viability, ranking, conversion, and diagnostics stay stable
  // for the whole call even if a later registration publishes a new generation.
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

  // Publication accepts every candidate the attempt staged, and the whole set
  // is reordered canonically afterwards so dispatch never observes an order
  // that depends on when a candidate was registered.
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

  // Restoration discards exactly the candidates the failed attempt staged and
  // leaves every committed candidate of the set untouched.
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
