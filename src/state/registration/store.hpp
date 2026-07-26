#pragma once

// The callable store of one State: one record per canonical callable path, and
// one overload set inside each record.
//
// A declaration whose qualified name already owns a record stages one more
// candidate in that record instead of a second record, because one qualified
// name owns exactly one installed closure and one overload set behind it.
// Rollback discards exactly the candidates the failed attempt staged and erases
// the record only when the attempt created it.

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

  // The canonical type generation every invocation of this State captures at
  // entry. Publication replaces it in one step; nothing else mutates it, and a
  // State that has published no type generation observes the migrated
  // foundation one.
  [[nodiscard]] const TypeGenerationSource &Types() const noexcept {
    return TypesValue;
  }

  void PublishTypes(std::shared_ptr<const TypeGeneration> Published) {
    TypesValue.Publish(std::move(Published));
  }

  // The dispatch indirection of this State: the permanent slot of every
  // canonical callable path and the immutable generation every invocation
  // resolves through. Installed closures carry only a slot of this table.
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

  // Stages one candidate of the overload set that owns `GlobalName`, creating
  // the record when this is the first candidate of the name. The staged
  // candidate stays invisible to dispatch until publication commits it.
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

    // The path receives its permanent dispatch slot here, once and for the
    // lifetime of this State. A path that is staged, rolled back, and staged
    // again resolves through exactly the same slot identity.
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

    // The staged record becomes the target of its slot immediately, exactly as
    // it becomes findable in this store immediately. Dispatch still refuses it
    // until publication commits a candidate, so no ordinary invocation can
    // reach staged work.
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

  // Discards every candidate the open attempt staged in `Expected`. The record
  // itself survives when the overload set still holds a committed candidate,
  // because that candidate belongs to an earlier published generation.
  [[nodiscard]] bool Rollback(std::string_view GlobalName,
                              const BindingRecord *Expected) noexcept {
    for (auto Iterator = Records.begin(); Iterator != Records.end();
         ++Iterator) {
      if (Iterator->first != GlobalName || Iterator->second.get() != Expected ||
          !Iterator->second->HasStagedCandidate())
        continue;
      const bool Discarded = Iterator->second->DiscardStagedCandidates();
      if (Iterator->second->CandidateCount() == 0) {
        // The record the failed attempt created is gone, so its slot must stop
        // naming it before it dies. The slot itself is permanent: it keeps its
        // identity and its canonical name and simply becomes unavailable, which
        // is what makes any closure that survived the failure fail
        // deterministically instead of following a released record.
        const DispatchSlotId Slot = Iterator->second->Slot();
        Records.erase(Iterator);
        RetireSlot(Slot);
      }
      return Discarded;
    }
    return false;
  }

private:
  // Retires one slot without ever leaving it naming a released record. Under
  // allocation failure the safe answer is fewer reachable callables, never a
  // slot that still resolves to storage this store has given back.
  void RetireSlot(DispatchSlotId Slot) noexcept {
    try {
      DispatchValue.Retire(Slot);
    } catch (...) {
      DispatchValue.RetireEverything();
    }
  }

  // Declared before the records so every record's captured source and the
  // dispatch generation that names it outlive the records themselves.
  DispatchTable DispatchValue;
  TypeGenerationSource TypesValue;
  std::unordered_map<std::string, std::unique_ptr<BindingRecord>> Records;
};

} // namespace Luna::Detail
