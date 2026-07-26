#pragma once

// The write half of typed userdata: turning one native object into exactly one
// Luau value, with exactly one owner.
//
// Reading a class value validates what already exists; writing one decides what
// comes into existence, so this is where ownership is established. Every
// exposure states its ownership model explicitly: a borrowed object arrives
// with the explicit `LifetimeHandle` its owner holds, a Lua-owned object
// arrives with the destruction and deallocation steps Luna will run exactly
// once, and a shared object arrives with exactly one `std::shared_ptr`
// ownership reference. An exposure that cannot satisfy its own model is refused
// before any value exists.
//
// Order matters and it is fixed. The identity cache decides first, so one
// native object exposed twice in one State is one value rather than two owners
// of one object, and a request that conflicts in ownership, type, or view is
// refused without creating a second owner. Then the release gate stages,
// constructs, and establishes ownership; only after the virtual-machine block,
// its class metatable, and its cache entry are all in place does the gate
// publish the value, which is the single state that permits native access. Any
// failure in between releases exactly what the earlier steps established.
//
// Nothing here decides how release happens: `OwnershipRegistry` owns that, and
// this path always goes through it, which is why a value written here is
// released exactly once whichever cause ends it.

// clang-format off
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/lifetime_handle.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/transaction/lifecycle.hpp"
#include "state/userdata/access.hpp"
#include "state/userdata/allocator.hpp"
#include "state/userdata/identity.hpp"
#include "state/userdata/ownership.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class ClassRegistry;
class TypeGeneration;
class UserdataIdentityCache;
struct RegisteredClass;

// The ownership statement of one value Luna is about to expose. It is the whole
// input of ownership establishment: which model applies, which view is exposed,
// the explicit lifetime of a borrowed object, the one shared ownership
// reference of a shared object, and the destruction and deallocation steps of
// storage Luna owns.
struct ClassExposureIntent final {
  void *Storage = nullptr;
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  ConstAccess Access = ConstAccess::Mutable;

  // The explicit lifetime a borrowed value requires. Invalidating it rejects
  // every later access to this value atomically, through one generation
  // comparison.
  LifetimeHandle Handle = LifetimeHandle::Undeclared();

  // The one shared ownership reference of a shared value. Luna retains exactly
  // this one and releases exactly this one.
  std::shared_ptr<void> SharedOwnership;

  // The semantic protocol this value's storage comes from and goes back to. Its
  // declared steps are the whole cleanup rule: a borrowed object arrives with a
  // protocol that declares nothing, an object Luna destroys arrives with a
  // destruction step, and an object Luna allocated arrives with the
  // deallocation step that gives that storage back.
  ClassAllocator Allocator;

  // The one construction step of a value Luna is creating rather than adopting.
  // When it is declared, `Storage` is left null and the protocol's allocation
  // step produces the storage this step constructs into; the exposure then owns
  // every milestone, so a refusal anywhere performs exactly the cleanup the
  // completed milestones warrant.
  ObjectConstruction Construct;
};

// Everything the write path needs from the State that exposes the value. Its
// address is stable across every move of its State, which is what lets one
// Luna-private virtual-machine slot name it.
struct UserdataExposureContext final {
  StateIdentity Origin;

  // The class registry is mutable here, because the first exposure of a class
  // creates and retains that class's metatable.
  ClassRegistry *Classes = nullptr;
  UserdataIdentityCache *Cache = nullptr;
  NativeIdentitySource *Nonces = nullptr;
  OwnershipRegistry *Ownership = nullptr;

  // The access context a value written here is later read through. The write
  // path publishes it, so a value it created is readable by exactly the
  // ordinary access gate.
  UserdataAccessContext *Access = nullptr;

  // The generation the value is published under. Until dispatch indirection
  // exists this is the lifecycle generation of the publication.
  std::uint64_t DispatchGeneration = 0;

  [[nodiscard]] bool IsUsable() const noexcept {
    return Origin.IsValid() && Classes != nullptr && Cache != nullptr &&
           Nonces != nullptr && Ownership != nullptr && Access != nullptr;
  }
};

