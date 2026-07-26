#pragma once

// Publishing one staged construction result as exactly one class value.
//
// A constructor, a factory, or a singleton accessor produces a native object
// plus the ownership statement it will be owned under. Nothing about that
// object is visible yet: this is the one place where the staged result becomes
// a value, and it becomes one only if every step succeeds - the captured type
// generation describes the class, the identity cache admits the object, the
// release gate stages, constructs, and establishes its ownership, the
// virtual-machine block and its class metatable exist, the cache entry exists,
// and the protected publication of the return value completes. Any failure
// publishes nothing.
//
// The publication runs through exactly the canonical class conversion the type
// registry already owns, so a constructed value is allocated, constructed,
// cached, owned, and released by the same paths a returned object is. An object
// Luna creates arrives with no storage at all: the semantic allocator protocol
// in the staged result produces it and the staged construction step builds into
// it, which is what lets one gate own every milestone and perform exactly the
// cleanup the completed milestones warrant.

// clang-format off
#include <luna/binding/class_construction.hpp>
#include <luna/type/stable_type_key.hpp>

#include <string>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class TypeGeneration;

// Why one staged construction result did or did not become a value.
enum class InstancePublicationStatus {
  // Exactly one value was published on top of the stack.
  Published,

  // The candidate produced no object at all.
  MissingObject,

  // The captured generation does not describe the class in the writing
  // direction, so nothing could have been published through it.
  UnavailableClass,

  // The State has no exposure context, which means no class was ever registered
  // in it: Luna's own inconsistency.
  UnavailableContext,

  // The conversion write path refused the exposure. `Diagnostic` carries its
  // one
  // deterministic message.
  RefusedExposure
};

struct InstancePublication final {
  InstancePublicationStatus Status = InstancePublicationStatus::MissingObject;
  int PublishedCount = 0;

  // The object the published value carries. A candidate that asked Luna to
  // create the object never knew its address, so this is the only thing that
  // names it afterwards - and naming it is what lets a later failure release
  // exactly the value this publication produced.
  void *Storage = nullptr;

  // Whether this publication established one new owner. A value the identity
  // cache handed back was owned before this call and is not this publication's
  // to release.
  bool EstablishedOwner = false;

  // The deterministic message of a refusal, empty on success.
  std::string Diagnostic;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Status == InstancePublicationStatus::Published;
  }
};

// Publishes `Produced` as exactly one value of the registered class `Class`,
// left on top of the stack. On refusal nothing is published, the object is
// destroyed and its Luna-owned storage released exactly once, and no ownership
// record, cache entry, or second owner survives.
[[nodiscard]] InstancePublication
PublishConstructedInstance(lua_State *State, const TypeGeneration &Types,
                           const StableTypeKey &Class,
                           const ConstructedInstance &Produced) noexcept;

// Releases one already published construction result through the one idempotent
// release gate, because the publication that carried it did not complete after
// all. Calling it for an object the gate no longer owns changes nothing.
[[nodiscard]] bool ReleasePublishedInstance(lua_State *State,
                                            void *Storage) noexcept;

} // namespace Luna::Detail
