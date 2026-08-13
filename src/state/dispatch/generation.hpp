#pragma once

// clang-format off
#include "state/type/type_generation.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

class BindingRecord;
class FaultInjector;
class ProfilingRegistry;

struct DispatchSlotId final {
  std::uint64_t Value = 0;

  [[nodiscard]] bool IsValid() const noexcept { return Value != 0; }

  [[nodiscard]] friend bool operator==(const DispatchSlotId &Left,
                                       const DispatchSlotId &Right) noexcept {
    return Left.Value == Right.Value;
  }

  [[nodiscard]] friend bool operator<(const DispatchSlotId &Left,
                                      const DispatchSlotId &Right) noexcept {
    return Left.Value < Right.Value;
  }
};

struct DispatchEntry final {
  DispatchSlotId Slot;
  std::string QualifiedName;

  BindingRecord *Target = nullptr;

  FaultInjector *Faults = nullptr;
  const TypeGenerationSource *Types = nullptr;
  ProfilingRegistry *Profiling = nullptr;

  [[nodiscard]] bool IsAvailable() const noexcept { return Target != nullptr; }
};

enum class DispatchRetainer : std::uint8_t {
  Invocation,

  UserdataCleanup,

  LifecycleJournal,
};

inline constexpr std::size_t DispatchRetainerKinds = 3;

class DispatchRetentionLedger final {
public:
  void Acquire(DispatchRetainer Retainer) noexcept;
  void Release(DispatchRetainer Retainer) noexcept;

  [[nodiscard]] std::size_t Count(DispatchRetainer Retainer) const noexcept;
  [[nodiscard]] std::size_t Total() const noexcept;

private:
  std::array<std::atomic<std::size_t>, DispatchRetainerKinds> Live{};
};

class DispatchGeneration;

class DispatchRetention final {
public:
  DispatchRetention() = default;
  DispatchRetention(std::shared_ptr<const DispatchGeneration> Held,
                    std::shared_ptr<DispatchRetentionLedger> Ledger,
                    DispatchRetainer Retainer) noexcept;
  ~DispatchRetention();

  DispatchRetention(const DispatchRetention &) = delete;
  DispatchRetention &operator=(const DispatchRetention &) = delete;
  DispatchRetention(DispatchRetention &&Other) noexcept;
  DispatchRetention &operator=(DispatchRetention &&Other) noexcept;

  [[nodiscard]] bool IsHeld() const noexcept {
    return HeldGeneration != nullptr;
  }

  [[nodiscard]] std::uint64_t GenerationNumber() const noexcept;

  [[nodiscard]] const DispatchEntry *Find(DispatchSlotId Slot) const noexcept;

  void Release() noexcept;

private:
  std::shared_ptr<const DispatchGeneration> HeldGeneration;
  std::shared_ptr<DispatchRetentionLedger> Ledger;
  DispatchRetainer RetainerKind = DispatchRetainer::Invocation;
};

class DispatchGeneration final {
public:
  [[nodiscard]] static std::shared_ptr<const DispatchGeneration> Empty();

  [[nodiscard]] static std::shared_ptr<const DispatchGeneration>
  Derive(const DispatchGeneration &Current, DispatchEntry Bound);

  [[nodiscard]] static std::shared_ptr<const DispatchGeneration>
  Derive(const DispatchGeneration &Current, std::vector<DispatchEntry> Bound);

  [[nodiscard]] static std::shared_ptr<const DispatchGeneration>
  Retire(const DispatchGeneration &Current, DispatchSlotId Slot,
         std::string_view CanonicalName);

  [[nodiscard]] std::uint64_t Generation() const noexcept {
    return GenerationValue;
  }

  [[nodiscard]] std::size_t Size() const noexcept { return Entries.size(); }

  [[nodiscard]] std::size_t AvailableCount() const noexcept;

  [[nodiscard]] std::span<const DispatchEntry> All() const noexcept {
    return Entries;
  }

  [[nodiscard]] const DispatchEntry *Find(DispatchSlotId Slot) const noexcept;

private:
  DispatchGeneration() = default;

  std::uint64_t GenerationValue = 0;

  std::vector<DispatchEntry> Entries;
};

class DispatchLatch final {
public:
  DispatchLatch() = default;

