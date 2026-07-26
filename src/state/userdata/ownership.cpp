// clang-format off
#include "state/userdata/ownership.hpp"

#include <luna/binding/class_allocator.hpp>
#include <luna/binding/lifetime_handle.hpp>

#include "state/userdata/allocator.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/identity.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {

std::string_view ReleaseCauseText(ReleaseCause Cause) noexcept {
  switch (Cause) {
  case ReleaseCause::ConstructionFailure:
    return "construction_failure";
  case ReleaseCause::PublicationFailure:
    return "publication_failure";
  case ReleaseCause::GarbageCollection:
    return "garbage_collection";
  case ReleaseCause::ExplicitInvalidation:
    return "explicit_invalidation";
  case ReleaseCause::LifecycleAction:
    return "lifecycle_action";
  case ReleaseCause::StateDestruction:
    return "state_destruction";
  }
  return "unknown";
}

std::uint64_t ObserveLifetimeHandleGeneration(const void *Record) noexcept {
  if (Record == nullptr)
    return 0;
  return static_cast<const LifetimeRecord *>(Record)->Generation();
}

std::string_view OwnershipFailureText(OwnershipFailure Failure) noexcept {
  switch (Failure) {
  case OwnershipFailure::None:
    return "none";
  case OwnershipFailure::UnknownIdentity:
    return "unknown_identity";
  case OwnershipFailure::StagedLifetime:
    return "staged_lifetime";
  case OwnershipFailure::MissingIdentity:
    return "missing_identity";
  case OwnershipFailure::MissingStorage:
    return "missing_storage";
  case OwnershipFailure::DuplicateIdentity:
    return "duplicate_identity";
  case OwnershipFailure::MissingLifetimeHandle:
    return "missing_lifetime_handle";
  case OwnershipFailure::ExpiredLifetimeHandle:
    return "expired_lifetime_handle";
  case OwnershipFailure::UnexpectedLifetimeHandle:
    return "unexpected_lifetime_handle";
  case OwnershipFailure::MissingSharedOwnership:
    return "missing_shared_ownership";
  case OwnershipFailure::UnexpectedSharedOwnership:
    return "unexpected_shared_ownership";
  case OwnershipFailure::UnreleasableStorage:
    return "unreleasable_storage";
  case OwnershipFailure::BorrowedStorageOwnership:
    return "borrowed_storage_ownership";
  case OwnershipFailure::OwnershipAlreadyEstablished:
    return "ownership_already_established";
  case OwnershipFailure::MissingOwnership:
    return "missing_ownership";
  case OwnershipFailure::RefusedConstruction:
    return "refused_construction";
  }
  return "unknown";
}

OwnershipOutcome OwnershipOutcome::Accept() noexcept {
  OwnershipOutcome Outcome;
  Outcome.Succeeded = true;
  return Outcome;
}

OwnershipOutcome OwnershipOutcome::Refuse(OwnershipFailure Reason) noexcept {
  OwnershipOutcome Outcome;
  Outcome.Succeeded = false;
  Outcome.Failure = Reason;
  return Outcome;
}

bool OwnershipRecord::RetainsCleanupMetadata() const noexcept {
  if (!Origin.IsValid() || !DynamicType.IsValid() ||
      !DeclaredViewType.IsValid() || !ClassSymbol.IsValid() ||
      !Metatable.IsValid())
    return false;

  // The protocol this value's storage came from is still named at all. Without
  // it no cleanup decision could be made.
  if (!Allocator.IsDeclared())
    return false;

  // A known-constructed Lua-owned object is only releasable while its
  // destruction step is still reachable.
  if (Ownership == OwnershipModel::LuaOwned && WasConstructed &&
      !Allocator.DeclaresDestruction())
    return false;
  return true;
}

bool OwnershipRecord::HasLiveHandle() const noexcept {
  if (Ownership != OwnershipModel::Borrowed)
    return true;
  if (!Handle)
    return false;

  // One comparison decides it: an invalidated handle has advanced past the
  // generation this value was published under.
  const std::uint64_t Current = Handle->Generation();
  return Handle->IsLive() && Current == HandleGeneration;
}