// Why one exposure did not produce a value, or which of the two ways it did.
enum class ClassExposureStatus : std::uint8_t {
  // One new value was created, owned, published, and recorded.
  Created,

  // The value already live for this native identity was handed back, with no
  // second owner and no second ownership record.
  Reused,

  // The request or the registered class is incomplete: Luna's own mistake.
  UnavailableRequest,

  // The object has no storage to expose, and no protocol that could create it.
  NullStorage,

  // The semantic allocation step produced no storage, so nothing was cleaned up
  // after it: there was nothing to clean up.
  StorageUnavailable,

  // The cache refused the request: the object is already exposed under a
  // different ownership model, type, or view.
  ConflictingOwnership,
  IncompatibleType,
  IncompatibleAccess,

  // Ownership establishment refused the request; `Ownership` names which
  // deterministic reason it was.
  UnestablishedOwnership,

  // The class metatable could not be created or retained.
  MetatableUnavailable,

  // The virtual machine could not reserve the slots the exposure needs, or the
  // exposure itself failed inside the virtual machine.
  StackCapacityFailure,
  ProtectedFailure
};

[[nodiscard]] std::string_view
ClassExposureStatusText(ClassExposureStatus Status) noexcept;

struct ClassExposureResult final {
  ClassExposureStatus Status = ClassExposureStatus::UnavailableRequest;

  // The deterministic ownership reason behind an `UnestablishedOwnership`
  // status, and `None` for every other status.
  OwnershipFailure Ownership = OwnershipFailure::None;

  // The state-local identity the value carries. A reused value keeps the
  // identity it was first exposed with.
  NativeIdentity Identity;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Status == ClassExposureStatus::Created ||
           Status == ClassExposureStatus::Reused;
  }
};

// Whether one intent creates its object rather than adopting one that already
// exists. A creating intent names no storage yet, because its storage does not
// exist until its protocol's allocation step runs.
[[nodiscard]] bool
DeclaresObjectConstruction(const ClassExposureIntent &Intent) noexcept;

// Exposes one native object of one registered class as exactly one value left
// on top of the stack. On success the value is published and its ownership is
// established; on failure nothing is left on the stack, no record survives, and
// no second owner of the object exists.
[[nodiscard]] ClassExposureResult
PushExposedClassValue(lua_State *State, UserdataExposureContext &Context,
                      RegisteredClass &Registered,
                      const ClassExposureIntent &Intent);

// Names one State's exposure context in that State's virtual machine, so the
// conversion write path can resolve the class registry, the identity cache, the
// nonce source, and the release gate without carrying them through every
// conversion signature. Publication is idempotent.
[[nodiscard]] bool
PublishUserdataExposureContext(lua_State *State,
                               UserdataExposureContext *Context) noexcept;

// The exposure context of the State this virtual machine belongs to, or null
// when no class was ever registered in it.
[[nodiscard]] UserdataExposureContext *
ObserveUserdataExposureContext(lua_State *State) noexcept;

// What one attempted write of a class value observed.
struct ClassValueWriteObservation final {
  // True only when exactly one value was published and stored at the path.
  bool Published = false;
  int PublishedCount = 0;

  // The deterministic reason and message of a refusal.
  std::string Failure;
  std::string Diagnostic;

  // Stack depth before and after the attempt. A refused write publishes nothing
  // and leaves the depth exactly as it found it.
  int EntryStackDepth = 0;
  int FinalStackDepth = 0;
};

// Writes one native object as a value of one registered class at a canonical
// path, through exactly the conversion write path a returned value takes.
// Nothing else in Luna exposes a class value differently, so this is the same
// gate a constructor, a factory, or a returned object will pass.
[[nodiscard]] ClassValueWriteObservation
WriteExposedClassValue(lua_State *State, const TypeGeneration &Types,
                       const StableTypeKey &Key, const std::string &Path,
                       std::shared_ptr<const ClassExposureIntent> Intent);

} // namespace Luna::Detail
