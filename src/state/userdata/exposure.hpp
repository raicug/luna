#pragma once

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

enum class UserdataExposureStatus : std::uint8_t {
  Created,

  Reused,

  UnavailableRequest,

  NullStorage,

  MissingLifetimeHandle,

  ConflictingOwnership,
  IncompatibleType,
  IncompatibleAccess,

  MetatableUnavailable,

  PathUnavailable,

  StackCapacityFailure,
  ProtectedFailure
};

[[nodiscard]] std::string_view
UserdataExposureStatusText(UserdataExposureStatus Status) noexcept;

struct UserdataExposureRequest final {
  void *Storage = nullptr;

  OwnershipModel Ownership = OwnershipModel::Borrowed;
  ConstAccess Access = ConstAccess::Mutable;

  LifetimeHandleReference Handle;
  AllocatorRecordReference Allocator;
  std::uint64_t DispatchGeneration = 0;

  std::string Path;

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

[[nodiscard]] UserdataExposure
ExposeUserdataValue(lua_State *State, UserdataAccessContext &Context,
                    RegisteredClass &Registered, NativeIdentitySource &Nonces,
                    const UserdataExposureRequest &Request) noexcept;

[[nodiscard]] bool
RetireExposedUserdata(lua_State *State, UserdataAccessContext &Context,
                      const NativeIdentity &Identity) noexcept;

[[nodiscard]] bool PushRegisteredClassMetatable(lua_State *State,
                                                RegisteredClass &Registered);

[[nodiscard]] bool PushCachedUserdataValue(lua_State *State,
                                           const void *Address);
void StoreCachedUserdataValue(lua_State *State, const void *Address,
                              int ValueIndex);
void DropCachedUserdataValue(lua_State *State, const void *Address);

[[nodiscard]] bool
PublishUserdataAccessContext(lua_State *State,
                             UserdataAccessContext *Context) noexcept;

[[nodiscard]] const UserdataAccessContext *
ObserveUserdataAccessContext(lua_State *State) noexcept;

[[nodiscard]] bool ObserveExposedUserdataHeader(lua_State *State,
                                                const std::string &Path,
                                                UserdataHeader &Observed);

using ExposedUserdataVisitor = std::function<void(UserdataHeader &)>;

[[nodiscard]] bool
VisitExposedUserdataHeader(lua_State *State, const std::string &Path,
                           const ExposedUserdataVisitor &Visit);

struct UserdataHandleObservation final {
  bool ReachedNativeCode = false;
  void *Storage = nullptr;
  bool PermitsMutation = false;

  std::string Failure;
  std::string Diagnostic;
};

[[nodiscard]] UserdataHandleObservation
ReadExposedUserdataHandle(lua_State *State, const TypeGeneration &Types,
                          const StableTypeKey &Key, const std::string &Path);

} // namespace Luna::Detail