OwnershipRegistry::~OwnershipRegistry() {
  IsShuttingDown = true;
  static_cast<void>(ReleaseAll(ReleaseCause::StateDestruction));
}

void OwnershipRegistry::InstallCacheRemover(CacheRemover Remover) {
  RemoveFromCache = std::move(Remover);
}

OwnershipOutcome OwnershipRegistry::Stage(UserdataHeader &Header,
                                          const StagedStorage &Staged) {
  if (!Header.HasCanonicalLayout() ||
      Header.Lifetime != LifetimeState::Allocated)
    return OwnershipOutcome::Refuse(OwnershipFailure::StagedLifetime);
  if (!Staged.Identity.IsValid())
    return OwnershipOutcome::Refuse(OwnershipFailure::MissingIdentity);
  if (Staged.Storage == nullptr)
    return OwnershipOutcome::Refuse(OwnershipFailure::MissingStorage);
  if (Find(Staged.Identity) != nullptr)
    return OwnershipOutcome::Refuse(OwnershipFailure::DuplicateIdentity);

  // Luna never owns the storage of an object it only borrows, so a protocol
  // that would deallocate it is refused before anything is recorded.
  if (Header.Ownership == OwnershipModel::Borrowed &&
      Staged.Allocator.OwnsStorage())
    return OwnershipOutcome::Refuse(OwnershipFailure::BorrowedStorageOwnership);

  auto Owned = std::make_unique<OwnershipRecord>();
  Owned->Identity = Staged.Identity;
  Owned->Ownership = Header.Ownership;
  Owned->Lifetime = LifetimeState::Allocated;
  Owned->Storage = Staged.Storage;

  // Every value names the protocol it was created under, including the one that
  // declares no step: a value Luna neither created nor releases still has to
  // say so, and saying so is what makes its cleanup decisions readable.
  Owned->Allocator = Staged.Allocator.IsDeclared() ? Staged.Allocator
                                                   : BorrowedStorageProtocol();
  Owned->Origin = Header.Origin;
  Owned->DynamicType = Header.DynamicType;
  Owned->DeclaredViewType = Header.DeclaredViewType;
  Owned->ClassSymbol = Header.ClassSymbol;
  Owned->Metatable = Header.Metatable;
  Owned->DispatchGeneration = Header.DispatchGeneration;

  OwnershipRecord *Recorded = Owned.get();
  Records.push_back(std::move(Owned));

  Header.Identity = Staged.Identity;
  Header.Payload.Storage = Staged.Storage;
  Header.Payload.SharedOwnership = nullptr;

  // The value names the immutable allocator record its cleanup will use. Luna
  // retains that record here, so the record outlives the allocator value the
  // consumer supplied and stays valid through this value's final release step.
  Header.Allocator.Record = AllocatorRecordIdentity(Recorded->Allocator);
  return OwnershipOutcome::Accept();
}

StorageAllocationOutcome
OwnershipRegistry::Allocate(const ClassAllocator &Allocator) noexcept {
  const StorageAllocationOutcome Outcome = AllocateObjectStorage(Allocator);
  if (Outcome.ContainedException)
    ++Built.ContainedException;
  if (Outcome.Succeeded())
    ++Built.Allocate;
  else
    ++Built.AllocationFailure;
  return Outcome;
}

void OwnershipRegistry::DiscardStorage(const ClassAllocator &Allocator,
                                       void *Storage) noexcept {
  if (Storage == nullptr || !Allocator.OwnsStorage())
    return;

  // Nothing was constructed in this storage and no record ever described it, so
  // the one step it warrants is the deallocation it is given here.
  ++Counted.Deallocate;
  const AllocatorStepOutcome Given =
      DeallocateObjectStorage(Allocator, Storage);
  if (Given.ContainedException)
    ++Counted.ContainedException;
}

