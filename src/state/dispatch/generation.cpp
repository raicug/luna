// clang-format off
#include "state/dispatch/generation.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] bool SlotPrecedes(const DispatchEntry &Entry,
                                DispatchSlotId Slot) noexcept {
  return Entry.Slot < Slot;
}

[[nodiscard]] bool EntryPrecedes(const DispatchEntry &Left,
                                 const DispatchEntry &Right) noexcept {
  return Left.Slot < Right.Slot;
}

[[nodiscard]] std::size_t RetainerIndex(DispatchRetainer Retainer) noexcept {
  const auto Index = static_cast<std::size_t>(Retainer);
  return Index < DispatchRetainerKinds ? Index : 0;
}

} // namespace

void DispatchRetentionLedger::Acquire(DispatchRetainer Retainer) noexcept {
  Live[RetainerIndex(Retainer)].fetch_add(1, std::memory_order_acq_rel);
}

void DispatchRetentionLedger::Release(DispatchRetainer Retainer) noexcept {
  std::atomic<std::size_t> &Counted = Live[RetainerIndex(Retainer)];
  std::size_t Observed = Counted.load(std::memory_order_acquire);
  while (Observed != 0 && !Counted.compare_exchange_weak(
                              Observed, Observed - 1, std::memory_order_acq_rel,
                              std::memory_order_acquire)) {
  }
}

std::size_t
DispatchRetentionLedger::Count(DispatchRetainer Retainer) const noexcept {
  return Live[RetainerIndex(Retainer)].load(std::memory_order_acquire);
}

std::size_t DispatchRetentionLedger::Total() const noexcept {
  std::size_t Result = 0;
  for (const std::atomic<std::size_t> &Counted : Live)
    Result += Counted.load(std::memory_order_acquire);
  return Result;
}

DispatchRetention::DispatchRetention(
    std::shared_ptr<const DispatchGeneration> Held,
    std::shared_ptr<DispatchRetentionLedger> LedgerValue,
    DispatchRetainer Retainer) noexcept
    : HeldGeneration(std::move(Held)), Ledger(std::move(LedgerValue)),
      RetainerKind(Retainer) {
  if (HeldGeneration && Ledger)
    Ledger->Acquire(RetainerKind);
  else
    Ledger.reset();
}

DispatchRetention::~DispatchRetention() {
  Release();
}

DispatchRetention::DispatchRetention(DispatchRetention &&Other) noexcept
    : HeldGeneration(std::move(Other.HeldGeneration)),
      Ledger(std::move(Other.Ledger)), RetainerKind(Other.RetainerKind) {
  Other.HeldGeneration.reset();
  Other.Ledger.reset();
}

DispatchRetention &
DispatchRetention::operator=(DispatchRetention &&Other) noexcept {
  if (this == &Other)
    return *this;
  Release();
  HeldGeneration = std::move(Other.HeldGeneration);
  Ledger = std::move(Other.Ledger);
  RetainerKind = Other.RetainerKind;
  Other.HeldGeneration.reset();
  Other.Ledger.reset();
  return *this;
}

std::uint64_t DispatchRetention::GenerationNumber() const noexcept {
  return HeldGeneration ? HeldGeneration->Generation() : 0;
}

const DispatchEntry *
DispatchRetention::Find(DispatchSlotId Slot) const noexcept {
  return HeldGeneration ? HeldGeneration->Find(Slot) : nullptr;
}

void DispatchRetention::Release() noexcept {
  if (Ledger)
    Ledger->Release(RetainerKind);
  Ledger.reset();

  HeldGeneration.reset();
}

std::shared_ptr<const DispatchGeneration> DispatchGeneration::Empty() {
  static const std::shared_ptr<const DispatchGeneration> Value =
      std::shared_ptr<const DispatchGeneration>(new DispatchGeneration());
  return Value;
}

std::shared_ptr<const DispatchGeneration>
DispatchGeneration::Derive(const DispatchGeneration &Current,
                           DispatchEntry Bound) {
  if (!Bound.Slot.IsValid())
    return nullptr;

  auto Derived = std::shared_ptr<DispatchGeneration>(new DispatchGeneration());
  Derived->GenerationValue = Current.GenerationValue + 1;
  Derived->Entries = Current.Entries;

  const auto Position =
      std::lower_bound(Derived->Entries.begin(), Derived->Entries.end(),
                       Bound.Slot, SlotPrecedes);
  if (Position != Derived->Entries.end() && Position->Slot == Bound.Slot)
    *Position = std::move(Bound);
  else
    Derived->Entries.insert(Position, std::move(Bound));
  return Derived;
}

