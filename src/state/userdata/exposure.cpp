// clang-format off
#include "state/userdata/exposure.hpp"

#include "state/userdata/access.hpp"
#include "state/userdata/class_registry.hpp"
#include "state/userdata/collection.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/identity.hpp"
#include "state/userdata/identity_cache.hpp"
#include "state/userdata/lazy_cache.hpp"
#include "state/userdata/member_dispatch.hpp"
#include "state/userdata/operator_dispatch.hpp"
#include "state/type/structural_types.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/type_generation.hpp"
#include "state/vm/namespace_table.hpp"
#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr const char *AccessContextSlot = "Luna.UserdataAccessContext";
constexpr const char *IdentityCacheSlot = "Luna.UserdataIdentityCache";

static_assert(alignof(UserdataHeader) <= 8,
              "The userdata header must not require stronger alignment than "
              "the virtual machine guarantees for a userdata block.");

[[nodiscard]] int RaiseLiteral(lua_State *State, const char *Message) {
  lua_pushstring(State, Message);
  lua_error(State);
  return 0;
}

[[nodiscard]] UserdataHeader *MutableHeaderOf(void *Block,
                                              std::size_t ByteCount) noexcept {
  if (InspectUserdataHeader(Block, ByteCount) == nullptr)
    return nullptr;
  return static_cast<UserdataHeader *>(Block);
}

void PushIdentityCacheTable(lua_State *State) {
  lua_rawgetfield(State, LUA_REGISTRYINDEX, IdentityCacheSlot);
  if (lua_istable(State, -1))
    return;

  lua_pop(State, 1);
  lua_newtable(State);
  lua_newtable(State);
  lua_pushstring(State, "v");
  lua_rawsetfield(State, -2, "__mode");
  lua_setmetatable(State, -2);
  lua_pushvalue(State, -1);
  lua_rawsetfield(State, LUA_REGISTRYINDEX, IdentityCacheSlot);
}

[[nodiscard]] bool PushClassMetatable(lua_State *State,
                                      RegisteredClass &Registered) {
  if (Registered.Reference != LUA_REFNIL && Registered.Table != nullptr) {
    lua_getref(State, Registered.Reference);
    if (lua_istable(State, -1))
      return true;
    lua_pop(State, 1);
  }

  lua_newtable(State);
  lua_pushstring(State, Registered.QualifiedName.c_str());
  lua_rawsetfield(State, -2, "__metatable");
  lua_pushstring(State, Registered.QualifiedName.c_str());
  lua_rawsetfield(State, -2, "__type");

  const std::vector<std::string> Segments =
      SplitVmPath(Registered.QualifiedName);
  const int MetatableIndex = lua_gettop(State);
  if (lua_checkstack(State, 8) && PushVmPathContainer(State, Segments)) {
    PushVmPathField(State, Segments);
    if (lua_istable(State, -1)) {
      static_cast<void>(InstallClassMemberDispatch(
          State, MetatableIndex, lua_gettop(State), Registered.QualifiedName));

      static_cast<void>(InstallClassOperatorDispatch(
          State, MetatableIndex, lua_gettop(State), Registered.Operators));
      lua_pop(State, 2);
    } else {
      lua_pop(State, 2);
    }
  }

  Registered.Table = lua_topointer(State, -1);
  Registered.Reference = lua_ref(State, -1);
  if (Registered.Table == nullptr || Registered.Reference == LUA_REFNIL)
    return false;

  ++Registered.MetatableCreations;
  return true;
}

[[nodiscard]] bool PublishAtPath(lua_State *State,
                                 const std::vector<std::string> &Segments,
                                 int ValueIndex) {
  if (!PushVmPathContainer(State, Segments))
    return false;
  lua_pushvalue(State, ValueIndex);
  SetVmPathField(State, Segments);
  lua_pop(State, 1);
  return true;
}

struct ExposurePayload final {
  const UserdataExposureRequest *Request = nullptr;
  const std::vector<std::string> *Segments = nullptr;
  RegisteredClass *Registered = nullptr;
  const UserdataHeader *Prepared = nullptr;
  UserdataIdentityCache *Cache = nullptr;
  UserdataCacheEntry Recorded;
  UserdataExposureStatus Status = UserdataExposureStatus::ProtectedFailure;
  const void *Block = nullptr;
  bool ReuseRecorded = false;
};