OwnershipOutcome OwnershipRegistry::Construct(UserdataHeader &Header,
                                              const ObjectConstruction &Build) {
  OwnershipRecord *Recorded = FindForUpdate(Header.Identity);
  if (Recorded == nullptr)
    return OwnershipOutcome::Refuse(OwnershipFailure::UnknownIdentity);
  if (Recorded->Lifetime != LifetimeState::Allocated)
    return OwnershipOutcome::Refuse(OwnershipFailure::StagedLifetime);

  const AllocatorStepOutcome Constructed =
      ConstructObject(Recorded->Allocator, Recorded->Storage, Build);
  if (Constructed.ContainedException)
    ++Built.ContainedException;

  if (!Constructed.Performed) {
    // No object exists, so the staged storage is simply given back: cleanup
    // deallocates without destroying anything it never constructed.
    ++Built.ConstructionFailure;
    static_cast<void>(Release(Header, ReleaseCause::ConstructionFailure));
    return OwnershipOutcome::Refuse(OwnershipFailure::RefusedConstruction);
  }

  ++Built.Construct;
  return Construct(Header);
}

OwnershipOutcome OwnershipRegistry::Construct(UserdataHeader &Header) {
  OwnershipRecord *Recorded = FindForUpdate(Header.Identity);
  if (Recorded == nullptr)
    return OwnershipOutcome::Refuse(OwnershipFailure::UnknownIdentity);
  if (Recorded->Lifetime != LifetimeState::Allocated)
    return OwnershipOutcome::Refuse(OwnershipFailure::StagedLifetime);

  Recorded->WasConstructed = true;
  Recorded->Lifetime = LifetimeState::Constructed;
  Header.Lifetime = LifetimeState::Constructed;
  return OwnershipOutcome::Accept();
}

OwnershipOutcome OwnershipRegistry::Establish(UserdataHeader &Header,
                                              const OwnershipRequest &Request) {
  OwnershipRecord *Recorded = FindForUpdate(Header.Identity);
  if (Recorded == nullptr)
    return OwnershipOutcome::Refuse(OwnershipFailure::UnknownIdentity);
  if (Recorded->Lifetime != LifetimeState::Constructed)
    return OwnershipOutcome::Refuse(OwnershipFailure::StagedLifetime);
  if (Recorded->OwnershipEstablished)
    return OwnershipOutcome::Refuse(
        OwnershipFailure::OwnershipAlreadyEstablished);

  const bool HasHandle = Request.Handle.IsDeclared();
  const bool HasShared = Request.SharedOwnership != nullptr;

  switch (Recorded->Ownership) {
  case OwnershipModel::Borrowed:
    // A borrowed object is never Luna's to delete, so the only thing that can
    // end it is the explicit lifetime the owner declared.
    if (!HasHandle)
      return OwnershipOutcome::Refuse(OwnershipFailure::MissingLifetimeHandle);
    if (!Request.Handle.IsValid())
      return OwnershipOutcome::Refuse(OwnershipFailure::ExpiredLifetimeHandle);
    if (HasShared)
      return OwnershipOutcome::Refuse(
          OwnershipFailure::UnexpectedSharedOwnership);
    break;

  case OwnershipModel::LuaOwned:
    // Exactly one destruction, so exactly one destruction step is required.
    if (HasHandle)
      return OwnershipOutcome::Refuse(
          OwnershipFailure::UnexpectedLifetimeHandle);
    if (HasShared)
      return OwnershipOutcome::Refuse(
          OwnershipFailure::UnexpectedSharedOwnership);
    if (!Recorded->Allocator.DeclaresDestruction())
      return OwnershipOutcome::Refuse(OwnershipFailure::UnreleasableStorage);
    break;

  case OwnershipModel::Shared:
    // Exactly one corresponding shared ownership reference per stored object.
    if (HasHandle)
      return OwnershipOutcome::Refuse(
          OwnershipFailure::UnexpectedLifetimeHandle);
    if (!HasShared)
      return OwnershipOutcome::Refuse(OwnershipFailure::MissingSharedOwnership);
    break;
  }

  if (Recorded->Ownership == OwnershipModel::Borrowed) {
    Recorded->Handle = LifetimeHandleAccess::Retain(Request.Handle);
    Recorded->HandleGeneration =
        LifetimeHandleAccess::Generation(Request.Handle);
    Header.Handle.Record = LifetimeHandleAccess::Identity(Request.Handle);
    Header.Handle.Generation = Recorded->HandleGeneration;
  }

  if (Recorded->Ownership == OwnershipModel::Shared) {
    Recorded->SharedOwnership = Request.SharedOwnership;
    Header.Payload.SharedOwnership = Recorded->SharedOwnership.get();
  }

  Recorded->OwnershipEstablished = true;
  return OwnershipOutcome::Accept();
}

