#pragma once

// The result of one constructed, produced, or accessed native object.
//
// A constructor, a factory, or a singleton accessor does not return a value: it
// returns an object, plus the statement of how that object will be owned. Luna
// never guesses that statement. An object Luna creates arrives with the
// semantic storage protocol that allocates and releases it and with the one
// construction step that builds it from the converted arguments; a shared
// object arrives with exactly one `std::shared_ptr` ownership reference; a
// borrowed object arrives with the explicit `LifetimeHandle` its owner holds.
// `OwnershipPolicy` is how a consumer states that explicitly when the declared
// result alone would not decide it.
//
// Nothing here is published. The staged result becomes a value only after
// allocation, native construction, ownership establishment, identity-cache
// insertion, metatable association, and protected return publication have all
// succeeded, and any failure performs exactly the cleanup the completed
// milestones warrant.
//
// Nothing here is a virtual-machine value, a stack index, or a Luau type: an
// object plus an ownership statement is all a construction result ever is.

// clang-format off
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/lifetime_handle.hpp>

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna {

// How Luna will own the object one construction candidate produced.
enum class ConstructionOwnership { LuaOwned, Borrowed, Shared };

[[nodiscard]] constexpr std::string_view
ConstructionOwnershipText(ConstructionOwnership Ownership) noexcept {
  switch (Ownership) {
  case ConstructionOwnership::LuaOwned:
    return "lua-owned";
  case ConstructionOwnership::Borrowed:
    return "borrowed";
  case ConstructionOwnership::Shared:
    return "shared";
  }
  return "lua-owned";
}

// One explicit ownership policy for a construction candidate whose declared
// result does not decide its ownership on its own.
//
// A default-constructed policy is the singleton default: borrowed, with one
// Luna-owned lifetime that stays live for as long as the registration that
// declared it. Supplying a different model is always explicit, and a policy
// that contradicts itself - a borrowed result with no declared lifetime, or a
// non-borrowed result that declares one - is rejected transactionally rather
// than reinterpreted.
class OwnershipPolicy final {
public:
  OwnershipPolicy() = default;

  // Borrowed: Luna never releases the object, and the explicit lifetime decides
  // how long every value exposed from it may be accessed.
  [[nodiscard]] static OwnershipPolicy Borrowed(LifetimeHandle Lifetime) {
    OwnershipPolicy Policy;
    Policy.OwnershipValue = ConstructionOwnership::Borrowed;
    Policy.LifetimeValue = std::move(Lifetime);
    return Policy;
  }

  // Lua-owned: Luna destroys the object exactly once and releases the storage
  // it allocated for it.
  [[nodiscard]] static OwnershipPolicy LuaOwned() {
    OwnershipPolicy Policy;
    Policy.OwnershipValue = ConstructionOwnership::LuaOwned;
    Policy.LifetimeValue = LifetimeHandle::Undeclared();
    return Policy;
  }

  // Shared: Luna holds and releases exactly one corresponding shared ownership
  // reference per stored object.
  [[nodiscard]] static OwnershipPolicy Shared() {
    OwnershipPolicy Policy;
    Policy.OwnershipValue = ConstructionOwnership::Shared;
    Policy.LifetimeValue = LifetimeHandle::Undeclared();
    return Policy;
  }

  [[nodiscard]] ConstructionOwnership Ownership() const noexcept {
    return OwnershipValue;
  }

  [[nodiscard]] const LifetimeHandle &Lifetime() const noexcept {
    return LifetimeValue;
  }

  // A borrowed policy states one declared lifetime; no other model may state
  // one at all.
  [[nodiscard]] bool IsCoherent() const noexcept {
    if (OwnershipValue == ConstructionOwnership::Borrowed)
      return LifetimeValue.IsDeclared();
    return !LifetimeValue.IsDeclared();
  }

private:
  ConstructionOwnership OwnershipValue = ConstructionOwnership::Borrowed;
  LifetimeHandle LifetimeValue;
};

namespace Detail {

// Luna's default name for one constructor of a class. Every constructor
// declared without an explicit name shares it, so the constructors of one class
// form one canonical overload set instead of one symbol each.
inline constexpr std::string_view DefaultConstructorName = "New";

// Canonical identity of the allocator policy behind one object Luna creates
// itself.
inline constexpr std::string_view ConstructedStoragePolicyName =
    "Luna.ConstructedStorage";

// Canonical identity of the policy behind an object Luna neither allocates nor
// releases: it already exists, and its owner decides when it ends.
inline constexpr std::string_view AdoptedStoragePolicyName =
    "Luna.AdoptedStorage";

// One object a construction candidate produced, staged and not yet exposed.
struct ConstructedInstance final {
  // The object, when it already exists. A candidate that asks Luna to create
  // the object leaves this null and declares the storage protocol and the
  // construction step instead, so allocation, construction, ownership, and
  // publication are all milestones of one gate.
  void *Storage = nullptr;

  ConstructionOwnership Ownership = ConstructionOwnership::LuaOwned;
  bool PermitsMutation = true;

  // The semantic storage protocol of this object. An undeclared protocol states
  // that Luna neither allocates nor releases the storage.
  ClassAllocator Allocator;

  // The one construction step of an object Luna creates: it builds the object
  // from the arguments the call already converted. An object that already
  // exists declares none.
  ClassAllocator::ConstructOperation Construct;

  // The explicit lifetime of a borrowed object.
  LifetimeHandle Lifetime = LifetimeHandle::Undeclared();

  // The one shared ownership reference of a shared object.
  std::shared_ptr<void> SharedOwnership;
};

// One object Luna creates: `Storage` allocates and releases it, and `Build`
// constructs it. Luna owns the result and destroys it exactly once.
[[nodiscard]] inline ConstructedInstance
CreatedClassInstance(ClassAllocator Storage,
                     ClassAllocator::ConstructOperation Build) {
  ConstructedInstance Produced;
  Produced.Ownership = ConstructionOwnership::LuaOwned;
  Produced.Allocator = std::move(Storage);
  Produced.Construct = std::move(Build);
  return Produced;
}

// One already living object Luna only borrows. Luna releases neither the object
// nor its storage; the explicit lifetime is the only thing that ends it.
[[nodiscard]] inline ConstructedInstance
BorrowedClassInstance(void *Storage, LifetimeHandle Lifetime) {
  ConstructedInstance Produced;
  Produced.Storage = Storage;
  Produced.Ownership = ConstructionOwnership::Borrowed;
  Produced.Lifetime = std::move(Lifetime);
  return Produced;
}

// One object owned through exactly one shared ownership reference Luna retains
// and releases exactly once.
[[nodiscard]] inline ConstructedInstance
SharedClassInstance(void *Storage, std::shared_ptr<void> Ownership) {
  ConstructedInstance Produced;
  Produced.Storage = Storage;
  Produced.Ownership = ConstructionOwnership::Shared;
  Produced.SharedOwnership = std::move(Ownership);
  return Produced;
}

} // namespace Detail

} // namespace Luna