[[nodiscard]] bool PushCachedValue(lua_State *State, const void *Address) {
  PushIdentityCacheTable(State);
  lua_pushlightuserdata(State, const_cast<void *>(Address));
  lua_rawget(State, -2);
  if (lua_type(State, -1) != LUA_TUSERDATA) {
    lua_pop(State, 2);
    return false;
  }
  lua_remove(State, -2);
  return true;
}

void StoreCachedValue(lua_State *State, const void *Address, int ValueIndex) {
  PushIdentityCacheTable(State);
  lua_pushlightuserdata(State, const_cast<void *>(Address));
  lua_pushvalue(State, ValueIndex);
  lua_rawset(State, -3);
  lua_pop(State, 1);
}

void DropCachedValue(lua_State *State, const void *Address) {
  PushIdentityCacheTable(State);
  lua_pushlightuserdata(State, const_cast<void *>(Address));
  lua_pushnil(State);
  lua_rawset(State, -3);
  lua_pop(State, 1);
}

[[nodiscard]] int ExposeValue(lua_State *State) {
  auto *Payload = static_cast<ExposurePayload *>(lua_tolightuserdata(State, 1));
  if (!Payload || !Payload->Request || !Payload->Segments ||
      !Payload->Registered || !Payload->Prepared || !Payload->Cache)
    return RaiseLiteral(State, "Internal error: invalid userdata exposure "
                               "request.");

  const void *Address = Payload->Request->Storage;

  if (Payload->ReuseRecorded) {
    if (PushCachedValue(State, Address)) {
      const int ValueIndex = lua_gettop(State);
      if (!PublishAtPath(State, *Payload->Segments, ValueIndex)) {
        Payload->Status = UserdataExposureStatus::PathUnavailable;
        return 0;
      }
      Payload->Block = lua_touserdata(State, ValueIndex);
      Payload->Status = UserdataExposureStatus::Reused;
      return 0;
    }
    static_cast<void>(Payload->Cache->Forget(Payload->Recorded.Identity));
  }

  void *Block =
      lua_newuserdatatagged(State, sizeof(UserdataHeader), TypedUserdataTag);
  if (!Block) {
    Payload->Status = UserdataExposureStatus::ProtectedFailure;
    return 0;
  }
  const int ValueIndex = lua_gettop(State);
  std::memcpy(Block, Payload->Prepared, sizeof(UserdataHeader));

  if (!PushClassMetatable(State, *Payload->Registered)) {
    Payload->Status = UserdataExposureStatus::MetatableUnavailable;
    return 0;
  }
  lua_setmetatable(State, ValueIndex);

  if (!PublishAtPath(State, *Payload->Segments, ValueIndex)) {
    Payload->Status = UserdataExposureStatus::PathUnavailable;
    return 0;
  }
  StoreCachedValue(State, Address, ValueIndex);

  UserdataCacheEntry Recorded = Payload->Recorded;
  Recorded.IsActive = true;
  if (!Payload->Cache->Record(Recorded)) {
    DropCachedValue(State, Address);
    Payload->Status = UserdataExposureStatus::ProtectedFailure;
    return 0;
  }

  UserdataHeader *Written = MutableHeaderOf(Block, sizeof(UserdataHeader));
  if (!Written) {
    static_cast<void>(Payload->Cache->Forget(Payload->Recorded.Identity));
    Payload->Status = UserdataExposureStatus::ProtectedFailure;
    return 0;
  }
  Written->Lifetime = LifetimeState::Published;
  Payload->Block = Block;
  Payload->Status = UserdataExposureStatus::Created;
  return 0;
}

struct RetirePayload final {
  NativeIdentity Identity;
  LazyPropertyCache *Lazy = nullptr;
  bool Invalidated = false;
};