OwnershipOutcome OwnershipRegistry::Publish(UserdataHeader &Header) {
  OwnershipRecord *Recorded = FindForUpdate(Header.Identity);
  if (Recorded == nullptr)
    return OwnershipOutcome::Refuse(OwnershipFailure::UnknownIdentity);
  if (Recorded->Lifetime != LifetimeState::Constructed)
    return OwnershipOutcome::Refuse(OwnershipFailure::StagedLifetime);
  if (!Recorded->OwnershipEstablished)
    return OwnershipOutcome::Refuse(OwnershipFailure::MissingOwnership);
  if (!Header.HasRequiredLifetimeHandle())
    return OwnershipOutcome::Refuse(OwnershipFailure::MissingLifetimeHandle);
  if (!Recorded->HasLiveHandle())
    return OwnershipOutcome::Refuse(OwnershipFailure::ExpiredLifetimeHandle);

  Recorded->Lifetime = LifetimeState::Published;
  Header.Lifetime = LifetimeState::Published;
  return OwnershipOutcome::Accept();
}

OwnershipOutcome OwnershipRegistry::Expose(UserdataHeader &Header,
                                           const StagedStorage &Staged,
                                           const OwnershipRequest &Request) {
  const OwnershipOutcome Staging = Stage(Header, Staged);
  if (!Staging.Succeeded)
    return Staging;

  const OwnershipOutcome Constructed = Construct(Header);
  if (!Constructed.Succeeded) {
    static_cast<void>(Release(Header, ReleaseCause::ConstructionFailure));
    return Constructed;
  }

  const OwnershipOutcome Established = Establish(Header, Request);
  if (!Established.Succeeded) {
    static_cast<void>(Release(Header, ReleaseCause::PublicationFailure));
    return Established;
  }

  const OwnershipOutcome Published = Publish(Header);
  if (!Published.Succeeded) {
    static_cast<void>(Release(Header, ReleaseCause::PublicationFailure));
    return Published;
  }
  return Published;
}

bool OwnershipRegistry::Invalidate(UserdataHeader &Header,
                                   ReleaseCause Cause) noexcept {
  static_cast<void>(Cause);
  OwnershipRecord *Recorded = FindForUpdate(Header.Identity);
  if (Recorded == nullptr)
    return false;
  if (Recorded->Lifetime != LifetimeState::Constructed &&
      Recorded->Lifetime != LifetimeState::Published)
    return false;

  InvalidateRecord(*Recorded, &Header);
  return true;
}

bool OwnershipRegistry::Release(UserdataHeader &Header,
                                ReleaseCause Cause) noexcept {
  OwnershipRecord *Recorded = FindForUpdate(Header.Identity);
  if (Recorded == nullptr)
    return false;

  const NativeIdentity Identity = Recorded->Identity;
  const bool Released = ReleaseRecord(*Recorded, Cause, &Header);
  Drop(Identity);
  return Released;
}

bool OwnershipRegistry::ReleaseCollected(UserdataHeader &Header) noexcept {
  // Collection is one more cause of the same gate. The only thing it changes is
  // that for the duration of this call no step may re-enter the virtual
  // machine, because the collector that called it is still traversing.
  const bool WasFinalizing = IsFinalizing;
  IsFinalizing = true;
  const bool Released = Release(Header, ReleaseCause::GarbageCollection);
  IsFinalizing = WasFinalizing;
  return Released;
}