std::shared_ptr<const DispatchGeneration>
DispatchGeneration::Derive(const DispatchGeneration &Current,
                           std::vector<DispatchEntry> Bound) {
  auto Derived = std::shared_ptr<DispatchGeneration>(new DispatchGeneration());
  Derived->GenerationValue = Current.GenerationValue + 1;
  Derived->Entries = std::move(Bound);
  std::sort(Derived->Entries.begin(), Derived->Entries.end(), EntryPrecedes);
  return Derived;
}

std::shared_ptr<const DispatchGeneration>
DispatchGeneration::Retire(const DispatchGeneration &Current,
                           DispatchSlotId Slot,
                           std::string_view CanonicalName) {
  if (!Slot.IsValid())
    return nullptr;

  DispatchEntry Retired;
  if (const DispatchEntry *Existing = Current.Find(Slot)) {
    Retired = *Existing;
  } else {
    Retired.Slot = Slot;
    Retired.QualifiedName = std::string(CanonicalName);
  }
  Retired.Target = nullptr;
  return Derive(Current, std::move(Retired));
}

std::size_t DispatchGeneration::AvailableCount() const noexcept {
  std::size_t Result = 0;
  for (const DispatchEntry &Entry : Entries) {
    if (Entry.IsAvailable())
      ++Result;
  }
  return Result;
}

const DispatchEntry *
DispatchGeneration::Find(DispatchSlotId Slot) const noexcept {
  if (!Slot.IsValid())
    return nullptr;
  const auto Position =
      std::lower_bound(Entries.begin(), Entries.end(), Slot, SlotPrecedes);
  if (Position == Entries.end() || !(Position->Slot == Slot))
    return nullptr;
  return &*Position;
}

DispatchTable::DispatchTable()
    : Current(DispatchGeneration::Empty()),
      Ledger(std::make_shared<DispatchRetentionLedger>()) {}

DispatchSlotId DispatchTable::SlotFor(std::string_view QualifiedName) {
  DispatchLatchGuard Guard(Latch);
  for (const SlotName &Named : Slots) {
    if (Named.QualifiedName == QualifiedName)
      return Named.Slot;
  }

  const DispatchSlotId Issued{NextSlot};
  Slots.push_back(SlotName{std::string(QualifiedName), Issued});
  ++NextSlot;
  return Issued;
}

std::optional<DispatchSlotId>
DispatchTable::FindSlot(std::string_view QualifiedName) const noexcept {
  DispatchLatchGuard Guard(Latch);
  for (const SlotName &Named : Slots) {
    if (Named.QualifiedName == QualifiedName)
      return Named.Slot;
  }
  return std::nullopt;
}

std::size_t DispatchTable::IssuedSlotCount() const noexcept {
  DispatchLatchGuard Guard(Latch);
  return Slots.size();
}

void DispatchTable::Bind(DispatchSlotId Slot, std::string QualifiedName,
                         BindingRecord *Target, FaultInjector *Faults,
                         const TypeGenerationSource *Types,
                         ProfilingRegistry *Profiling) {
  if (!Slot.IsValid())
    return;

  DispatchEntry Entry;
  Entry.Slot = Slot;
  Entry.QualifiedName = std::move(QualifiedName);
  Entry.Target = Target;
  Entry.Faults = Faults;
  Entry.Types = Types;
  Entry.Profiling = Profiling;

  DispatchLatchGuard Guard(Latch);
  const std::shared_ptr<const DispatchGeneration> Captured = CurrentLocked();

  PublishLocked(DispatchGeneration::Derive(*Captured, std::move(Entry)));
}

void DispatchTable::Retire(DispatchSlotId Slot) {
  if (!Slot.IsValid())
    return;

  DispatchLatchGuard Guard(Latch);
  const std::string *Named = NameForLocked(Slot);
  if (!Named)
    return;

  const std::shared_ptr<const DispatchGeneration> Captured = CurrentLocked();
  const DispatchEntry *Existing = Captured->Find(Slot);
  if (Existing && !Existing->IsAvailable())
    return;

  PublishLocked(DispatchGeneration::Retire(*Captured, Slot, *Named));
}

void DispatchTable::RetireEverything() noexcept {
  DispatchLatchGuard Guard(Latch);
  const std::shared_ptr<const DispatchGeneration> Captured = CurrentLocked();
  try {
    std::vector<DispatchEntry> Unavailable;
    Unavailable.reserve(Slots.size());
    for (const SlotName &Named : Slots) {
      DispatchEntry Entry;
      if (const DispatchEntry *Existing = Captured->Find(Named.Slot))
        Entry = *Existing;
      else {
        Entry.Slot = Named.Slot;
        Entry.QualifiedName = Named.QualifiedName;
      }
      Entry.Target = nullptr;
      Unavailable.push_back(std::move(Entry));
    }
    PublishLocked(
        DispatchGeneration::Derive(*Captured, std::move(Unavailable)));
  } catch (...) {
    PublishLocked(DispatchGeneration::Empty());
  }
}

