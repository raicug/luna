#pragma once

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

struct ClassExposureIntent final {
  void *Storage = nullptr;
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  ConstAccess Access = ConstAccess::Mutable;

  LifetimeHandle Handle = LifetimeHandle::Undeclared();

  std::shared_ptr<void> SharedOwnership;

  ClassAllocator Allocator;

  ObjectConstruction Construct;
};

struct UserdataExposureContext final {
  StateIdentity Origin;

  ClassRegistry *Classes = nullptr;
  UserdataIdentityCache *Cache = nullptr;
  NativeIdentitySource *Nonces = nullptr;
  OwnershipRegistry *Ownership = nullptr;

  UserdataAccessContext *Access = nullptr;

  std::uint64_t DispatchGeneration = 0;

  [[nodiscard]] bool IsUsable() const noexcept {
    return Origin.IsValid() && Classes != nullptr && Cache != nullptr &&
           Nonces != nullptr && Ownership != nullptr && Access != nullptr;
  }
};

enum class ClassExposureStatus : std::uint8_t {
  Created,

  Reused,

  UnavailableRequest,

  NullStorage,

  StorageUnavailable,

  ConflictingOwnership,
  IncompatibleType,
  IncompatibleAccess,

  UnestablishedOwnership,

  MetatableUnavailable,

  StackCapacityFailure,
  ProtectedFailure
};

[[nodiscard]] std::string_view
ClassExposureStatusText(ClassExposureStatus Status) noexcept;

struct ClassExposureResult final {
  ClassExposureStatus Status = ClassExposureStatus::UnavailableRequest;

  OwnershipFailure Ownership = OwnershipFailure::None;

  NativeIdentity Identity;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Status == ClassExposureStatus::Created ||
           Status == ClassExposureStatus::Reused;
  }
};

[[nodiscard]] bool
DeclaresObjectConstruction(const ClassExposureIntent &Intent) noexcept;

[[nodiscard]] ClassExposureResult
PushExposedClassValue(lua_State *State, UserdataExposureContext &Context,
                      RegisteredClass &Registered,
                      const ClassExposureIntent &Intent);

[[nodiscard]] bool
PublishUserdataExposureContext(lua_State *State,
                               UserdataExposureContext *Context) noexcept;

[[nodiscard]] UserdataExposureContext *
ObserveUserdataExposureContext(lua_State *State) noexcept;

struct ClassValueWriteObservation final {
  bool Published = false;
  int PublishedCount = 0;

  std::string Failure;
  std::string Diagnostic;

  int EntryStackDepth = 0;
  int FinalStackDepth = 0;
};

[[nodiscard]] ClassValueWriteObservation
WriteExposedClassValue(lua_State *State, const TypeGeneration &Types,
                       const StableTypeKey &Key, const std::string &Path,
                       std::shared_ptr<const ClassExposureIntent> Intent);

} // namespace Luna::Detail
