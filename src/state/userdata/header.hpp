#pragma once

// clang-format off
#include <luna/reflection/ids.hpp>

#include "state/transaction/lifecycle.hpp"
#include "state/userdata/identity.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>
// clang-format on

namespace Luna::Detail {

inline constexpr std::uint32_t UserdataMagic = 0x4C554E41U;

inline constexpr std::uint16_t UserdataLayoutVersion = 2;

struct OwnershipPayload final {
  void *Storage = nullptr;
  void *SharedOwnership = nullptr;

  [[nodiscard]] constexpr bool HasStorage() const noexcept {
    return Storage != nullptr;
  }
};

struct LifetimeHandleReference final {
  const void *Record = nullptr;
  std::uint64_t Generation = 0;

  [[nodiscard]] constexpr bool IsDeclared() const noexcept {
    return Record != nullptr;
  }
};

struct AllocatorRecordReference final {
  const void *Record = nullptr;

  [[nodiscard]] constexpr bool IsDeclared() const noexcept {
    return Record != nullptr;
  }
};

struct LazyPropertyCacheSlot final {
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

  StateIdentity Origin;

  TypeId DynamicType;
  TypeId DeclaredViewType;

  SymbolId ClassSymbol;
  MetatableId Metatable;

  NativeIdentity Identity;
  OwnershipPayload Payload;
  LifetimeHandleReference Handle;
  AllocatorRecordReference Allocator;

  std::uint64_t DispatchGeneration = 0;

  LazyPropertyCacheSlot LazyCache;

  [[nodiscard]] constexpr bool HasCanonicalLayout() const noexcept {
    return Magic == UserdataMagic && LayoutVersion == UserdataLayoutVersion;
  }

  [[nodiscard]] constexpr bool
  BelongsTo(const StateIdentity &Requested) const noexcept {
    return Origin.IsValid() && Origin == Requested;
  }

  [[nodiscard]] constexpr bool IdentifiesClass() const noexcept {
    return DynamicType.IsValid() && DeclaredViewType.IsValid() &&
           ClassSymbol.IsValid() && Metatable.IsValid();
  }

  [[nodiscard]] constexpr bool
  CarriesMetatable(const MetatableId &Requested) const noexcept {
    return Metatable.IsValid() && Metatable == Requested;
  }

  [[nodiscard]] constexpr bool HasLiveLifetime() const noexcept {
    return Lifetime == LifetimeState::Published;
  }

  [[nodiscard]] constexpr bool PermitsMutation() const noexcept {
    return Access == ConstAccess::Mutable;
  }

  [[nodiscard]] constexpr bool HasRequiredLifetimeHandle() const noexcept {
    return Ownership != OwnershipModel::Borrowed || Handle.IsDeclared();
  }
};

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

[[nodiscard]] const UserdataHeader *
InspectUserdataHeader(const void *Block, std::size_t ByteCount) noexcept;

static_assert(std::is_trivially_copyable_v<UserdataHeader>,
              "The userdata header lives in virtual-machine owned memory and "
              "must stay trivially copyable.");

} // namespace Luna::Detail
