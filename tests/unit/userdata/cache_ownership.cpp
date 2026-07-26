// clang-format off
#include <luna/binding/value.hpp>
#include <luna/reflection/ids.hpp>

#include "state/transaction/lifecycle.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/identity_cache.hpp"
#include "state/userdata/lazy_cache.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>
// clang-format on

namespace {

using Luna::Detail::ConstAccess;
using Luna::Detail::LazyPropertyCache;
using Luna::Detail::MetatableId;
using Luna::Detail::NativeIdentity;
using Luna::Detail::OwnershipModel;
using Luna::Detail::StateIdentity;
using Luna::Detail::UserdataCacheDecision;
using Luna::Detail::UserdataCacheEntry;
using Luna::Detail::UserdataCacheRequest;
using Luna::Detail::UserdataHeader;
using Luna::Detail::UserdataIdentityCache;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "cache ownership check failed: " << Description << '\n';
}

template <class Identity>
[[nodiscard]] Identity MakeIdentity(std::uint8_t Seed) {
  typename Identity::Storage Bytes{};
  Bytes[0] = Seed;
  Bytes[Identity::ByteCount - 1] = static_cast<std::uint8_t>(Seed ^ 0x5aU);
  return Identity::FromBytes(Bytes);
}
[[nodiscard]] UserdataCacheRequest
MakeRequest(StateIdentity Origin, const void *Address,
            std::uint64_t DispatchGeneration) {
  UserdataCacheRequest Request;
  Request.Origin = Origin;
  Request.Address = Address;
  Request.DynamicType = MakeIdentity<Luna::TypeId>(1);
  Request.DeclaredViewType = Request.DynamicType;
  Request.ClassSymbol = MakeIdentity<Luna::SymbolId>(2);
  Request.Metatable = MetatableId::FromValue(3);
  Request.Ownership = OwnershipModel::Borrowed;
  Request.Access = ConstAccess::Mutable;
  Request.DispatchGeneration = DispatchGeneration;
  return Request;
}

[[nodiscard]] UserdataCacheEntry MakeEntry(const UserdataCacheRequest &Request,
                                           std::uint64_t Nonce) {
  UserdataCacheEntry Entry;
  Entry.Origin = Request.Origin;
  Entry.Identity = NativeIdentity{Request.Address, Nonce};
  Entry.DynamicType = Request.DynamicType;
  Entry.DeclaredViewType = Request.DeclaredViewType;
  Entry.ClassSymbol = Request.ClassSymbol;
  Entry.Metatable = Request.Metatable;
  Entry.Ownership = Request.Ownership;
  Entry.Access = Request.Access;
  Entry.DispatchGeneration = Request.DispatchGeneration;
  Entry.IsActive = true;
  return Entry;
}

[[nodiscard]] UserdataHeader MakeHeader(StateIdentity Origin, void *Storage,
                                        std::uint64_t Nonce) {
  UserdataHeader Header;
  Header.Origin = Origin;
  Header.Identity = NativeIdentity{Storage, Nonce};
  Header.Payload.Storage = Storage;
  Header.DynamicType = MakeIdentity<Luna::TypeId>(1);
  Header.DeclaredViewType = Header.DynamicType;
  Header.ClassSymbol = MakeIdentity<Luna::SymbolId>(2);
  Header.Metatable = MetatableId::FromValue(3);
  return Header;
}