bool OwnershipRegistry::ReleaseByStorage(const void *Address,
                                         ReleaseCause Cause) noexcept {
  if (Address == nullptr)
    return false;

  OwnershipRecord *Found = nullptr;
  for (const std::unique_ptr<OwnershipRecord> &Held : Records) {
    if (Held && Held->Storage == Address) {
      Found = Held.get();
      break;
    }
  }
  if (Found == nullptr)
    return false;

  const NativeIdentity Identity = Found->Identity;
  const bool Released = ReleaseRecord(*Found, Cause, nullptr);
  Drop(Identity);
  return Released;
}

std::size_t OwnershipRegistry::ReleaseAll(ReleaseCause Cause) noexcept {
  std::size_t Count = 0;
  for (const std::unique_ptr<OwnershipRecord> &Held : Records) {
    if (!Held)
      continue;
    if (ReleaseRecord(*Held, Cause, nullptr))
      ++Count;
  }
  Records.clear();
  return Count;
}

bool OwnershipRegistry::LifetimePermitsAccess(
    const UserdataHeader &Header) const noexcept {
  if (!Header.HasCanonicalLayout() || !Header.HasLiveLifetime())
    return false;
  if (!Header.HasRequiredLifetimeHandle())
    return false;

  const OwnershipRecord *Recorded = Find(Header.Identity);
  if (Recorded == nullptr || Recorded->Lifetime != LifetimeState::Published)
    return false;
  if (Recorded->Ownership != Header.Ownership)
    return false;
  if (Header.Ownership != OwnershipModel::Borrowed)
    return true;

  // The value is only reachable while its declared lifetime is still exactly
  // the one it was published under.
  return Recorded->HasLiveHandle() &&
         Header.Handle.Generation == Recorded->HandleGeneration;
}

const OwnershipRecord *
OwnershipRegistry::Find(const NativeIdentity &Identity) const noexcept {
  if (!Identity.IsValid())
    return nullptr;
  for (const std::unique_ptr<OwnershipRecord> &Held : Records) {
    if (Held && Held->Identity == Identity)
      return Held.get();
  }
  return nullptr;
}

std::vector<const OwnershipRecord *> OwnershipRegistry::OwnedValues() const {
  std::vector<const OwnershipRecord *> Owned;
  Owned.reserve(Records.size());
  for (const std::unique_ptr<OwnershipRecord> &Held : Records) {
    if (Held)
      Owned.push_back(Held.get());
  }
  std::sort(Owned.begin(), Owned.end(),
            [](const OwnershipRecord *Left, const OwnershipRecord *Right) {
              return Left->Identity.Nonce < Right->Identity.Nonce;
            });
  return Owned;
}

std::size_t OwnershipRegistry::PublishedCount() const noexcept {
  std::size_t Count = 0;
  for (const std::unique_ptr<OwnershipRecord> &Held : Records) {
    if (Held && Held->Lifetime == LifetimeState::Published)
      ++Count;
  }
  return Count;
}

bool OwnershipRegistry::RetainsCleanupMetadata() const noexcept {
  for (const std::unique_ptr<OwnershipRecord> &Held : Records) {
    if (!Held)
      return false;

    // A value whose every applicable step has already run needs nothing more.
    if (Held->Lifetime == LifetimeState::Released)
      continue;
    if (!Held->RetainsCleanupMetadata())
      return false;
  }
  return true;
}

OwnershipRecord *
OwnershipRegistry::FindForUpdate(const NativeIdentity &Identity) noexcept {
  if (!Identity.IsValid())
    return nullptr;
  for (const std::unique_ptr<OwnershipRecord> &Held : Records) {
    if (Held && Held->Identity == Identity)
      return Held.get();
  }
  return nullptr;
}

