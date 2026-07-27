// clang-format off
#include "state/userdata/lazy_cache.hpp"

#include <luna/binding/value.hpp>
#include <luna/reflection/ids.hpp>

#include "state/userdata/header.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {

const Value *LazyPropertyCache::Observe(const UserdataHeader &Header,
                                        const SymbolId &Member,
                                        std::uint64_t Generation) noexcept {
  if (!Member.IsValid() || !Header.Payload.HasStorage() ||
      Header.Origin != OwnerIdentity || !Header.Identity.IsValid()) {
    ++Counted.Miss;
    return nullptr;
  }

  const LazyCacheNode *Node =
      Header.LazyCache.IsPopulated() ? Resolve(Header) : nullptr;
  if (Node == nullptr) {
    ++Counted.Miss;
    return nullptr;
  }

  if (Node->Generation != Generation ||
      Header.LazyCache.Generation != Generation) {
    ++Counted.GenerationMismatch;
    return nullptr;
  }

  for (const LazyCacheEntry &Entry : Node->Entries) {
    if (Entry.Member == Member) {
      ++Counted.Hit;
      return &Entry.Cached;
    }
  }
  ++Counted.Miss;
  return nullptr;
}

bool LazyPropertyCache::Store(UserdataHeader &Header, const SymbolId &Member,
                              const Value &Produced, std::uint64_t Generation) {
  if (!Member.IsValid() || !Header.Payload.HasStorage() ||
      Header.Origin != OwnerIdentity || !Header.Identity.IsValid())
    return false;

  LazyCacheNode *Node = Find(Header.Identity);
  if (Node == nullptr) {
    auto Created = std::make_unique<LazyCacheNode>();
    Created->Origin = OwnerIdentity;
    Created->Owner = Header.Identity;
    Created->Identity = ++NextNodeIdentity;
    Created->Generation = Generation;
    Node = Created.get();
    Nodes.push_back(std::move(Created));
  } else if (Node->Generation != Generation) {
    Node->Entries.clear();
    Node->Generation = Generation;
  }

  for (LazyCacheEntry &Entry : Node->Entries) {
    if (Entry.Member == Member) {
      Entry.Cached = Produced;
      ++Counted.Replace;
      Publish(&Header, *Node, Generation);
      return true;
    }
  }

  LazyCacheEntry Entry;
  Entry.Member = Member;
  Entry.Cached = Produced;
  Node->Entries.push_back(std::move(Entry));
  ++Counted.Store;
  Publish(&Header, *Node, Generation);
  return true;
}

std::size_t
LazyPropertyCache::InvalidateOwner(UserdataHeader &Header) noexcept {
  if (Header.Origin != OwnerIdentity || !Header.Identity.IsValid())
    return 0;
  LazyCacheNode *Node = Find(Header.Identity);
  if (Node == nullptr)
    return 0;

  const std::size_t Removed = Node->Entries.size();
  Node->Entries.clear();
  if (Removed != 0)
    ++Counted.Invalidate;

  Withdraw(&Header);
  return Removed;
}

std::size_t
LazyPropertyCache::InvalidateMember(UserdataHeader &Header,
                                    const SymbolId &Member) noexcept {
  if (Header.Origin != OwnerIdentity || !Header.Identity.IsValid())
    return 0;
  LazyCacheNode *Node = Find(Header.Identity);
  if (Node == nullptr || !Member.IsValid())
    return 0;

  std::size_t Removed = 0;
  for (std::size_t Index = Node->Entries.size(); Index > 0; --Index) {
    if (Node->Entries[Index - 1].Member != Member)
      continue;
    Node->Entries.erase(Node->Entries.begin() +
                        static_cast<std::ptrdiff_t>(Index - 1));
    ++Removed;
  }
  if (Removed != 0)
    ++Counted.Invalidate;
  if (Node->Entries.empty())
    Withdraw(&Header);
  return Removed;
}