[[nodiscard]] int RetireValue(lua_State *State) {
  auto *Payload = static_cast<RetirePayload *>(lua_tolightuserdata(State, 1));
  if (!Payload)
    return RaiseLiteral(State,
                        "Internal error: invalid userdata retire request.");

  if (PushCachedValue(State, Payload->Identity.Address)) {
    void *Block = lua_touserdata(State, -1);
    const std::size_t ByteCount =
        Block ? static_cast<std::size_t>(lua_objlen(State, -1)) : 0;
    if (UserdataHeader *Header = MutableHeaderOf(Block, ByteCount);
        Header != nullptr && Header->Identity == Payload->Identity) {
      if (Payload->Lazy != nullptr)
        static_cast<void>(Payload->Lazy->Drop(Payload->Identity, Header));
      if (Header->Lifetime == LifetimeState::Published ||
          Header->Lifetime == LifetimeState::Allocated ||
          Header->Lifetime == LifetimeState::Constructed)
        Header->Lifetime = LifetimeState::Invalid;
      Payload->Invalidated = true;
    }
    lua_pop(State, 1);
  }
  if (Payload->Invalidated)
    DropCachedValue(State, Payload->Identity.Address);
  return 0;
}

struct ObservePayload final {
  const std::vector<std::string> *Segments = nullptr;
  UserdataHeader *Observed = nullptr;
  bool Found = false;
};

struct VisitPayload final {
  const std::vector<std::string> *Segments = nullptr;
  const ExposedUserdataVisitor *Visit = nullptr;
  bool Found = false;
};

[[nodiscard]] int VisitHeader(lua_State *State) {
  auto *Payload = static_cast<VisitPayload *>(lua_tolightuserdata(State, 1));
  if (!Payload || !Payload->Segments || !Payload->Visit)
    return RaiseLiteral(State, "Internal error: invalid userdata visit "
                               "request.");

  if (!PushVmPathContainer(State, *Payload->Segments))
    return 0;
  PushVmPathField(State, *Payload->Segments);
  if (lua_type(State, -1) == LUA_TUSERDATA) {
    void *Block = lua_touserdata(State, -1);
    const std::size_t ByteCount =
        static_cast<std::size_t>(lua_objlen(State, -1));
    if (UserdataHeader *Header = MutableHeaderOf(Block, ByteCount)) {
      (*Payload->Visit)(*Header);
      Payload->Found = true;
    }
  }
  lua_pop(State, 2);
  return 0;
}

struct HandlePayload final {
  const std::vector<std::string> *Segments = nullptr;
  const TypeGeneration *Types = nullptr;
  const TypeDescriptor *Type = nullptr;
  StructuredReadResult *Read = nullptr;
  bool Attempted = false;
};

[[nodiscard]] int ReadHandle(lua_State *State) {
  auto *Payload = static_cast<HandlePayload *>(lua_tolightuserdata(State, 1));
  if (!Payload || !Payload->Segments || !Payload->Types || !Payload->Type ||
      !Payload->Read)
    return RaiseLiteral(State, "Internal error: invalid userdata handle read "
                               "request.");

  if (!PushVmPathContainer(State, *Payload->Segments))
    return 0;
  PushVmPathField(State, *Payload->Segments);

  *Payload->Read = ReadStructuredValue(*Payload->Types, State,
                                       lua_gettop(State), *Payload->Type);
  Payload->Attempted = true;
  lua_pop(State, 2);
  return 0;
}

[[nodiscard]] int ObserveHeader(lua_State *State) {
  auto *Payload = static_cast<ObservePayload *>(lua_tolightuserdata(State, 1));
  if (!Payload || !Payload->Segments || !Payload->Observed)
    return RaiseLiteral(
        State, "Internal error: invalid userdata observation request.");

  if (!PushVmPathContainer(State, *Payload->Segments))
    return 0;
  PushVmPathField(State, *Payload->Segments);
  if (lua_type(State, -1) == LUA_TUSERDATA) {
    const void *Block = lua_touserdata(State, -1);
    const std::size_t ByteCount =
        static_cast<std::size_t>(lua_objlen(State, -1));
    if (const UserdataHeader *Header =
            InspectUserdataHeader(Block, ByteCount)) {
      *Payload->Observed = *Header;
      Payload->Found = true;
    }
  }
  lua_pop(State, 2);
  return 0;
}

[[nodiscard]] bool CallProtected(lua_State *State, lua_CFunction Function,
                                 const char *Debug, void *Payload) {
  lua_pushcfunction(State, Function, Debug);
  lua_pushlightuserdata(State, Payload);
  return lua_pcall(State, 1, 0, 0) == LUA_OK;
}