void OwnershipRegistry::InvalidateRecord(OwnershipRecord &Record,
                                         UserdataHeader *Header) noexcept {
  if (Record.Lifetime != LifetimeState::Constructed &&
      Record.Lifetime != LifetimeState::Published)
    return;

  ++Counted.Invalidate;
  Record.Lifetime = LifetimeState::Invalid;
  if (Header != nullptr)
    Header->Lifetime = LifetimeState::Invalid;
}

void OwnershipRegistry::RemoveCacheEntry(
    const OwnershipRecord &Record) noexcept {
  ++Counted.CacheRemoval;
  if (IsShuttingDown || !RemoveFromCache)
    return;

  // Nothing a cache remover does may escape a garbage collector or a State
  // destructor.
  try {
    RemoveFromCache(Record.Identity);
  } catch (...) {
    ++Counted.ContainedException;
  }
}

bool OwnershipRegistry::ReleaseRecord(OwnershipRecord &Record,
                                      ReleaseCause Cause,
                                      UserdataHeader *Header) noexcept {
  static_cast<void>(Cause);
  if (Record.Lifetime == LifetimeState::Released)
    return false;

  // Access stops first, and it stops for every cause. A value that never got
  // past `Allocated` was never accessible, so it moves straight to release.
  if (Record.Lifetime != LifetimeState::Allocated)
    InvalidateRecord(Record, Header);

  // The identity-cache entry goes before the payload it points at.
  RemoveCacheEntry(Record);

  if (!Record.RetainsCleanupMetadata())
    ++Counted.IncompleteMetadata;

  // Native destruction, only for an object Luna owns and only for one that was
  // actually constructed. A borrowed object is never destroyed here, and
  // storage nothing was ever constructed in is never destroyed at all.
  if (Record.Ownership == OwnershipModel::LuaOwned && Record.WasConstructed) {
    ++Counted.Destroy;
    const AllocatorStepOutcome Destroyed =
        DestroyKnownConstructedObject(Record.Allocator, Record.Storage);
    if (Destroyed.ContainedException)
      ++Counted.ContainedException;
    Record.WasConstructed = false;
    Record.Lifetime = LifetimeState::Destroyed;
    if (Header != nullptr)
      Header->Lifetime = LifetimeState::Destroyed;
  }

  // Exactly one shared ownership reference is released, exactly once.
  if (Record.SharedOwnership) {
    ++Counted.SharedRelease;
    try {
      Record.SharedOwnership.reset();
    } catch (...) {
      ++Counted.ContainedException;
    }
    if (Record.Lifetime != LifetimeState::Destroyed) {
      Record.Lifetime = LifetimeState::SharedReleased;
      if (Header != nullptr)
        Header->Lifetime = LifetimeState::SharedReleased;
    }
  }

  // Storage Luna owns is deallocated last, whether or not anything was ever
  // constructed in it.
  if (Record.Allocator.OwnsStorage() && Record.Storage != nullptr) {
    ++Counted.Deallocate;
    const AllocatorStepOutcome Given =
        DeallocateObjectStorage(Record.Allocator, Record.Storage);
    if (Given.ContainedException)
      ++Counted.ContainedException;
  }

  // Only now does Luna let go of the metadata cleanup needed, the retained
  // allocator protocol included: this value was the last thing keeping its own
  // claim on it.
  ++Counted.MetadataRelease;
  Record.Storage = nullptr;
  Record.Handle.reset();
  Record.Allocator = ClassAllocator();
  Record.Lifetime = LifetimeState::Released;

  if (Header != nullptr) {
    Header->Lifetime = LifetimeState::Released;
    Header->Payload = OwnershipPayload{};
    Header->Handle = LifetimeHandleReference{};
    Header->Allocator = AllocatorRecordReference{};
  }
  return true;
}

void OwnershipRegistry::Drop(const NativeIdentity &Identity) noexcept {
  for (std::size_t Index = 0; Index < Records.size(); ++Index) {
    const std::unique_ptr<OwnershipRecord> &Held = Records[Index];
    if (!Held || !(Held->Identity == Identity))
      continue;
    Records.erase(Records.begin() + static_cast<std::ptrdiff_t>(Index));
    return;
  }
}

} // namespace Luna::Detail
