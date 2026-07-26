#pragma once

// Stable dispatch indirection: the permanent identity one installed native
// closure carries, and the immutable generation an invocation resolves that
// identity through.
//
// A closure installed in the virtual machine holds exactly one
// `DispatchSlotId`. It holds no callable record, no target, and no metadata
// address, so nothing a later lifecycle operation may retire can ever be
// reached through the closure itself.
//
// One slot is issued per canonical callable path and is permanent for the whole
// life of its State: a path that is registered, rolled back, and registered
// again keeps the same slot identity. What changes is the entry the current
// dispatch generation holds for that slot.
//
// A generation is immutable. It owns one entry per issued slot with everything
// one invocation of that slot needs: the canonical qualified name it reports
// diagnostics under, the callable target it dispatches to, the fault context
// its callback-stack restoration is recorded through, and the canonical type
// source it captures its type generation from. An invocation retains the whole
// generation at entry, so the records it needs stay valid for the entire call
// even if a later publication replaces the generation meanwhile.
//
// An entry whose target is absent is unavailable: its slot exists, but no
// invocation can reach a callable through it, and the call fails
// deterministically instead of following a stale pointer. Removal is always
// expressed this way - a new immutable generation in which the slot holds an
// unavailable entry - never by mutating a published generation and never by
// releasing the slot identity itself.
//
// Retention decides reclamation. Whoever needs an old generation to stay
// readable holds a `DispatchRetention` of it: an invocation for the whole call
// it began, a userdata release step for the cleanup metadata it still has to
// run, and a lifecycle journal for the undo it may still have to perform. A
// superseded generation stays alive while any of the three retains it, and the
// table reclaims it only once none of them does.
//
// Synchronization is private and narrow. One latch guards this table's own
// storage - the issued slots, the current generation pointer, and the journal
// of superseded generations - and is never held while native code, Luau code,
// or any callback runs, so a call that publishes or retires from inside another
// call cannot deadlock. The latch grants nothing about the virtual machine:
// every VM-backed operation stays owner-thread-only exactly as before.

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

// The permanent dispatch identity of one canonical callable path. Zero is the
// identity no slot is ever issued, so an absent or foreign closure payload can
// never resolve to a callable.
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

// One entry of one immutable dispatch generation.
struct DispatchEntry final {
  DispatchSlotId Slot;
  std::string QualifiedName;

  // The callable target of this slot in this generation. The reflection kernel
  // still spells one target as the foundation's binding record; the record is
  // owned by the State's callable store, never by a closure.
  BindingRecord *Target = nullptr;

  // The cleanup and capture metadata one invocation of this slot needs, so the
  // retained generation alone is enough to run the call to completion.
  FaultInjector *Faults = nullptr;
  const TypeGenerationSource *Types = nullptr;

  [[nodiscard]] bool IsAvailable() const noexcept { return Target != nullptr; }
};

// Who is keeping one immutable generation readable. Reclamation waits for all
// three, so naming them is what makes "no one retains it any more" observable
// rather than assumed.
enum class DispatchRetainer : std::uint8_t {
  // One call that already began. It must finish under the generation it entered
  // under, so it is never retargeted mid-call.
  Invocation,

  // One userdata release step whose cleanup metadata belongs to the generation
  // the value was published under.
  UserdataCleanup,

  // One lifecycle journal that may still have to undo the publication it
  // recorded.
  LifecycleJournal,
};

inline constexpr std::size_t DispatchRetainerKinds = 3;

// How many retentions of each kind are live. The ledger is shared ownership
// rather than a member reference, so a retention that outlives its table still
// releases its own count safely instead of writing through a dangling address.
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

// One held generation. While it lives, the generation it names cannot be
// reclaimed, and the entries it resolves stay exactly as they were published.
// It is move-only: retention is one accounted claim, never a silently copied
// one.
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

  // The number of the generation this claim holds, or zero when it holds none.
  [[nodiscard]] std::uint64_t GenerationNumber() const noexcept;

  // The entry this retained generation holds for one slot, or null when the
  // slot is unissued in it. The pointer stays valid for as long as this
  // retention does.
  [[nodiscard]] const DispatchEntry *Find(DispatchSlotId Slot) const noexcept;

  // Gives the claim back early. Releasing twice changes nothing.
  void Release() noexcept;

private:
  std::shared_ptr<const DispatchGeneration> HeldGeneration;
  std::shared_ptr<DispatchRetentionLedger> Ledger;
  DispatchRetainer RetainerKind = DispatchRetainer::Invocation;
};

class DispatchGeneration final {
public:
  // The generation every State starts from: every slot is still unissued.
  [[nodiscard]] static std::shared_ptr<const DispatchGeneration> Empty();

  // The successor of `Current` in which `Bound` is the entry of its slot. An
  // existing entry for that slot is replaced; `Current` is never mutated.
  // Null when `Bound` names no slot: there is no successor to publish, so the
  // current generation stays exactly what it is.
  [[nodiscard]] static std::shared_ptr<const DispatchGeneration>
  Derive(const DispatchGeneration &Current, DispatchEntry Bound);