[[nodiscard]] UserdataExposureStatus
StatusFor(UserdataCacheDecision Decision) noexcept {
  switch (Decision) {
  case UserdataCacheDecision::Create:
    return UserdataExposureStatus::Created;
  case UserdataCacheDecision::Reuse:
    return UserdataExposureStatus::Reused;
  case UserdataCacheDecision::ConflictingOwnership:
    return UserdataExposureStatus::ConflictingOwnership;
  case UserdataCacheDecision::IncompatibleType:
    return UserdataExposureStatus::IncompatibleType;
  case UserdataCacheDecision::IncompatibleAccess:
    return UserdataExposureStatus::IncompatibleAccess;
  case UserdataCacheDecision::UnavailableRequest:
    return UserdataExposureStatus::UnavailableRequest;
  }
  return UserdataExposureStatus::UnavailableRequest;
}

} // namespace

std::string_view
UserdataExposureStatusText(UserdataExposureStatus Status) noexcept {
  switch (Status) {
  case UserdataExposureStatus::Created:
    return "created";
  case UserdataExposureStatus::Reused:
    return "reused";
  case UserdataExposureStatus::UnavailableRequest:
    return "unavailable_request";
  case UserdataExposureStatus::NullStorage:
    return "null_storage";
  case UserdataExposureStatus::MissingLifetimeHandle:
    return "missing_lifetime_handle";
  case UserdataExposureStatus::ConflictingOwnership:
    return "conflicting_ownership";
  case UserdataExposureStatus::IncompatibleType:
    return "incompatible_type";
  case UserdataExposureStatus::IncompatibleAccess:
    return "incompatible_access";
  case UserdataExposureStatus::MetatableUnavailable:
    return "metatable_unavailable";
  case UserdataExposureStatus::PathUnavailable:
    return "path_unavailable";
  case UserdataExposureStatus::StackCapacityFailure:
    return "stack_capacity_failure";
  case UserdataExposureStatus::ProtectedFailure:
    return "protected_failure";
  }
  return "protected_failure";
}

