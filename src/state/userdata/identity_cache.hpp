#pragma once

// clang-format off
#include <luna/reflection/ids.hpp>

#include "state/transaction/lifecycle.hpp"
#include "state/userdata/identity.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

struct UserdataCacheEntry final {
  StateIdentity Origin;
  NativeIdentity Identity;

  TypeId DynamicType;
  TypeId DeclaredViewType;
  SymbolId ClassSymbol;
  MetatableId Metatable;
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  ConstAccess Access = ConstAccess::Mutable;
  std::uint64_t DispatchGeneration = 0;

  bool IsActive = false;

  [[nodiscard]] bool IsComplete() const noexcept;
};

struct UserdataCacheRequest final {
  StateIdentity Origin;
  const void *Address = nullptr;
  TypeId DynamicType;
  TypeId DeclaredViewType;
  SymbolId ClassSymbol;
  MetatableId Metatable;
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  ConstAccess Access = ConstAccess::Mutable;
  std::uint64_t DispatchGeneration = 0;

  [[nodiscard]] bool IsComplete() const noexcept;
};

enum class UserdataCacheDecision : std::uint8_t {
  Create,

  Reuse,

  UnavailableRequest,

  ConflictingOwnership,

  IncompatibleType,

  IncompatibleAccess
};

[[nodiscard]] std::string_view
UserdataCacheDecisionText(UserdataCacheDecision Decision) noexcept;

struct UserdataCacheLookup final {
  UserdataCacheDecision Decision = UserdataCacheDecision::Create;

  std::optional<UserdataCacheEntry> Entry;

  [[nodiscard]] bool PermitsCreation() const noexcept {
    return Decision == UserdataCacheDecision::Create;
  }

  [[nodiscard]] bool PermitsReuse() const noexcept {
    return Decision == UserdataCacheDecision::Reuse && Entry.has_value();
  }
};

class UserdataIdentityCache final {
public:
  explicit UserdataIdentityCache(StateIdentity Owner) noexcept
      : OwnerIdentity(Owner) {}

  [[nodiscard]] UserdataCacheLookup
  Evaluate(const UserdataCacheRequest &Request) const noexcept;

  [[nodiscard]] const UserdataCacheEntry *
  FindActive(const void *Address) const noexcept;

  [[nodiscard]] const UserdataCacheEntry *
  Find(const void *Address) const noexcept;

  [[nodiscard]] bool Record(const UserdataCacheEntry &Entry);

  [[nodiscard]] bool Inactivate(const NativeIdentity &Identity) noexcept;

  [[nodiscard]] bool Forget(const NativeIdentity &Identity) noexcept;
  [[nodiscard]] bool ForgetAddress(const void *Address) noexcept;

  [[nodiscard]] std::span<const UserdataCacheEntry> Entries() const noexcept {
    return Records;
  }

  [[nodiscard]] std::size_t Size() const noexcept { return Records.size(); }
  [[nodiscard]] std::size_t ActiveCount() const noexcept;
  [[nodiscard]] bool IsEmpty() const noexcept { return Records.empty(); }
  void Clear() noexcept { Records.clear(); }

  [[nodiscard]] static bool Matches(const UserdataCacheEntry &Entry,
                                    const UserdataCacheRequest &Request);

private:
  [[nodiscard]] UserdataCacheEntry *
  FindForUpdate(const NativeIdentity &Identity) noexcept;
  [[nodiscard]] UserdataCacheEntry *
  FindAddressForUpdate(const void *Address) noexcept;

  StateIdentity OwnerIdentity;
  std::vector<UserdataCacheEntry> Records;
};

} // namespace Luna::Detail
