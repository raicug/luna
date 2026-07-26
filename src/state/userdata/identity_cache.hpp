#pragma once

// The per-State native identity cache.
//
// One native object exposed twice in one State must be one Luau value, not two:
// two values would mean two headers, two lifetime records, and - for an owning
// model - two owners of one object. So every exposure first asks this cache
// what to do with the identity it is about to expose:
//
//   - nothing live is recorded: create one value and record it;
//   - a live entry describes exactly this identity, view, and ownership: hand
//     the existing value back;
//   - a live entry describes the same object owned differently, or typed
//     differently, or viewed differently: refuse, and create no second owner.
//
// Entries are weak with respect to virtual-machine reachability. The cache
// itself keeps no reference that would keep a value alive: it records what the
// compatibility decision needs and nothing else, and the virtual-machine half
// of the cache is a weak-valued table, so collecting a value drops its slot.
// Before any payload release the entry is inactivated and removed, so a
// released object can never be handed back by the cache.
//
// The address inside a recorded identity is Luna's own cache key. It is paired
// with the state-local nonce of the exposure that produced it, so recycled
// storage never impersonates a released object, and it never contributes to a
// `TypeId`, a `SymbolId`, reflection, a persisted diagnostic, or generated
// output.

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

// One recorded exposure of one native object.
struct UserdataCacheEntry final {
  // Every state-local entry names its logical owner explicitly. The cache also
  // verifies this against its own owner, so an entry can never be transplanted
  // between States even when all other IDs happen to compare equal.
  StateIdentity Origin;
  NativeIdentity Identity;

  TypeId DynamicType;
  TypeId DeclaredViewType;
  SymbolId ClassSymbol;
  MetatableId Metatable;
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  ConstAccess Access = ConstAccess::Mutable;
  std::uint64_t DispatchGeneration = 0;

  // False once the entry was inactivated ahead of a payload release. An
  // inactive entry never satisfies a lookup and is never reused.
  bool IsActive = false;

  [[nodiscard]] bool IsComplete() const noexcept;
};

// What one exposure wants to record.
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

// What the cache policy decided about one exposure request.
enum class UserdataCacheDecision : std::uint8_t {
  // No live entry describes this address, so one value is created and recorded.
  Create,

  // A live entry describes exactly this identity, view, and ownership.
  Reuse,

  // The request is not one complete identity.
  UnavailableRequest,

  // The object is already exposed under a different ownership model.
  ConflictingOwnership,

  // The object is already exposed as a different canonical type, class, or
  // declared view.
  IncompatibleType,

  // The object is already exposed through a view with different mutation
  // permission. One native identity has exactly one live view per State, so the
  // request is refused rather than granted a second, differently permissioned
  // header for the same object.
  IncompatibleAccess
};

[[nodiscard]] std::string_view
UserdataCacheDecisionText(UserdataCacheDecision Decision) noexcept;

// The decision plus the live entry it was taken against, when there is one.
struct UserdataCacheLookup final {
  UserdataCacheDecision Decision = UserdataCacheDecision::Create;

  // A copy of the immutable decision material, never a pointer into the
  // cache's replaceable vector storage.
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

  // What the documented policy says about one exposure request. Nothing is
  // recorded and nothing is mutated.
  [[nodiscard]] UserdataCacheLookup
  Evaluate(const UserdataCacheRequest &Request) const noexcept;

  // The live entry recorded for one address, or null when the address has no
  // active entry.
  [[nodiscard]] const UserdataCacheEntry *
  FindActive(const void *Address) const noexcept;

  // The entry recorded for one address whether it is active or not.
  [[nodiscard]] const UserdataCacheEntry *
  Find(const void *Address) const noexcept;

  // Records one created value. An inactive entry for the same address is
  // replaced, which is how storage that was released and allocated again is
  // recorded under its new nonce. Recording over a live entry is refused, so a
  // second owner can never be recorded by mistake.
  [[nodiscard]] bool Record(const UserdataCacheEntry &Entry);

  // Inactivates one exact userdata identity ahead of its payload release. An
  // older release can therefore never inactivate a newer exposure that reused
  // the same storage address.
  [[nodiscard]] bool Inactivate(const NativeIdentity &Identity) noexcept;

  // Drops one exact userdata identity entirely. Address-only removal is kept
  // private to stale weak-slot recovery, where the cache has already selected
  // the current live entry.
  [[nodiscard]] bool Forget(const NativeIdentity &Identity) noexcept;
  [[nodiscard]] bool ForgetAddress(const void *Address) noexcept;

  [[nodiscard]] std::span<const UserdataCacheEntry> Entries() const noexcept {
    return Records;
  }

  [[nodiscard]] std::size_t Size() const noexcept { return Records.size(); }
  [[nodiscard]] std::size_t ActiveCount() const noexcept;
  [[nodiscard]] bool IsEmpty() const noexcept { return Records.empty(); }
  void Clear() noexcept { Records.clear(); }

  // True when the entry describes exactly the requested identity, class, view,
  // and ownership, so the recorded value may be handed back.
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