UserdataExposure
ExposeUserdataValue(lua_State *State, UserdataAccessContext &Context,
                    RegisteredClass &Registered, NativeIdentitySource &Nonces,
                    const UserdataExposureRequest &Request) noexcept {
  UserdataExposure Exposure;
  if (!State || !Context.IsUsable() || !Registered.IsComplete() ||
      Request.Path.empty() ||
      !ClassRegistry::Matches(Registered, Context.Origin, Registered.Type,
                              Registered.ClassSymbol)) {
    Exposure.Status = UserdataExposureStatus::UnavailableRequest;
    return Exposure;
  }
  if (Request.Storage == nullptr) {
    Exposure.Status = UserdataExposureStatus::NullStorage;
    return Exposure;
  }
  if (Request.Ownership == OwnershipModel::Borrowed &&
      !Request.Handle.IsDeclared()) {
    Exposure.Status = UserdataExposureStatus::MissingLifetimeHandle;
    return Exposure;
  }

  UserdataCacheRequest Wanted;
  Wanted.Origin = Context.Origin;
  Wanted.Address = Request.Storage;
  Wanted.DynamicType = Registered.Type;
  Wanted.DeclaredViewType = Registered.Type;
  Wanted.ClassSymbol = Registered.ClassSymbol;
  Wanted.Metatable = Registered.Metatable;
  Wanted.Ownership = Request.Ownership;
  Wanted.Access = Request.Access;
  Wanted.DispatchGeneration = Request.DispatchGeneration;

  const UserdataCacheLookup Decided = Context.Cache->Evaluate(Wanted);
  if (!Decided.PermitsCreation() && !Decided.PermitsReuse()) {
    Exposure.Status = StatusFor(Decided.Decision);
    return Exposure;
  }

  if (!PublishUserdataAccessContext(State, &Context)) {
    Exposure.Status = UserdataExposureStatus::ProtectedFailure;
    return Exposure;
  }

  UserdataCacheEntry Recorded;
  Recorded.Origin = Wanted.Origin;
  Recorded.Identity.Address = Request.Storage;
  Recorded.Identity.Nonce = Decided.PermitsReuse() && Decided.Entry.has_value()
                                ? Decided.Entry->Identity.Nonce
                                : Nonces.Next();
  Recorded.DynamicType = Wanted.DynamicType;
  Recorded.DeclaredViewType = Wanted.DeclaredViewType;
  Recorded.ClassSymbol = Wanted.ClassSymbol;
  Recorded.Metatable = Wanted.Metatable;
  Recorded.Ownership = Wanted.Ownership;
  Recorded.Access = Wanted.Access;
  Recorded.DispatchGeneration = Wanted.DispatchGeneration;

  UserdataHeaderRequest HeaderRequest;
  HeaderRequest.Origin = Context.Origin;
  HeaderRequest.DynamicType = Wanted.DynamicType;
  HeaderRequest.DeclaredViewType = Wanted.DeclaredViewType;
  HeaderRequest.ClassSymbol = Wanted.ClassSymbol;
  HeaderRequest.Metatable = Wanted.Metatable;
  HeaderRequest.Ownership = Wanted.Ownership;
  HeaderRequest.Access = Wanted.Access;
  HeaderRequest.DispatchGeneration = Request.DispatchGeneration;

  UserdataHeader Prepared = MakeUserdataHeader(HeaderRequest);
  Prepared.Lifetime = LifetimeState::Constructed;
  Prepared.Identity = Recorded.Identity;
  Prepared.Payload.Storage = Request.Storage;
  Prepared.Payload.SharedOwnership = Request.SharedOwnership;
  Prepared.Handle = Request.Handle;
  Prepared.Allocator = Request.Allocator;

  const std::vector<std::string> Segments = SplitVmPath(Request.Path);
  if (Segments.empty()) {
    Exposure.Status = UserdataExposureStatus::PathUnavailable;
    return Exposure;
  }

  StackCheckpoint Checkpoint(State);
  if (!lua_checkstack(State, static_cast<int>(Segments.size()) + 12)) {
    Exposure.Status = UserdataExposureStatus::StackCapacityFailure;
    return Exposure;
  }

  ExposurePayload Payload;
  Payload.Request = &Request;
  Payload.Segments = &Segments;
  Payload.Registered = &Registered;
  Payload.Prepared = &Prepared;
  Payload.Cache = Context.Cache;
  Payload.Recorded = Recorded;
  Payload.ReuseRecorded = Decided.PermitsReuse();
  if (!CallProtected(State, ExposeValue, "Luna.ExposeUserdataValue",
                     &Payload)) {
    Exposure.Status = UserdataExposureStatus::ProtectedFailure;
    return Exposure;
  }

  Exposure.Status = Payload.Status;
  Exposure.Block = Payload.Block;
  if (Exposure.IsSuccess())
    Exposure.Identity = Recorded.Identity;
  return Exposure;
}

bool RetireExposedUserdata(lua_State *State, UserdataAccessContext &Context,
                           const NativeIdentity &Identity) noexcept {
  if (!State || !Context.Cache || !Identity.IsValid())
    return false;

  const bool Inactivated = Context.Cache->Inactivate(Identity);

  StackCheckpoint Checkpoint(State);
  if (!lua_checkstack(State, 12))
    return false;

  RetirePayload Payload;
  Payload.Identity = Identity;
  Payload.Lazy = Context.Lazy;
  if (!CallProtected(State, RetireValue, "Luna.RetireExposedUserdata",
                     &Payload))
    return false;

  if (Context.Lazy != nullptr)
    static_cast<void>(Context.Lazy->Drop(Identity, nullptr));
  const bool Forgotten = Context.Cache->Forget(Identity);
  return Inactivated || Forgotten || Payload.Invalidated;
}

bool PushRegisteredClassMetatable(lua_State *State,
                                  RegisteredClass &Registered) {
  if (!State)
    return false;
  return PushClassMetatable(State, Registered);
}

bool PushCachedUserdataValue(lua_State *State, const void *Address) {
  if (!State || Address == nullptr)
    return false;
  return PushCachedValue(State, Address);
}

void StoreCachedUserdataValue(lua_State *State, const void *Address,
                              int ValueIndex) {
  if (!State || Address == nullptr)
    return;
  StoreCachedValue(State, Address, ValueIndex);
}

void DropCachedUserdataValue(lua_State *State, const void *Address) {
  if (!State || Address == nullptr)
    return;
  DropCachedValue(State, Address);
}

