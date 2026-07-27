#pragma once

// clang-format off
#include "state/dispatch/generation.hpp"
#include "state/identity/symbol_descriptor.hpp"
#include "state/registration/record.hpp"
#include "state/type/type_generation.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
// clang-format on

namespace Luna::Detail {

class FaultInjector;

class BindingStore final {
public:
  [[nodiscard]] std::size_t Count() const noexcept { return Records.size(); }

  [[nodiscard]] const TypeGenerationSource &Types() const noexcept {
    return TypesValue;
  }

  void PublishTypes(std::shared_ptr<const TypeGeneration> Published) {
    TypesValue.Publish(std::move(Published));
  }

  [[nodiscard]] DispatchTable &Dispatch() noexcept { return DispatchValue; }

  [[nodiscard]] const DispatchTable &Dispatch() const noexcept {
    return DispatchValue;
  }

  [[nodiscard]] std::size_t PendingCount() const noexcept {
    std::size_t Result = 0;
    for (const auto &[Name, Record] : Records) {
      static_cast<void>(Name);
      if (Record->HasStagedCandidate())
        ++Result;
    }
    return Result;
  }

  [[nodiscard]] bool Contains(std::string_view GlobalName) const noexcept {
    return Find(GlobalName) != nullptr;
  }

  [[nodiscard]] BindingRecord *Find(std::string_view GlobalName) noexcept {
    for (auto &[StoredName, Record] : Records) {
      if (StoredName == GlobalName)
        return Record.get();
    }
    return nullptr;
  }

  [[nodiscard]] const BindingRecord *
  Find(std::string_view GlobalName) const noexcept {
    for (const auto &[StoredName, Record] : Records) {
      if (StoredName == GlobalName)
        return Record.get();
    }
    return nullptr;
  }

  [[nodiscard]] BindingRecord *Prepare(std::string GlobalName,
                                       ErasedCallableDescriptor Descriptor,
                                       CallableSignatureDescriptor Signature,
                                       SymbolId Identity,
                                       FaultInjector &Faults) {
    if (BindingRecord *Existing = Find(GlobalName)) {
      const OverloadCandidate *Staged = Existing->AppendCandidate(
          std::move(Descriptor), std::move(Signature), Identity);
      return Staged ? Existing : nullptr;
    }

    const DispatchSlotId Slot = DispatchValue.SlotFor(GlobalName);

    Records.reserve(Records.size() + 1);
    auto Record = std::make_unique<BindingRecord>(
        std::move(GlobalName), std::move(Descriptor), std::move(Signature),
        Identity, Faults, TypesValue, DispatchValue, Slot);
    auto *Address = Record.get();
    const auto [Position, Inserted] =
        Records.try_emplace(Address->GlobalName(), std::move(Record));
    static_cast<void>(Position);
    if (!Inserted)
      return nullptr;

    const std::string Name = Address->GlobalName();
    try {
      DispatchValue.Bind(Slot, Name, Address, &Faults, &TypesValue);
    } catch (...) {
      Records.erase(Name);
      throw;
    }
    return Address;
  }

  void Commit(BindingRecord &Record) noexcept {
    Record.CommitStagedCandidates();
  }

  [[nodiscard]] bool Rollback(std::string_view GlobalName,
                              const BindingRecord *Expected) noexcept {
    for (auto Iterator = Records.begin(); Iterator != Records.end();
         ++Iterator) {
      if (Iterator->first != GlobalName || Iterator->second.get() != Expected ||
          !Iterator->second->HasStagedCandidate())
        continue;
      const bool Discarded = Iterator->second->DiscardStagedCandidates();
      if (Iterator->second->CandidateCount() == 0) {
        const DispatchSlotId Slot = Iterator->second->Slot();
        Records.erase(Iterator);
        RetireSlot(Slot);
      }
      return Discarded;
    }
    return false;
  }

private:
  void RetireSlot(DispatchSlotId Slot) noexcept {
    try {
      DispatchValue.Retire(Slot);
    } catch (...) {
      DispatchValue.RetireEverything();
    }
  }

  DispatchTable DispatchValue;
  TypeGenerationSource TypesValue;
  std::unordered_map<std::string, std::unique_ptr<BindingRecord>> Records;
};

} // namespace Luna::Detail
