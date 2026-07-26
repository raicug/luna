// clang-format off
#include "state/userdata/identity_cache.hpp"

#include "state/userdata/identity.hpp"

#include <cstddef>
#include <string_view>
// clang-format on

namespace Luna::Detail {

bool UserdataCacheEntry::IsComplete() const noexcept {
  return Origin.IsValid() && Identity.IsValid() && DynamicType.IsValid() &&
         DeclaredViewType.IsValid() && ClassSymbol.IsValid() &&
         Metatable.IsValid();
}

bool UserdataCacheRequest::IsComplete() const noexcept {
  return Origin.IsValid() && Address != nullptr && DynamicType.IsValid() &&
         DeclaredViewType.IsValid() && ClassSymbol.IsValid() &&
         Metatable.IsValid();
}

std::string_view
UserdataCacheDecisionText(UserdataCacheDecision Decision) noexcept {
  switch (Decision) {
  case UserdataCacheDecision::Create:
    return "create";
  case UserdataCacheDecision::Reuse:
    return "reuse";
  case UserdataCacheDecision::UnavailableRequest:
    return "unavailable_request";
  case UserdataCacheDecision::ConflictingOwnership:
    return "conflicting_ownership";
  case UserdataCacheDecision::IncompatibleType:
    return "incompatible_type";
  case UserdataCacheDecision::IncompatibleAccess:
    return "incompatible_access";
  }
  return "unavailable_request";
}

bool UserdataIdentityCache::Matches(const UserdataCacheEntry &Entry,
                                    const UserdataCacheRequest &Request) {
  return Entry.Origin == Request.Origin &&
         Entry.Identity.Address == Request.Address &&
         Entry.DynamicType == Request.DynamicType &&
         Entry.DeclaredViewType == Request.DeclaredViewType &&
         Entry.ClassSymbol == Request.ClassSymbol &&
         Entry.Metatable == Request.Metatable &&
         Entry.Ownership == Request.Ownership &&
         Entry.Access == Request.Access &&
         Entry.DispatchGeneration == Request.DispatchGeneration;
}

UserdataCacheLookup UserdataIdentityCache::Evaluate(
    const UserdataCacheRequest &Request) const noexcept {
  UserdataCacheLookup Lookup;
  if (!Request.IsComplete() || Request.Origin != OwnerIdentity) {
    Lookup.Decision = UserdataCacheDecision::UnavailableRequest;
    return Lookup;
  }

  const UserdataCacheEntry *Live = FindActive(Request.Address);
  if (Live == nullptr)
    return Lookup;

  // Return immutable decision material by value. No caller retains a pointer
  // into the vector that a later record or invalidation can replace.
  Lookup.Entry = *Live;

  // Ownership is the first incompatibility, because handing back a value owned
  // differently, or creating a second one, are both ways of ending up with two
  // owners of one object.
  if (Live->Ownership != Request.Ownership) {
    Lookup.Decision = UserdataCacheDecision::ConflictingOwnership;
    return Lookup;
  }
  if (Live->DynamicType != Request.DynamicType ||
      Live->DeclaredViewType != Request.DeclaredViewType ||
      Live->ClassSymbol != Request.ClassSymbol ||
      Live->Metatable != Request.Metatable || Live->Origin != Request.Origin ||
      Live->DispatchGeneration != Request.DispatchGeneration) {
    Lookup.Decision = UserdataCacheDecision::IncompatibleType;
    return Lookup;
  }
  if (Live->Access != Request.Access) {
    Lookup.Decision = UserdataCacheDecision::IncompatibleAccess;
    return Lookup;
  }

  Lookup.Decision = UserdataCacheDecision::Reuse;
  return Lookup;
}

const UserdataCacheEntry *
UserdataIdentityCache::Find(const void *Address) const noexcept {
  if (Address == nullptr)
    return nullptr;
  for (const UserdataCacheEntry &Entry : Records) {
    if (Entry.Origin == OwnerIdentity && Entry.Identity.Address == Address)
      return &Entry;
  }
  return nullptr;
}

const UserdataCacheEntry *
UserdataIdentityCache::FindActive(const void *Address) const noexcept {
  const UserdataCacheEntry *Entry = Find(Address);
  return Entry != nullptr && Entry->IsActive ? Entry : nullptr;
}

UserdataCacheEntry *
UserdataIdentityCache::FindAddressForUpdate(const void *Address) noexcept {
  if (Address == nullptr)
    return nullptr;
  for (UserdataCacheEntry &Entry : Records) {
    if (Entry.Origin == OwnerIdentity && Entry.Identity.Address == Address)
      return &Entry;
  }
  return nullptr;
}

UserdataCacheEntry *
UserdataIdentityCache::FindForUpdate(const NativeIdentity &Identity) noexcept {
  if (!Identity.IsValid())
    return nullptr;
  for (UserdataCacheEntry &Entry : Records) {
    if (Entry.Origin == OwnerIdentity && Entry.Identity == Identity)
      return &Entry;
  }
  return nullptr;
}

bool UserdataIdentityCache::Record(const UserdataCacheEntry &Entry) {
  if (!Entry.IsComplete() || !Entry.IsActive || Entry.Origin != OwnerIdentity)
    return false;

  UserdataCacheEntry *Existing = FindAddressForUpdate(Entry.Identity.Address);
  if (Existing == nullptr) {
    Records.push_back(Entry);
    return true;
  }

  // A live entry is never overwritten: that is what keeps one native identity
  // from acquiring a second header, and therefore a second owner.
  if (Existing->IsActive)
    return false;
  *Existing = Entry;
  return true;
}

bool UserdataIdentityCache::Inactivate(
    const NativeIdentity &Identity) noexcept {
  UserdataCacheEntry *Entry = FindForUpdate(Identity);
  if (Entry == nullptr || !Entry->IsActive)
    return false;
  Entry->IsActive = false;
  return true;
}

bool UserdataIdentityCache::Forget(const NativeIdentity &Identity) noexcept {
  if (!Identity.IsValid())
    return false;
  for (std::size_t Index = 0; Index < Records.size(); ++Index) {
    if (Records[Index].Origin != OwnerIdentity ||
        !(Records[Index].Identity == Identity))
      continue;
    Records.erase(Records.begin() + static_cast<std::ptrdiff_t>(Index));
    return true;
  }
  return false;
}

bool UserdataIdentityCache::ForgetAddress(const void *Address) noexcept {
  if (Address == nullptr)
    return false;
  for (std::size_t Index = 0; Index < Records.size(); ++Index) {
    if (Records[Index].Origin != OwnerIdentity ||
        Records[Index].Identity.Address != Address)
      continue;
    Records.erase(Records.begin() + static_cast<std::ptrdiff_t>(Index));
    return true;
  }
  return false;
}

std::size_t UserdataIdentityCache::ActiveCount() const noexcept {
  std::size_t Count = 0;
  for (const UserdataCacheEntry &Entry : Records) {
    if (Entry.IsActive)
      ++Count;
  }
  return Count;
}

} // namespace Luna::Detail
