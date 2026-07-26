#pragma once

// The virtual-machine half of typed userdata: the lazily created class
// metatable, the value block itself, the weak identity slot, and the
// Luna-private slot that names one State's access context.
//
// Nothing here decides ownership transitions. It creates or reuses exactly one
// value per native identity under the cache policy, writes the versioned
// header, associates the class metatable, and records the exposure; the release
// gate owns destruction, shared release, and deallocation. What it does own is
// the eviction that must happen before any payload release, so a released
// object can never be reached through the cache or through a value the cache
// handed back.
//
// The class metatable is created on first exposure rather than at registration:
// a class no value was ever created of installs nothing in the virtual machine.
// Once created it is retained by the registered class for the life of the
// State, so every value of that class carries exactly one metatable, and the
// metatable itself is protected against script replacement.

// clang-format off
#include <luna/type/stable_type_key.hpp>

#include "state/userdata/access.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/identity.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class TypeGeneration;
struct RegisteredClass;

// Why one exposure did not produce a value, or which of the two ways it did.
enum class UserdataExposureStatus : std::uint8_t {
  // One new value was created, published, and recorded.
  Created,

  // The value already live for this native identity was handed back.
  Reused,

  // The exposure request or the registered class is incomplete.
  UnavailableRequest,

  // The object has no storage to expose.
  NullStorage,

  // A borrowed value was requested without the explicit lifetime handle it
  // requires.
  MissingLifetimeHandle,

  // The cache refused the request: the object is already exposed under a
  // different ownership model, type, or view.
  ConflictingOwnership,
  IncompatibleType,
  IncompatibleAccess,

  // The class metatable could not be created or retained.
  MetatableUnavailable,

  // The canonical path the value would be published at is unavailable.
  PathUnavailable,

  // The virtual machine could not reserve the slots the exposure needs, or the
  // protected exposure itself failed.
  StackCapacityFailure,
  ProtectedFailure
};

[[nodiscard]] std::string_view
UserdataExposureStatusText(UserdataExposureStatus Status) noexcept;

struct UserdataExposureRequest final {
  // The native object being exposed.
  void *Storage = nullptr;

  OwnershipModel Ownership = OwnershipModel::Borrowed;
  ConstAccess Access = ConstAccess::Mutable;

  // The explicit lifetime handle a borrowed value requires, the allocator
  // record that owns its storage when it has one, and the dispatch generation
  // it is published under.
  LifetimeHandleReference Handle;
  AllocatorRecordReference Allocator;
  std::uint64_t DispatchGeneration = 0;

  // The canonical virtual-machine path the value is published at. Every parent
  // segment must already exist.
  std::string Path;

  // The erased holder of exactly one shared ownership reference, for a shared
  // value. The release gate owns its release.
  void *SharedOwnership = nullptr;
};

struct UserdataExposure final {
  UserdataExposureStatus Status = UserdataExposureStatus::UnavailableRequest;
  NativeIdentity Identity;
  const void *Block = nullptr;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Status == UserdataExposureStatus::Created ||
           Status == UserdataExposureStatus::Reused;
  }
};

// Exposes one native object of one registered class at a canonical path. The
// cache decides first: an object already live under a compatible view and
// ownership is handed back as exactly the value that already exists, and an
// incompatible request is refused without creating a second owner.
[[nodiscard]] UserdataExposure
ExposeUserdataValue(lua_State *State, UserdataAccessContext &Context,
                    RegisteredClass &Registered, NativeIdentitySource &Nonces,
                    const UserdataExposureRequest &Request) noexcept;

// Evicts one exposed value ahead of any payload release: access is invalidated,
// the cache entry is inactivated and dropped, and the weak virtual-machine slot
// is removed. Nothing is destroyed and nothing is deallocated here - the
// release gate performs those steps after this one, which is what guarantees no
// stale value and no cache entry can reach a payload that is being released.
// Running it twice is harmless.
[[nodiscard]] bool
RetireExposedUserdata(lua_State *State, UserdataAccessContext &Context,
                      const NativeIdentity &Identity) noexcept;

// The Luna-owned metatable of one registered class, pushed onto the stack. It
// is created on the first exposure of that class and retained by the class
// afterwards, so every value of the class carries exactly one metatable. The
// write path shares this one creator rather than making a second metatable of
// its own.
[[nodiscard]] bool PushRegisteredClassMetatable(lua_State *State,
                                                RegisteredClass &Registered);

// The weak virtual-machine half of the identity cache. Pushing hands back the
// value one address still has, if the virtual machine still holds it; storing
// records one created value; dropping removes the slot ahead of a release. The
// table is weak in its values, so collecting a value drops its slot without
// Luna traversing anything.
[[nodiscard]] bool PushCachedUserdataValue(lua_State *State,
                                           const void *Address);
void StoreCachedUserdataValue(lua_State *State, const void *Address,
                              int ValueIndex);
void DropCachedUserdataValue(lua_State *State, const void *Address);

// Names one State's access context in that State's virtual machine, so a
// validated access can resolve the logical State identity, the registered
// classes, and the identity cache without carrying them through every
// conversion signature. Publication is idempotent.
[[nodiscard]] bool
PublishUserdataAccessContext(lua_State *State,
                             UserdataAccessContext *Context) noexcept;

// The access context of the State this virtual machine belongs to, or null when
// no typed userdata was ever exposed in it.
[[nodiscard]] const UserdataAccessContext *
ObserveUserdataAccessContext(lua_State *State) noexcept;

// The header of the value one canonical path holds, copied out for observation.
// It never hands out the block itself.
[[nodiscard]] bool ObserveExposedUserdataHeader(lua_State *State,
                                                const std::string &Path,
                                                UserdataHeader &Observed);

// One operation over the live header of the value a canonical path holds, run
// inside a protected call. This is how a member access reaches the header slots
// it owns - the lazy cache slot above all - without any caller ever holding the
// block itself. The operation must not re-enter the virtual machine.
using ExposedUserdataVisitor = std::function<void(UserdataHeader &)>;

[[nodiscard]] bool
VisitExposedUserdataHeader(lua_State *State, const std::string &Path,
                           const ExposedUserdataVisitor &Visit);

// What one attempted access observed.
struct UserdataHandleObservation final {
  // True only when every check passed and a native pointer was produced.
  bool ReachedNativeCode = false;
  void *Storage = nullptr;
  bool PermitsMutation = false;

  // The deterministic reason and message of a refusal.
  std::string Failure;
  std::string Diagnostic;
};

// Reads the value one canonical path holds as a handle of one registered class,
// through exactly the conversion path an ordinary argument takes. Nothing else
// in Luna reads a class value differently, so this is the same gate a method
// receiver or an argument passes.
[[nodiscard]] UserdataHandleObservation
ReadExposedUserdataHandle(lua_State *State, const TypeGeneration &Types,
                          const StableTypeKey &Key, const std::string &Path);

} // namespace Luna::Detail