  DispatchLatch(const DispatchLatch &) = delete;
  DispatchLatch &operator=(const DispatchLatch &) = delete;

  void Acquire() noexcept {
    while (Held.test_and_set(std::memory_order_acquire)) {
    }
  }

  void Release() noexcept { Held.clear(std::memory_order_release); }

private:
  std::atomic_flag Held;
};

class DispatchLatchGuard final {
public:
  explicit DispatchLatchGuard(DispatchLatch &Guarded) noexcept
      : Guarded(&Guarded) {
    Guarded.Acquire();
  }

  ~DispatchLatchGuard() { Guarded->Release(); }

  DispatchLatchGuard(const DispatchLatchGuard &) = delete;
  DispatchLatchGuard &operator=(const DispatchLatchGuard &) = delete;

private:
  DispatchLatch *Guarded = nullptr;
};

class DispatchTable final {
public:
  DispatchTable();

  DispatchTable(const DispatchTable &) = delete;
  DispatchTable &operator=(const DispatchTable &) = delete;

  [[nodiscard]] DispatchSlotId SlotFor(std::string_view QualifiedName);

  [[nodiscard]] std::optional<DispatchSlotId>
  FindSlot(std::string_view QualifiedName) const noexcept;

  [[nodiscard]] std::size_t IssuedSlotCount() const noexcept;

  void Bind(DispatchSlotId Slot, std::string QualifiedName,
            BindingRecord *Target, FaultInjector *Faults,
            const TypeGenerationSource *Types,
            ProfilingRegistry *Profiling = nullptr);

  void Retire(DispatchSlotId Slot);

  void RetireEverything() noexcept;

  void FreezeForInvocation(const TypeGeneration *Types) noexcept;

  [[nodiscard]] bool HasFrozenSnapshot() const noexcept;

  [[nodiscard]] const TypeGeneration *FrozenTypes() const noexcept;

  [[nodiscard]] const DispatchEntry *
  FindFrozen(DispatchSlotId Slot) const noexcept;

  [[nodiscard]] bool
  Publish(std::shared_ptr<const DispatchGeneration> Published) noexcept;

  [[nodiscard]] std::shared_ptr<const DispatchGeneration> Capture() const;

  [[nodiscard]] DispatchRetention Retain(DispatchRetainer Retainer) const;

  [[nodiscard]] std::uint64_t Generation() const noexcept;

  [[nodiscard]] std::size_t
  RetainerCount(DispatchRetainer Retainer) const noexcept;
  [[nodiscard]] std::size_t TotalRetainerCount() const noexcept;

  [[nodiscard]] std::size_t SupersededGenerationCount() const noexcept;
  [[nodiscard]] std::size_t RetainedGenerationCount() const noexcept;
  [[nodiscard]] bool IsGenerationRetained(std::uint64_t Number) const noexcept;

  [[nodiscard]] std::vector<std::uint64_t> RetainedGenerationNumbers() const;

  std::size_t ReclaimUnretained() noexcept;

  [[nodiscard]] BindingRecord *Resolve(DispatchSlotId Slot) const noexcept;

private:
  struct SlotName final {
    std::string QualifiedName;
    DispatchSlotId Slot;
  };

  [[nodiscard]] std::shared_ptr<const DispatchGeneration> CurrentLocked() const;

  [[nodiscard]] const std::string *
  NameForLocked(DispatchSlotId Slot) const noexcept;

  void
  PublishLocked(std::shared_ptr<const DispatchGeneration> Published) noexcept;
  void JournalSupersededLocked(
      std::shared_ptr<const DispatchGeneration> Previous) noexcept;
  std::size_t ReclaimUnretainedLocked() noexcept;

  mutable DispatchLatch Latch;

  std::uint64_t NextSlot = 1;
  std::vector<SlotName> Slots;
  std::shared_ptr<const DispatchGeneration> Current;
  std::atomic<const DispatchGeneration *> Frozen{nullptr};
  std::atomic<const TypeGeneration *> FrozenTypeGeneration{nullptr};

  std::vector<std::shared_ptr<const DispatchGeneration>> Superseded;

  std::shared_ptr<DispatchRetentionLedger> Ledger;
};

} // namespace Luna::Detail