void CheckIdentityCacheUsesOwnedStableKeys() {
  const StateIdentity Owner = StateIdentity::Next();
  const StateIdentity Foreign = StateIdentity::Next();
  UserdataIdentityCache Cache(Owner);
  int Object = 0;

  const UserdataCacheRequest Request = MakeRequest(Owner, &Object, 7);
  Check(Cache.Evaluate(Request).Decision == UserdataCacheDecision::Create,
        "an empty owner cache chooses creation");

  const UserdataCacheEntry First = MakeEntry(Request, 11);
  Check(Cache.Record(First), "one complete owner entry is recorded");
  const auto Reused = Cache.Evaluate(Request);
  Check(Reused.PermitsReuse() && Reused.Entry.has_value() &&
            Reused.Entry->Identity == First.Identity,
        "reuse returns copied immutable decision material");

  UserdataCacheRequest ForeignRequest = Request;
  ForeignRequest.Origin = Foreign;
  Check(Cache.Evaluate(ForeignRequest).Decision ==
            UserdataCacheDecision::UnavailableRequest,
        "another logical State cannot query this cache");

  UserdataCacheRequest LaterGeneration = Request;
  LaterGeneration.DispatchGeneration = 8;
  Check(Cache.Evaluate(LaterGeneration).Decision ==
            UserdataCacheDecision::IncompatibleType,
        "a dispatch generation mismatch never reuses stale userdata");

  Check(Cache.Inactivate(First.Identity),
        "the exact old identity is invalidated before replacement");
  const UserdataCacheEntry Replacement = MakeEntry(Request, 12);
  Check(Cache.Record(Replacement),
        "recycled storage records a new Luna identity");
  Check(Reused.Entry.has_value() && Reused.Entry->Identity == First.Identity,
        "a retained lookup owns its decision after cache replacement");
  Check(!Cache.Forget(First.Identity) && Cache.ActiveCount() == 1,
        "a delayed old release cannot erase the replacement");
  Check(Cache.Forget(Replacement.Identity) && Cache.IsEmpty(),
        "the exact replacement identity is removed");
}
void CheckLazyCacheUsesStableNodeIdentity() {
  const StateIdentity Owner = StateIdentity::Next();
  const StateIdentity Foreign = StateIdentity::Next();
  LazyPropertyCache Cache(Owner);
  int Object = 0;
  const Luna::SymbolId Member = MakeIdentity<Luna::SymbolId>(9);

  UserdataHeader First = MakeHeader(Owner, &Object, 21);
  Check(Cache.Store(First, Member, Luna::Value(42), 3),
        "a successful getter value is stored");
  const std::uint64_t Node = First.LazyCache.Node;
  Check(Node != 0 && First.LazyCache.Generation == 3,
        "the header stores a stable node ID and dispatch generation");
  const Luna::Value *Observed = Cache.Observe(First, Member, 3);
  Check(Observed != nullptr && std::get<int>(*Observed) == 42,
        "the exact owner and generation observe the cached value");

  UserdataHeader Recycled = MakeHeader(Owner, &Object, 22);
  Recycled.LazyCache = First.LazyCache;
  Check(Cache.Observe(Recycled, Member, 3) == nullptr,
        "recycled storage cannot impersonate the old userdata identity");

  UserdataHeader ForeignHeader = First;
  ForeignHeader.Origin = Foreign;
  Check(Cache.Observe(ForeignHeader, Member, 3) == nullptr,
        "a copied slot from another State cannot resolve this cache");
  Check(Cache.Observe(First, Member, 4) == nullptr,
        "a later dispatch generation cannot observe an earlier value");

  Check(Cache.Store(First, Member, Luna::Value(84), 4),
        "the later generation stores its own value");
  Check(First.LazyCache.Node == Node && Cache.LiveEntryCount(4) == 1 &&
            Cache.LiveEntryCount(3) == 0,
        "generation replacement reuses only the stable owner node");
  Check(Cache.InvalidateOwner(First) == 1 && !First.LazyCache.IsPopulated(),
        "explicit invalidation withdraws the stable slot");

  Check(Cache.Store(First, Member, Luna::Value(7), 4),
        "the invalidated owner can record a later successful result");
  Check(Cache.Drop(First.Identity, &First) && Cache.NodeCount() == 0,
        "retirement drops the exact node before payload release");

  Check(Cache.Store(Recycled, Member, Luna::Value(9), 4),
        "recycled storage owns a distinct cache node");
  Check(!Cache.Drop(NativeIdentity{&Object, 21}, nullptr) &&
            Cache.EntryCountOf(&Object) == 1,
        "a delayed old drop cannot erase the recycled identity");
  Check(Cache.Drop(Recycled.Identity, &Recycled) && Cache.NodeCount() == 0,
        "the recycled identity drops only its own node");
}

} // namespace

int RunCacheOwnershipAndInvalidationTests() {
  FailureCount = 0;
  CheckIdentityCacheUsesOwnedStableKeys();
  CheckLazyCacheUsesStableNodeIdentity();
  return FailureCount == 0 ? 0 : 1;
}
