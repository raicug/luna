#pragma once

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

struct LazyCacheEntry final {
  SymbolId Member;
  Value Cached;
};

struct LazyCacheNode final {
  StateIdentity Origin;
  NativeIdentity Owner;

  std::uint64_t Identity = 0;

  std::uint64_t Generation = 0;

  std::vector<LazyCacheEntry> Entries;
};

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

  [[nodiscard]] const Value *Observe(const UserdataHeader &Header,
                                     const SymbolId &Member,
                                     std::uint64_t Generation) noexcept;

  [[nodiscard]] bool Store(UserdataHeader &Header, const SymbolId &Member,
                           const Value &Produced, std::uint64_t Generation);

  std::size_t InvalidateOwner(UserdataHeader &Header) noexcept;

  std::size_t InvalidateMember(UserdataHeader &Header,
                               const SymbolId &Member) noexcept;

  bool Drop(const NativeIdentity &Owner, UserdataHeader *Header) noexcept;

  std::size_t Clear() noexcept;

  [[nodiscard]] std::size_t NodeCount() const noexcept { return Nodes.size(); }

  [[nodiscard]] std::size_t EntryCount() const noexcept;
  [[nodiscard]] std::size_t
  LiveEntryCount(std::uint64_t Generation) const noexcept;
  [[nodiscard]] std::size_t EntryCountOf(const void *Owner) const noexcept;

  [[nodiscard]] std::uint64_t GenerationOf(const void *Owner) const noexcept;

  [[nodiscard]] LazyCacheCounters Counters() const noexcept { return Counted; }
  void ResetCounters() noexcept { Counted = LazyCacheCounters(); }

private:
  [[nodiscard]] LazyCacheNode *Find(const NativeIdentity &Owner) noexcept;
  [[nodiscard]] const LazyCacheNode *
  Find(const NativeIdentity &Owner) const noexcept;
  [[nodiscard]] const LazyCacheNode *
  FindAddress(const void *Owner) const noexcept;

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