bool PublishUserdataAccessContext(lua_State *State,
                                  UserdataAccessContext *Context) noexcept {
  if (!State || !Context || !Context->IsUsable())
    return false;
  if (!lua_checkstack(State, 4))
    return false;

  StackCheckpoint Checkpoint(State);
  lua_pushlightuserdata(State, Context);
  lua_rawsetfield(State, LUA_REGISTRYINDEX, AccessContextSlot);
  return true;
}

const UserdataAccessContext *
ObserveUserdataAccessContext(lua_State *State) noexcept {
  if (!State || !lua_checkstack(State, 2))
    return nullptr;

  StackCheckpoint Checkpoint(State);
  lua_rawgetfield(State, LUA_REGISTRYINDEX, AccessContextSlot);
  const auto *Context = static_cast<const UserdataAccessContext *>(
      lua_tolightuserdata(State, -1));
  if (Context == nullptr || !Context->IsUsable())
    return nullptr;
  return Context;
}

bool ObserveExposedUserdataHeader(lua_State *State, const std::string &Path,
                                  UserdataHeader &Observed) {
  if (!State || Path.empty())
    return false;

  const std::vector<std::string> Segments = SplitVmPath(Path);
  if (Segments.empty())
    return false;

  StackCheckpoint Checkpoint(State);
  if (!lua_checkstack(State, static_cast<int>(Segments.size()) + 8))
    return false;

  ObservePayload Payload;
  Payload.Segments = &Segments;
  Payload.Observed = &Observed;
  if (!CallProtected(State, ObserveHeader, "Luna.ObserveExposedUserdata",
                     &Payload))
    return false;
  return Payload.Found;
}

bool VisitExposedUserdataHeader(lua_State *State, const std::string &Path,
                                const ExposedUserdataVisitor &Visit) {
  if (!State || Path.empty() || !Visit)
    return false;

  const std::vector<std::string> Segments = SplitVmPath(Path);
  if (Segments.empty())
    return false;

  StackCheckpoint Checkpoint(State);
  if (!lua_checkstack(State, static_cast<int>(Segments.size()) + 8))
    return false;

  VisitPayload Payload;
  Payload.Segments = &Segments;
  Payload.Visit = &Visit;
  if (!CallProtected(State, VisitHeader, "Luna.VisitExposedUserdata", &Payload))
    return false;
  return Payload.Found;
}

UserdataHandleObservation ReadExposedUserdataHandle(lua_State *State,
                                                    const TypeGeneration &Types,
                                                    const StableTypeKey &Key,
                                                    const std::string &Path) {
  UserdataHandleObservation Observed;
  Observed.Failure = "internal_failure";
  if (!State || Path.empty())
    return Observed;

  const std::vector<std::string> Segments = SplitVmPath(Path);
  if (Segments.empty())
    return Observed;

  const TypeDescriptor Type = TypeDescriptor::ForClass(Key);
  StructuredReadResult Read;
  StackCheckpoint Checkpoint(State);
  if (!lua_checkstack(State, static_cast<int>(Segments.size()) + 12))
    return Observed;

  HandlePayload Payload;
  Payload.Segments = &Segments;
  Payload.Types = &Types;
  Payload.Type = &Type;
  Payload.Read = &Read;
  if (!CallProtected(State, ReadHandle, "Luna.ReadExposedUserdataHandle",
                     &Payload) ||
      !Payload.Attempted)
    return Observed;

  if (Read.IsSuccess()) {
    Observed.ReachedNativeCode = Read.ConvertedValue.HandleStorage() != nullptr;
    Observed.Storage = Read.ConvertedValue.HandleStorage();
    Observed.PermitsMutation = Read.ConvertedValue.HandlePermitsMutation();
    Observed.Failure =
        std::string(StructuredFailureText(StructuredFailure::None));
    return Observed;
  }

  ConversionSubject Subject;
  Subject.Kind = ConversionSubjectKind::Callable;
  Subject.Name = "userdata access";
  Observed.Failure =
      std::string(StructuredFailureText(Read.Diagnostic.Failure));
  Observed.Diagnostic = DescribeConversionFailure(
      Subject, ConversionDirection::Argument, 1, Read.Diagnostic);
  return Observed;
}

} // namespace Luna::Detail
