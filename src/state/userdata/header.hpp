#pragma once

// The private, versioned layout of one typed userdata value.
//
// This is the only thing Luna trusts inside a Luau userdata block: a magic
// value and a layout version, the logical State the value came from, the
// dynamic and declared-view canonical types, the class symbol, the metatable
// identity, the ownership model, the lifetime and const state, Luna's own
// native identity, the ownership payload, the lifetime handle, the allocator
// record, the dispatch generation the value was published under, and the lazy
// property cache slot.
//
// Nothing here is reachable from a consumer and nothing here is an address a
// consumer could observe. Metatable equality alone is never treated as proof of
// type or origin: access validates the layout, the origin State, the metatable
// identity, the lifetime state, the dynamic type or a registered cast path, and
// finally const permission before any native pointer is handed out. This header
// defines the layout and the two cheapest of those checks; the complete
// deterministic access order, the identity cache, and the ownership transitions
// belong to the userdata access and release paths.
//
// The layout is deliberately trivially copyable and free of owning members: it
// lives in memory the virtual machine allocated, so it can never depend on a
// destructor running inside that block to stay coherent.

// clang-format off
#include <luna/reflection/ids.hpp>

#include "state/transaction/lifecycle.hpp"
#include "state/userdata/identity.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>
// clang-format on

namespace Luna::Detail {

// Luna's own userdata marker, spelled 'LUNA' as one big-endian byte sequence.
// A block that does not start with it is never treated as a Luna userdata.
inline constexpr std::uint32_t UserdataMagic = 0x4C554E41U;

// The current layout version. Every field addition or reorder bumps it, so a
// value written by another layout is rejected instead of misread.
inline constexpr std::uint16_t UserdataLayoutVersion = 2;

// Where the ownership payload of one value lives. `Storage` is the native
// object Luna handed out, and `SharedOwnership` is the erased holder of exactly
// one shared ownership reference when the value is shared.
struct OwnershipPayload final {
  void *Storage = nullptr;
  void *SharedOwnership = nullptr;

  [[nodiscard]] constexpr bool HasStorage() const noexcept {
    return Storage != nullptr;
  }
};

// The explicit lifetime handle a borrowed value requires. The record is
// Luna-owned; the generation is what makes invalidation atomic, because an
// invalidated handle advances its generation and every later access compares
// unequal.
struct LifetimeHandleReference final {
  const void *Record = nullptr;
  std::uint64_t Generation = 0;

  [[nodiscard]] constexpr bool IsDeclared() const noexcept {
    return Record != nullptr;
  }
};

// The immutable allocator record that owns allocation, construction,
// destruction, and deallocation of this value's storage. Luna retains it until
// the last dependent userdata completes cleanup.
struct AllocatorRecordReference final {
  const void *Record = nullptr;

  [[nodiscard]] constexpr bool IsDeclared() const noexcept {
    return Record != nullptr;
  }
};

// The lazy property cache of one value. Entries are Luna-owned and keyed by the
// dispatch generation they were produced under, so a generation change
// invalidates them by mismatch instead of by traversal.
struct LazyPropertyCacheSlot final {
  // Luna-issued state-local node identity. Zero means no cache node. The VM
  // block never retains a pointer into the cache's replaceable storage.
  std::uint64_t Node = 0;
  std::uint64_t Generation = 0;

  [[nodiscard]] constexpr bool IsPopulated() const noexcept {
    return Node != 0;
  }
};

struct UserdataHeader final {
  std::uint32_t Magic = UserdataMagic;
  std::uint16_t LayoutVersion = UserdataLayoutVersion;

  OwnershipModel Ownership = OwnershipModel::Borrowed;
  LifetimeState Lifetime = LifetimeState::Allocated;
  ConstAccess Access = ConstAccess::Mutable;

  // The logical State the value was exposed by. It lives in `State::Impl`, so a
  // State move transfers it unchanged and a value keeps its origin identity
  // across every move of its State.
  StateIdentity Origin;

  // The canonical type of the stored object, and the canonical type of the view
  // it was exposed as. They differ when a base view of a derived object is
  // exposed through one registered cast path.
  TypeId DynamicType;
  TypeId DeclaredViewType;

  SymbolId ClassSymbol;
  MetatableId Metatable;

  NativeIdentity Identity;
  OwnershipPayload Payload;
  LifetimeHandleReference Handle;
  AllocatorRecordReference Allocator;

  // The dispatch generation this value was published under. Cached member
  // results and resolved targets are only valid while it still matches.
  std::uint64_t DispatchGeneration = 0;

  LazyPropertyCacheSlot LazyCache;

  // The block starts with Luna's marker and was written by exactly this layout.
  // This is the first question every access asks, before any other field is
  // read.
  [[nodiscard]] constexpr bool HasCanonicalLayout() const noexcept {
    return Magic == UserdataMagic && LayoutVersion == UserdataLayoutVersion;
  }

  // The value was exposed by exactly this logical State. A wrong-origin value
  // never reaches native code, whichever metatable it happens to carry.
  [[nodiscard]] constexpr bool
  BelongsTo(const StateIdentity &Requested) const noexcept {
    return Origin.IsValid() && Origin == Requested;
  }

  // The value names one complete registered class identity: a dynamic type, a
  // declared view type, a class symbol, and a metatable identity.
  [[nodiscard]] constexpr bool IdentifiesClass() const noexcept {
    return DynamicType.IsValid() && DeclaredViewType.IsValid() &&
           ClassSymbol.IsValid() && Metatable.IsValid();
  }

  // The value carries exactly the metatable identity of the requested class.
  [[nodiscard]] constexpr bool
  CarriesMetatable(const MetatableId &Requested) const noexcept {
    return Metatable.IsValid() && Metatable == Requested;
  }

  // The lifetime state still permits native access. Every other state - not yet
  // published, invalidated, expired, destroyed, released - fails before a
  // native pointer exists.
  [[nodiscard]] constexpr bool HasLiveLifetime() const noexcept {
    return Lifetime == LifetimeState::Published;
  }

  [[nodiscard]] constexpr bool PermitsMutation() const noexcept {
    return Access == ConstAccess::Mutable;
  }

  // A borrowed value requires an explicit lifetime handle; the other two
  // ownership models never need one.
  [[nodiscard]] constexpr bool HasRequiredLifetimeHandle() const noexcept {
    return Ownership != OwnershipModel::Borrowed || Handle.IsDeclared();
  }
};

// One header describing a value of `Class` that this State is about to expose.
// It is created in the `Allocated` state and carries no payload yet: ownership
// establishment, publication, and the identity cache fill the rest in, so a
// half-built value can never look published.
struct UserdataHeaderRequest final {
  StateIdentity Origin;
  TypeId DynamicType;
  TypeId DeclaredViewType;
  SymbolId ClassSymbol;
  MetatableId Metatable;
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  ConstAccess Access = ConstAccess::Mutable;
  std::uint64_t DispatchGeneration = 0;
};

[[nodiscard]] UserdataHeader
MakeUserdataHeader(const UserdataHeaderRequest &Request) noexcept;

// The header of one candidate block, or null when the block is too small or
// does not carry Luna's marker and layout version. Nothing else reads a
// candidate block directly.
[[nodiscard]] const UserdataHeader *
InspectUserdataHeader(const void *Block, std::size_t ByteCount) noexcept;

static_assert(std::is_trivially_copyable_v<UserdataHeader>,
              "The userdata header lives in virtual-machine owned memory and "
              "must stay trivially copyable.");

} // namespace Luna::Detail
