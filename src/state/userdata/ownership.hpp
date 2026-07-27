#pragma once

// clang-format off
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/lifetime_handle.hpp>
#include <luna/reflection/ids.hpp>

#include "state/transaction/lifecycle.hpp"
#include "state/userdata/allocator.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/identity.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

enum class ReleaseCause : std::uint8_t {
  ConstructionFailure,
  PublicationFailure,
  GarbageCollection,
  ExplicitInvalidation,
  LifecycleAction,
  StateDestruction
};

[[nodiscard]] std::string_view ReleaseCauseText(ReleaseCause Cause) noexcept;

[[nodiscard]] std::uint64_t
ObserveLifetimeHandleGeneration(const void *Record) noexcept;

struct ConstructionCounters final {
  std::uint64_t Allocate = 0;
  std::uint64_t AllocationFailure = 0;
  std::uint64_t Construct = 0;
  std::uint64_t ConstructionFailure = 0;

  std::uint64_t ContainedException = 0;
};

struct ReleaseCounters final {
  std::uint64_t Invalidate = 0;
  std::uint64_t CacheRemoval = 0;
  std::uint64_t Destroy = 0;
  std::uint64_t SharedRelease = 0;
  std::uint64_t Deallocate = 0;
  std::uint64_t MetadataRelease = 0;

  std::uint64_t ContainedException = 0;
  std::uint64_t IncompleteMetadata = 0;
};

enum class OwnershipFailure : std::uint8_t {
  None,
  UnknownIdentity,
  StagedLifetime,
  MissingIdentity,
  MissingStorage,
  DuplicateIdentity,
  MissingLifetimeHandle,
  ExpiredLifetimeHandle,
  UnexpectedLifetimeHandle,
  MissingSharedOwnership,
  UnexpectedSharedOwnership,
  UnreleasableStorage,
  BorrowedStorageOwnership,
  OwnershipAlreadyEstablished,
  MissingOwnership,

  RefusedConstruction
};

[[nodiscard]] std::string_view
OwnershipFailureText(OwnershipFailure Failure) noexcept;

struct OwnershipOutcome final {
  bool Succeeded = false;
  OwnershipFailure Failure = OwnershipFailure::None;

  [[nodiscard]] static OwnershipOutcome Accept() noexcept;
  [[nodiscard]] static OwnershipOutcome
  Refuse(OwnershipFailure Reason) noexcept;
};

struct StagedStorage final {
  void *Storage = nullptr;
  NativeIdentity Identity;

  ClassAllocator Allocator;
};

struct OwnershipRequest final {
  LifetimeHandle Handle = LifetimeHandle::Undeclared();
  std::shared_ptr<void> SharedOwnership;
};

struct OwnershipRecord final {
  NativeIdentity Identity;
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  LifetimeState Lifetime = LifetimeState::Allocated;

  bool WasConstructed = false;
  bool OwnershipEstablished = false;

  void *Storage = nullptr;

  ClassAllocator Allocator;

  std::shared_ptr<const LifetimeRecord> Handle;
  std::uint64_t HandleGeneration = 0;

  std::shared_ptr<void> SharedOwnership;

  StateIdentity Origin;
  TypeId DynamicType;
  TypeId DeclaredViewType;
  SymbolId ClassSymbol;
  MetatableId Metatable;
  std::uint64_t DispatchGeneration = 0;

  [[nodiscard]] bool RetainsCleanupMetadata() const noexcept;

  [[nodiscard]] bool HasLiveHandle() const noexcept;
};

class OwnershipRegistry final {
public:
  using CacheRemover = std::function<void(const NativeIdentity &)>;

  OwnershipRegistry() = default;

  OwnershipRegistry(const OwnershipRegistry &) = delete;
  OwnershipRegistry &operator=(const OwnershipRegistry &) = delete;
  OwnershipRegistry(OwnershipRegistry &&) = delete;
  OwnershipRegistry &operator=(OwnershipRegistry &&) = delete;

  ~OwnershipRegistry();

  void InstallCacheRemover(CacheRemover Remover);

  [[nodiscard]] StorageAllocationOutcome
  Allocate(const ClassAllocator &Allocator) noexcept;

  void DiscardStorage(const ClassAllocator &Allocator, void *Storage) noexcept;

  [[nodiscard]] OwnershipOutcome Stage(UserdataHeader &Header,
                                       const StagedStorage &Staged);

  [[nodiscard]] OwnershipOutcome Construct(UserdataHeader &Header);

  [[nodiscard]] OwnershipOutcome Construct(UserdataHeader &Header,
                                           const ObjectConstruction &Build);

  [[nodiscard]] OwnershipOutcome Establish(UserdataHeader &Header,
                                           const OwnershipRequest &Request);

  [[nodiscard]] OwnershipOutcome Publish(UserdataHeader &Header);

  [[nodiscard]] OwnershipOutcome Expose(UserdataHeader &Header,
                                        const StagedStorage &Staged,
                                        const OwnershipRequest &Request);

  bool Invalidate(UserdataHeader &Header, ReleaseCause Cause) noexcept;

  bool Release(UserdataHeader &Header, ReleaseCause Cause) noexcept;

  bool ReleaseCollected(UserdataHeader &Header) noexcept;

  [[nodiscard]] bool PermitsVirtualMachineAccess() const noexcept {
    return !IsFinalizing && !IsShuttingDown;
  }

  bool ReleaseByStorage(const void *Address, ReleaseCause Cause) noexcept;

  std::size_t ReleaseAll(ReleaseCause Cause) noexcept;

  [[nodiscard]] bool
  LifetimePermitsAccess(const UserdataHeader &Header) const noexcept;

  [[nodiscard]] const OwnershipRecord *
  Find(const NativeIdentity &Identity) const noexcept;

  [[nodiscard]] std::vector<const OwnershipRecord *> OwnedValues() const;

  [[nodiscard]] std::size_t RecordCount() const noexcept {
    return Records.size();
  }
  [[nodiscard]] std::size_t PublishedCount() const noexcept;

  [[nodiscard]] bool RetainsCleanupMetadata() const noexcept;

  [[nodiscard]] ReleaseCounters Counters() const noexcept { return Counted; }

  [[nodiscard]] ConstructionCounters ConstructionCounts() const noexcept {
    return Built;
  }

private:
  [[nodiscard]] OwnershipRecord *
  FindForUpdate(const NativeIdentity &Identity) noexcept;

  bool ReleaseRecord(OwnershipRecord &Record, ReleaseCause Cause,
                     UserdataHeader *Header) noexcept;
  void InvalidateRecord(OwnershipRecord &Record,
                        UserdataHeader *Header) noexcept;
  void RemoveCacheEntry(const OwnershipRecord &Record) noexcept;
  void Drop(const NativeIdentity &Identity) noexcept;

  std::vector<std::unique_ptr<OwnershipRecord>> Records;
  CacheRemover RemoveFromCache;
  ReleaseCounters Counted;
  ConstructionCounters Built;

  bool IsShuttingDown = false;

  bool IsFinalizing = false;
};

} // namespace Luna::Detail
