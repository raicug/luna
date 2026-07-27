#pragma once

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

enum class UserdataAccessFailure : std::uint8_t {
  None,

  UnavailableRequest,

  MissingValue,

  ForeignLayout,

  ForeignState,

  MetatableMismatch,

  NullPayload,

  MissingLifetimeHandle,

  ExpiredLifetimeHandle,

  Unpublished,

  Invalidated,

  Destroyed,

  Released,

  TypeMismatch,

  IncompatibleObject,

  ConstViolation
};

[[nodiscard]] std::string_view
UserdataAccessFailureText(UserdataAccessFailure Failure) noexcept;

using LifetimeHandleGenerationProbe =
    std::uint64_t (*)(const void *Record) noexcept;

struct UserdataAccessRequest final {
  StateIdentity Origin;

  MetatableId Metatable;

  TypeId RequestedType;

  bool RequiresMutation = false;

  LifetimeHandleGenerationProbe HandleProbe = nullptr;

  const ClassRelationships *Relationships = nullptr;

  [[nodiscard]] bool IsComplete() const noexcept {
    return Origin.IsValid() && Metatable.IsValid() && RequestedType.IsValid();
  }
};

struct UserdataAccessResult final {
  UserdataAccessFailure Failure = UserdataAccessFailure::UnavailableRequest;
  const UserdataHeader *Header = nullptr;

  void *Storage = nullptr;
  ClassConversionKind Conversion = ClassConversionKind::Identity;
  bool PermitsMutation = false;

  [[nodiscard]] bool IsPermitted() const noexcept {
    return Failure == UserdataAccessFailure::None && Storage != nullptr;
  }
};

[[nodiscard]] UserdataAccessResult
ValidateUserdataAccess(const UserdataHeader &Header,
                       const UserdataAccessRequest &Request) noexcept;

[[nodiscard]] UserdataAccessResult
InspectUserdataAccess(const void *Block, std::size_t ByteCount,
                      const UserdataAccessRequest &Request) noexcept;

struct UserdataAccessContext final {
  StateIdentity Origin;
  const ClassRegistry *Classes = nullptr;
  UserdataIdentityCache *Cache = nullptr;
  LifetimeHandleGenerationProbe HandleProbe = nullptr;

  LazyPropertyCache *Lazy = nullptr;

  const TypeGenerationSource *Types = nullptr;
  FaultInjector *Faults = nullptr;
  MemberDispatchRecorder *Dispatch = nullptr;
  std::uint64_t DispatchGeneration = 0;

  [[nodiscard]] bool IsUsable() const noexcept {
    return Origin.IsValid() && Classes != nullptr && Cache != nullptr;
  }
};

} // namespace Luna::Detail
