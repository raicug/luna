#pragma once

// The one gate between a Luau value and a native object.
//
// Nothing reaches native code without passing every check below, in exactly
// this order:
//
//   1. the request itself names one complete registered class of one State;
//   2. a value is present at all;
//   3. the block carries Luna's marker and exactly this layout version;
//   4. the value was exposed by this logical State;
//   5. the value carries exactly the metatable identity of the requested class;
//   6. the payload is not null, its lifetime handle is present and current, and
//      the value is published rather than unpublished, invalidated, destroyed,
//      or released;
//   7. the value's dynamic type is the requested type, or one registered
//      accessible path leads from it to the requested view;
//   8. a requested safe downcast accepts this object;
//   9. a mutating access is permitted by the view.
//
// The order is deliberate and observable: a value that fails several checks
// always reports the earliest one, so one diagnostic never depends on which
// check happened to run first. Metatable equality alone is never proof of type
// or origin, which is why steps 4, 5, and 7 are separate questions.
//
// This header names no Luau type. It validates one header value, so it is
// equally usable from the conversion path, the member-dispatch path, and the
// release path.

// clang-format off
#include <luna/reflection/ids.hpp>

#include "state/transaction/lifecycle.hpp"
#include "state/userdata/class_relationships.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/identity.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
// clang-format on

namespace Luna::Detail {

class ClassRegistry;
class FaultInjector;
class LazyPropertyCache;
class MemberDispatchRecorder;
class TypeGenerationSource;
class UserdataIdentityCache;

// Why one access never reached native code. The enumerator order is exactly the
// order the checks run in, so a caller can compare two failures to prove which
// check is earlier without repeating the order itself.
enum class UserdataAccessFailure : std::uint8_t {
  None,

  // The access request does not name one complete registered class of one
  // logical State. This is Luna's own mistake, never the caller's.
  UnavailableRequest,

  // The stack position, table field, or member holds no value at all.
  MissingValue,

  // The block is not a Luna userdata, or it was written by another layout
  // version.
  ForeignLayout,

  // The value was exposed by a different logical State.
  ForeignState,

  // The value does not carry the metatable identity of the requested class.
  MetatableMismatch,

  // The value carries no native payload.
  NullPayload,

  // A borrowed value arrived without the explicit lifetime handle it requires.
  MissingLifetimeHandle,

  // The value's lifetime handle was invalidated, so every later access fails.
  ExpiredLifetimeHandle,

  // The value was never published.
  Unpublished,

  // The value was explicitly invalidated, or its final release began.
  Invalidated,

  // The value's native object was destroyed.
  Destroyed,

  // The value's ownership reference and storage were released.
  Released,

  // Neither the dynamic type nor any registered cast path leads to the
  // requested view type.
  TypeMismatch,

  // One registered safe downcast policy leads to the requested view, but its
  // non-mutating compatibility check refused this object.
  IncompatibleObject,

  // A mutating access arrived at a const view.
  ConstViolation
};

[[nodiscard]] std::string_view
UserdataAccessFailureText(UserdataAccessFailure Failure) noexcept;

// The generation one lifetime handle currently reports. Invalidation advances
// that generation, so a value whose recorded generation no longer matches is
// expired before any native pointer exists. The probe is supplied by whoever
// owns the handle records, which keeps this gate independent of their layout.
using LifetimeHandleGenerationProbe =
    std::uint64_t (*)(const void *Record) noexcept;

// What one access asks for.
struct UserdataAccessRequest final {
  // The logical State the access runs in.
  StateIdentity Origin;

  // The metatable identity the requested class owns in that State.
  MetatableId Metatable;

  // The canonical type native code will see.
  TypeId RequestedType;

  // True when the access mutates the object: a non-const method, a setter, or a
  // writable field.
  bool RequiresMutation = false;

  LifetimeHandleGenerationProbe HandleProbe = nullptr;

  // The explicit relationship graph of the State the access runs in. A State
  // that declared no relationship at all supplies none, and then a value only
  // ever reaches the class it was exposed as.
  const ClassRelationships *Relationships = nullptr;

  [[nodiscard]] bool IsComplete() const noexcept {
    return Origin.IsValid() && Metatable.IsValid() && RequestedType.IsValid();
  }
};

// The outcome of one access. `Storage` is non-null only when every check
// passed, so a caller cannot obtain a native pointer by ignoring the failure.
struct UserdataAccessResult final {
  UserdataAccessFailure Failure = UserdataAccessFailure::UnavailableRequest;
  const UserdataHeader *Header = nullptr;

  // `Storage` is the pointer the requested view sees, so it is already adjusted
  // by the path the access resolved through.
  void *Storage = nullptr;
  ClassConversionKind Conversion = ClassConversionKind::Identity;
  bool PermitsMutation = false;

  [[nodiscard]] bool IsPermitted() const noexcept {
    return Failure == UserdataAccessFailure::None && Storage != nullptr;
  }
};

// Validates one header that is already known to live in a block of the right
// size.
[[nodiscard]] UserdataAccessResult
ValidateUserdataAccess(const UserdataHeader &Header,
                       const UserdataAccessRequest &Request) noexcept;

// Validates one candidate block: the layout check runs before any other field
// of the block is read, so a foreign or undersized block is never interpreted
// as a Luna userdata.
[[nodiscard]] UserdataAccessResult
InspectUserdataAccess(const void *Block, std::size_t ByteCount,
                      const UserdataAccessRequest &Request) noexcept;

// Everything one access needs from the State the value came from. It is owned
// by `State::Impl`, so its address is stable across every move of its State,
// which is what lets one Luna-private virtual-machine slot name it. The class
// registry is read-only here: an access never registers anything.
struct UserdataAccessContext final {
  StateIdentity Origin;
  const ClassRegistry *Classes = nullptr;
  UserdataIdentityCache *Cache = nullptr;
  LifetimeHandleGenerationProbe HandleProbe = nullptr;

  // The lazy value cache of this State. Retiring one exposed value drops its
  // entries here before any payload is released, so a cached value can never
  // outlive the object it was produced from. It is optional: a State that never
  // declared a lazy member still validates every access exactly the same way.
  LazyPropertyCache *Lazy = nullptr;

  // What one member access reached through the virtual machine needs beyond the
  // gate itself: the immutable type generation it captures at entry, the fault
  // context that records the callback-stack restoration of a refused access,
  // the dispatch generation a cached value belongs to, and the recorder that
  // keeps the last dispatch observable. All four are optional, so an access
  // taken outside the virtual machine still validates identically.
  const TypeGenerationSource *Types = nullptr;
  FaultInjector *Faults = nullptr;
  MemberDispatchRecorder *Dispatch = nullptr;
  std::uint64_t DispatchGeneration = 0;

  [[nodiscard]] bool IsUsable() const noexcept {
    return Origin.IsValid() && Classes != nullptr && Cache != nullptr;
  }
};

} // namespace Luna::Detail