bool DispatchTable::Publish(
    std::shared_ptr<const DispatchGeneration> Published) noexcept {
  if (!Published)
    return false;

  DispatchLatchGuard Guard(Latch);
  if (Published == Current)
    return true;
  if (Current && Published->Generation() <= Current->Generation())
    return false;

  PublishLocked(std::move(Published));
  return true;
}
std::shared_ptr<const DispatchGeneration> DispatchTable::Capture() const {
  DispatchLatchGuard Guard(Latch);
  return CurrentLocked();
}

DispatchRetention DispatchTable::Retain(DispatchRetainer Retainer) const {
  std::shared_ptr<const DispatchGeneration> Held;
  {
    DispatchLatchGuard Guard(Latch);
    Held = CurrentLocked();
  }

  return DispatchRetention(std::move(Held), Ledger, Retainer);
}

std::uint64_t DispatchTable::Generation() const noexcept {
  DispatchLatchGuard Guard(Latch);
  return Current ? Current->Generation() : 0;
}

std::size_t
DispatchTable::RetainerCount(DispatchRetainer Retainer) const noexcept {
  return Ledger ? Ledger->Count(Retainer) : 0;
}

std::size_t DispatchTable::TotalRetainerCount() const noexcept {
  return Ledger ? Ledger->Total() : 0;
}

std::size_t DispatchTable::SupersededGenerationCount() const noexcept {
  DispatchLatchGuard Guard(Latch);
  return Superseded.size();
}

std::size_t DispatchTable::RetainedGenerationCount() const noexcept {
  DispatchLatchGuard Guard(Latch);
  std::size_t Result = 0;
  for (const std::shared_ptr<const DispatchGeneration> &Retired : Superseded) {
    if (Retired.use_count() > 1)
      ++Result;
  }
  return Result;
}

bool DispatchTable::IsGenerationRetained(std::uint64_t Number) const noexcept {
  DispatchLatchGuard Guard(Latch);
  for (const std::shared_ptr<const DispatchGeneration> &Retired : Superseded) {
    if (Retired && Retired->Generation() == Number && Retired.use_count() > 1)
      return true;
  }
  return false;
}

std::vector<std::uint64_t> DispatchTable::RetainedGenerationNumbers() const {
  std::vector<std::uint64_t> Numbers;
  {
    DispatchLatchGuard Guard(Latch);
    Numbers.reserve(Superseded.size());
    for (const std::shared_ptr<const DispatchGeneration> &Retired :
         Superseded) {
      if (Retired && Retired.use_count() > 1)
        Numbers.push_back(Retired->Generation());
    }
  }
  std::sort(Numbers.begin(), Numbers.end());
  Numbers.erase(std::unique(Numbers.begin(), Numbers.end()), Numbers.end());
  return Numbers;
}

std::size_t DispatchTable::ReclaimUnretained() noexcept {
  DispatchLatchGuard Guard(Latch);
  return ReclaimUnretainedLocked();
}

BindingRecord *DispatchTable::Resolve(DispatchSlotId Slot) const noexcept {
  DispatchLatchGuard Guard(Latch);
  const DispatchEntry *Entry = Current ? Current->Find(Slot) : nullptr;
  return Entry ? Entry->Target : nullptr;
}

std::shared_ptr<const DispatchGeneration> DispatchTable::CurrentLocked() const {
  return Current ? Current : DispatchGeneration::Empty();
}

const std::string *
DispatchTable::NameForLocked(DispatchSlotId Slot) const noexcept {
  for (const SlotName &Named : Slots) {
    if (Named.Slot == Slot)
      return &Named.QualifiedName;
  }
  return nullptr;
}

void DispatchTable::PublishLocked(
    std::shared_ptr<const DispatchGeneration> Published) noexcept {
  if (!Published)
    return;

  std::shared_ptr<const DispatchGeneration> Previous =
      std::exchange(Current, std::move(Published));
  JournalSupersededLocked(std::move(Previous));
  static_cast<void>(ReclaimUnretainedLocked());
}

void DispatchTable::JournalSupersededLocked(
    std::shared_ptr<const DispatchGeneration> Previous) noexcept {
  if (!Previous || Previous == Current)
    return;

  if (Previous == DispatchGeneration::Empty())
    return;

  try {
    Superseded.push_back(std::move(Previous));
  } catch (...) {
  }
}

std::size_t DispatchTable::ReclaimUnretainedLocked() noexcept {
  std::size_t Reclaimed = 0;
  for (auto Position = Superseded.begin(); Position != Superseded.end();) {
    if (Position->use_count() == 1) {
      Position = Superseded.erase(Position);
      ++Reclaimed;
    } else {
      ++Position;
    }
  }
  return Reclaimed;
}

} // namespace Luna::Detail
