#pragma once

// clang-format off
#include <luna/binding/class_allocator.hpp>
#include <luna/binding/lifetime_handle.hpp>

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna {

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

class OwnershipPolicy final {
public:
  OwnershipPolicy() = default;

  [[nodiscard]] static OwnershipPolicy Borrowed(LifetimeHandle Lifetime) {
    OwnershipPolicy Policy;
    Policy.OwnershipValue = ConstructionOwnership::Borrowed;
    Policy.LifetimeValue = std::move(Lifetime);
    return Policy;
  }

  [[nodiscard]] static OwnershipPolicy LuaOwned() {
    OwnershipPolicy Policy;
    Policy.OwnershipValue = ConstructionOwnership::LuaOwned;
    Policy.LifetimeValue = LifetimeHandle::Undeclared();
    return Policy;
  }

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

inline constexpr std::string_view DefaultConstructorName = "New";

inline constexpr std::string_view ConstructedStoragePolicyName =
    "Luna.ConstructedStorage";

inline constexpr std::string_view AdoptedStoragePolicyName =
    "Luna.AdoptedStorage";

struct ConstructedInstance final {
  void *Storage = nullptr;

  ConstructionOwnership Ownership = ConstructionOwnership::LuaOwned;
  bool PermitsMutation = true;

  ClassAllocator Allocator;

  ClassAllocator::ConstructOperation Construct;

  LifetimeHandle Lifetime = LifetimeHandle::Undeclared();

  std::shared_ptr<void> SharedOwnership;
};

[[nodiscard]] inline ConstructedInstance
CreatedClassInstance(ClassAllocator Storage,
                     ClassAllocator::ConstructOperation Build) {
  ConstructedInstance Produced;
  Produced.Ownership = ConstructionOwnership::LuaOwned;
  Produced.Allocator = std::move(Storage);
  Produced.Construct = std::move(Build);
  return Produced;
}

[[nodiscard]] inline ConstructedInstance
BorrowedClassInstance(void *Storage, LifetimeHandle Lifetime) {
  ConstructedInstance Produced;
  Produced.Storage = Storage;
  Produced.Ownership = ConstructionOwnership::Borrowed;
  Produced.Lifetime = std::move(Lifetime);
  return Produced;
}

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
