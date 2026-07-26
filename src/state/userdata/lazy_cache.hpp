#pragma once

// The lazy value cache of one logical State.
//
// A lazy property caches exactly one thing: the value its getter already
// produced successfully, for one exposed object, under one dispatch generation.
// Nothing else is ever recorded. A refused getter, a getter that threw, a
// computed property, and an immediate property all leave the cache untouched,
// so a cached value is always a value a getter really produced.
//
// The entries live here rather than in the virtual-machine block. A userdata
// header is trivially copyable because the virtual machine owns its memory, so
// it can carry only the two words that name one Luna-owned entry node by stable
// state-local ID and the generation those entries were produced under. The node
// itself is owned by this cache and is dropped before that value's payload is
// released.
//
// Invalidation has exactly four causes and each one is cheap:
//
//   * a successful setter or field write clears every entry of that object,
//     because Luna cannot know which computed values a native mutation changed;
//   * an explicit invalidation clears every entry of that object;
//   * retiring or releasing the exposed value drops its whole node before the
//     payload goes away;
//   * a dispatch-generation change invalidates by mismatch - the node keeps the
//     generation it was produced under, and a lookup under any other generation
//     simply does not match, so nothing is traversed at all.
//
// Reflection never reads this cache: a lazy policy is metadata, a cached value
// is runtime state, and the two never mix.

// clang-format off
#include <luna/binding/value.hpp>
#include <luna/reflection/ids.hpp>

#include "state/userdata/header.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
// clang-format on

namespace Luna::Detail {

// One cached member value of one exposed object.
struct LazyCacheEntry final {
  SymbolId Member;
  Value Cached;
};

// The Luna-owned entries of one exposed object. A stable state-local ID, not
// this replaceable container node's address, is what the header cache slot
// names.
struct LazyCacheNode final {
  // Cache ownership is explicit. The logical State and Luna-issued userdata
  // identity are stable across State moves and distinguish recycled storage.
  StateIdentity Origin;
  NativeIdentity Owner;

  // The stable state-local ID written into the VM-owned userdata header. No
  // header retains a pointer into this cache's replaceable node container.
  std::uint64_t Identity = 0;

  // The dispatch generation these entries were produced under. A lookup under
  // another generation never matches, which is how generation replacement
  // invalidates without traversal.
  std::uint64_t Generation = 0;

  std::vector<LazyCacheEntry> Entries;
};

// Exactly what the cache did. Every counter is a decision count, so an
// independent model predicts all of them from the generated access sequence.
struct LazyCacheCounters final {
  std::uint64_t Hit = 0;
  std::uint64_t Miss = 0;
  std::uint64_t GenerationMismatch = 0;
  std::uint64_t Store = 0;
  std::uint64_t Replace = 0;
  std::uint64_t Invalidate = 0;
  std::uint64_t Drop = 0;
};

class LazyPropertyCache final {
public:
  explicit LazyPropertyCache(StateIdentity Owner) noexcept
      : OwnerIdentity(Owner) {}

  LazyPropertyCache(const LazyPropertyCache &) = delete;
  LazyPropertyCache &operator=(const LazyPropertyCache &) = delete;
  LazyPropertyCache(LazyPropertyCache &&) = delete;
  LazyPropertyCache &operator=(LazyPropertyCache &&) = delete;

  ~LazyPropertyCache() = default;

  // The value one member of one exposed object already produced under this
  // dispatch generation, or null. A generation mismatch is a miss and is
  // counted as one, and nothing is mutated: this is a probe.
  [[nodiscard]] const Value *Observe(const UserdataHeader &Header,
                                     const SymbolId &Member,
                                     std::uint64_t Generation) noexcept;

  // Records one successful getter result. A node whose generation no longer
  // matches is re-keyed and emptied first, so a stale generation never leaves a
  // value behind. The header's cache slot is updated to name the node and the
  // generation, which is what makes the mismatch check possible at all.
  [[nodiscard]] bool Store(UserdataHeader &Header, const SymbolId &Member,
                           const Value &Produced, std::uint64_t Generation);

  // Every entry of one exposed object, dropped. This is what a successful
  // setter or field write and an explicit invalidation both perform: Luna
  // cannot know which computed values a native mutation changed, so it keeps
  // none of them.
  std::size_t InvalidateOwner(UserdataHeader &Header) noexcept;

  // One member's entry of one exposed object, dropped.
  std::size_t InvalidateMember(UserdataHeader &Header,
                               const SymbolId &Member) noexcept;

  // The whole node of one exact exposed identity, dropped. An older release
  // cannot drop a newer object that happens to reuse the same address.
  bool Drop(const NativeIdentity &Owner, UserdataHeader *Header) noexcept;

  // Every entry of every object, dropped. State destruction performs it while
  // the values it describes are still being released.
  std::size_t Clear() noexcept;

  [[nodiscard]] std::size_t NodeCount() const noexcept { return Nodes.size(); }

  // How many entries exist at all, and how many of those still match one
  // dispatch generation. The difference is exactly what a generation change
  // invalidated by mismatch.
  [[nodiscard]] std::size_t EntryCount() const noexcept;
  [[nodiscard]] std::size_t
  LiveEntryCount(std::uint64_t Generation) const noexcept;
  [[nodiscard]] std::size_t EntryCountOf(const void *Owner) const noexcept;

  // The generation the entries of one exposed object were produced under, or
  // zero when it has no node.
  [[nodiscard]] std::uint64_t GenerationOf(const void *Owner) const noexcept;

  [[nodiscard]] LazyCacheCounters Counters() const noexcept { return Counted; }
  void ResetCounters() noexcept { Counted = LazyCacheCounters(); }

private:
  [[nodiscard]] LazyCacheNode *Find(const NativeIdentity &Owner) noexcept;
  [[nodiscard]] const LazyCacheNode *
  Find(const NativeIdentity &Owner) const noexcept;
  [[nodiscard]] const LazyCacheNode *
  FindAddress(const void *Owner) const noexcept;

  // The node the header's stable slot names, but only when the State and exact
  // userdata identity also agree.
  [[nodiscard]] LazyCacheNode *Resolve(const UserdataHeader &Header) noexcept;

  static void Publish(UserdataHeader *Header, const LazyCacheNode &Node,
                      std::uint64_t Generation) noexcept;
  static void Withdraw(UserdataHeader *Header) noexcept;

  StateIdentity OwnerIdentity;
  std::uint64_t NextNodeIdentity = 0;
  std::vector<std::unique_ptr<LazyCacheNode>> Nodes;
  LazyCacheCounters Counted;
};

} // namespace Luna::Detail