bool LazyPropertyCache::Drop(const NativeIdentity &Owner,
                             UserdataHeader *Header) noexcept {
  if (Header != nullptr && Header->Origin == OwnerIdentity &&
      Header->Identity == Owner)
    Withdraw(Header);
  for (std::size_t Index = 0; Index < Nodes.size(); ++Index) {
    if (Nodes[Index] == nullptr || Nodes[Index]->Origin != OwnerIdentity ||
        !(Nodes[Index]->Owner == Owner))
      continue;
    Nodes.erase(Nodes.begin() + static_cast<std::ptrdiff_t>(Index));
    ++Counted.Drop;
    return true;
  }
  return false;
}

std::size_t LazyPropertyCache::Clear() noexcept {
  const std::size_t Removed = EntryCount();
  if (!Nodes.empty())
    ++Counted.Drop;
  Nodes.clear();
  return Removed;
}

std::size_t LazyPropertyCache::EntryCount() const noexcept {
  std::size_t Total = 0;
  for (const auto &Node : Nodes) {
    if (Node)
      Total += Node->Entries.size();
  }
  return Total;
}

std::size_t
LazyPropertyCache::LiveEntryCount(std::uint64_t Generation) const noexcept {
  std::size_t Total = 0;
  for (const auto &Node : Nodes) {
    if (Node && Node->Generation == Generation)
      Total += Node->Entries.size();
  }
  return Total;
}

std::size_t LazyPropertyCache::EntryCountOf(const void *Owner) const noexcept {
  const LazyCacheNode *Node = FindAddress(Owner);
  return Node ? Node->Entries.size() : 0;
}

std::uint64_t
LazyPropertyCache::GenerationOf(const void *Owner) const noexcept {
  const LazyCacheNode *Node = FindAddress(Owner);
  return Node ? Node->Generation : 0;
}

LazyCacheNode *LazyPropertyCache::Find(const NativeIdentity &Owner) noexcept {
  if (!Owner.IsValid())
    return nullptr;
  for (auto &Node : Nodes) {
    if (Node && Node->Origin == OwnerIdentity && Node->Owner == Owner)
      return Node.get();
  }
  return nullptr;
}

const LazyCacheNode *
LazyPropertyCache::Find(const NativeIdentity &Owner) const noexcept {
  if (!Owner.IsValid())
    return nullptr;
  for (const auto &Node : Nodes) {
    if (Node && Node->Origin == OwnerIdentity && Node->Owner == Owner)
      return Node.get();
  }
  return nullptr;
}

const LazyCacheNode *
LazyPropertyCache::FindAddress(const void *Owner) const noexcept {
  if (Owner == nullptr)
    return nullptr;
  for (const auto &Node : Nodes) {
    if (Node && Node->Origin == OwnerIdentity && Node->Owner.Address == Owner)
      return Node.get();
  }
  return nullptr;
}

LazyCacheNode *
LazyPropertyCache::Resolve(const UserdataHeader &Header) noexcept {
  if (Header.Origin != OwnerIdentity || !Header.Identity.IsValid() ||
      Header.LazyCache.Node == 0)
    return nullptr;
  LazyCacheNode *Node = Find(Header.Identity);
  if (Node == nullptr || Node->Identity != Header.LazyCache.Node)
    return nullptr;
  return Node;
}

void LazyPropertyCache::Publish(UserdataHeader *Header,
                                const LazyCacheNode &Node,
                                std::uint64_t Generation) noexcept {
  if (Header == nullptr)
    return;

  Header->LazyCache.Node = Node.Identity;
  Header->LazyCache.Generation = Generation;
}

void LazyPropertyCache::Withdraw(UserdataHeader *Header) noexcept {
  if (Header == nullptr)
    return;
  Header->LazyCache.Node = 0;
  Header->LazyCache.Generation = 0;
}

} // namespace Luna::Detail