  // The successor of `Current` holding exactly `Bound`, ordered by slot. Used
  // by a publication that replaces every entry at once.
  [[nodiscard]] static std::shared_ptr<const DispatchGeneration>
  Derive(const DispatchGeneration &Current, std::vector<DispatchEntry> Bound);

  // The successor of `Current` in which `Slot` resolves to an unavailable
  // entry. The slot keeps its permanent identity, and it keeps the canonical
  // name it was published under - `CanonicalName` names it when this generation
  // holds no entry for it yet - so the refusal a stale closure receives still
  // names the symbol it was installed for.
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

  // Ordered by slot identity, so resolution never depends on the order the
  // slots were issued in.
  std::vector<DispatchEntry> Entries;
};

// The private latch of one dispatch table. It is held only across Luna's own
// bookkeeping - never across a callback, a native target, or a virtual-machine
// operation - so publishing or retiring from inside a running call is safe, and
// it claims nothing about which thread may touch the virtual machine.
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

// The dispatch indirection of one State: the permanent slot identity of every
// canonical callable path, and the immutable generation every invocation
// resolves through.
//
// The table itself lives as long as its State's callable store, which outlives
// the virtual machine. Publication replaces the whole generation in one step;
// an invocation retains it once at entry and keeps it for the entire call.
class DispatchTable final {
public:
  DispatchTable();

  DispatchTable(const DispatchTable &) = delete;
  DispatchTable &operator=(const DispatchTable &) = delete;

  // The permanent slot of one canonical callable path, issuing it when the path
  // has never owned one. The same path always answers with the same slot.
  [[nodiscard]] DispatchSlotId SlotFor(std::string_view QualifiedName);

  [[nodiscard]] std::optional<DispatchSlotId>
  FindSlot(std::string_view QualifiedName) const noexcept;

  [[nodiscard]] std::size_t IssuedSlotCount() const noexcept;

  // Publishes a generation in which `Slot` resolves to `Target` with the
  // metadata one invocation of it needs.
  void Bind(DispatchSlotId Slot, std::string QualifiedName,
            BindingRecord *Target, FaultInjector *Faults,
            const TypeGenerationSource *Types);

  // Publishes a generation in which `Slot` holds an immutable unavailable
  // entry. The slot keeps its permanent identity and its canonical name, so a
  // later registration of the same path reuses it and a stale closure refuses
  // deterministically. Retiring an already unavailable slot changes nothing.
  void Retire(DispatchSlotId Slot);

  // Publishes a generation in which every issued slot holds an unavailable
  // entry. If even that cannot be prepared, the empty generation is published
  // instead: the safe answer is always fewer reachable callables, never a slot
  // that still names storage its owner gave back.
  void RetireEverything() noexcept;

  // The current generation, retained without accounting. Used where nothing
  // outlives the call that asks; anything that has to keep a generation
  // readable across steps retains it instead.
  [[nodiscard]] std::shared_ptr<const DispatchGeneration> Capture() const;

  // One accounted claim on the current generation. The generation it holds
  // cannot be reclaimed until the returned retention is gone.
  [[nodiscard]] DispatchRetention Retain(DispatchRetainer Retainer) const;

  [[nodiscard]] std::uint64_t Generation() const noexcept;

  // How many claims of one kind, and of every kind, are live right now.
  [[nodiscard]] std::size_t
  RetainerCount(DispatchRetainer Retainer) const noexcept;
  [[nodiscard]] std::size_t TotalRetainerCount() const noexcept;

  // How many superseded generations this table still journals, and how many of
  // those something still retains. A journaled generation that nothing retains
  // is reclaimable; one that is retained is not, which is exactly the wait
  // Requirement 17.6 describes.
  [[nodiscard]] std::size_t SupersededGenerationCount() const noexcept;
  [[nodiscard]] std::size_t RetainedGenerationCount() const noexcept;
  [[nodiscard]] bool IsGenerationRetained(std::uint64_t Number) const noexcept;

  // The numbers of the superseded generations something still retains, in
  // ascending order. It is what lets a lifecycle analysis name the generations
  // it has to keep readable without ever naming their storage.
  [[nodiscard]] std::vector<std::uint64_t> RetainedGenerationNumbers() const;

  // Releases every superseded generation nothing retains any more and answers
  // how many were released. A retained one is left exactly as it is.
  std::size_t ReclaimUnretained() noexcept;

  // The target one slot resolves to in the current generation, or null when the
  // slot is unissued or unavailable. Nothing is retained.
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

  // Slot zero is never issued, so the first path receives one.
  std::uint64_t NextSlot = 1;
  std::vector<SlotName> Slots;
  std::shared_ptr<const DispatchGeneration> Current;

  // Every generation this table has replaced and not yet reclaimed. The journal
  // is what makes retention observable; shared ownership is what makes it
  // correct, so losing a journal entry can never shorten a retained
  // generation's life.
  std::vector<std::shared_ptr<const DispatchGeneration>> Superseded;

  std::shared_ptr<DispatchRetentionLedger> Ledger;
};

} // namespace Luna::Detail
