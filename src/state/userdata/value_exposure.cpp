// clang-format off
#include "state/userdata/value_exposure.hpp"

#include <luna/binding/lifetime_handle.hpp>

#include "state/userdata/access.hpp"
#include "state/userdata/allocator.hpp"
#include "state/userdata/class_registry.hpp"
#include "state/userdata/collection.hpp"
#include "state/userdata/exposure.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/identity.hpp"
#include "state/userdata/identity_cache.hpp"
#include "state/userdata/ownership.hpp"
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
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr const char *ExposureContextSlot = "Luna.UserdataExposureContext";

[[nodiscard]] ClassExposureResult
Refuse(ClassExposureStatus Status,
       OwnershipFailure Failure = OwnershipFailure::None) {
  ClassExposureResult Result;
  Result.Status = Status;
  Result.Ownership = Failure;
  return Result;
}

[[nodiscard]] ClassExposureStatus
StatusFor(UserdataCacheDecision Decision) noexcept {
  switch (Decision) {
  case UserdataCacheDecision::Create:
    return ClassExposureStatus::Created;
  case UserdataCacheDecision::Reuse:
    return ClassExposureStatus::Reused;
  case UserdataCacheDecision::ConflictingOwnership:
    return ClassExposureStatus::ConflictingOwnership;
  case UserdataCacheDecision::IncompatibleType:
    return ClassExposureStatus::IncompatibleType;
  case UserdataCacheDecision::IncompatibleAccess:
    return ClassExposureStatus::IncompatibleAccess;
  case UserdataCacheDecision::UnavailableRequest:
    return ClassExposureStatus::UnavailableRequest;
  }
  return ClassExposureStatus::UnavailableRequest;
}

[[nodiscard]] UserdataHeader *WrittenHeaderOf(void *Block,
                                              std::size_t ByteCount) noexcept {
  if (InspectUserdataHeader(Block, ByteCount) == nullptr)
    return nullptr;
  return static_cast<UserdataHeader *>(Block);
}

[[nodiscard]] OwnershipRequest RequestFrom(const ClassExposureIntent &Intent) {
  OwnershipRequest Request;
  if (Intent.Ownership == OwnershipModel::Borrowed)
    Request.Handle = Intent.Handle;
  if (Intent.Ownership == OwnershipModel::Shared)
    Request.SharedOwnership = Intent.SharedOwnership;
  return Request;
}

struct WritePayload final {
  const std::vector<std::string> *Segments = nullptr;
  const TypeGeneration *Types = nullptr;
  const TypeDescriptor *Type = nullptr;
  const StructuredValue *Staged = nullptr;
  StructuredWriteResult *Written = nullptr;
  bool Attempted = false;
  bool Stored = false;
};

[[nodiscard]] int WriteValue(lua_State *State) {
  auto *Payload = static_cast<WritePayload *>(lua_tolightuserdata(State, 1));
  if (!Payload || !Payload->Segments || !Payload->Types || !Payload->Type ||
      !Payload->Staged || !Payload->Written) {
    lua_pushstring(State,
                   "Internal error: invalid userdata exposure write request.");
    lua_error(State);
    return 0;
  }

  *Payload->Written = WriteStructuredValue(*Payload->Types, State,
                                           *Payload->Type, *Payload->Staged);
  Payload->Attempted = true;
  if (!Payload->Written->IsSuccess() || Payload->Written->PublishedCount != 1)
    return 0;

  const int ValueIndex = lua_gettop(State);
  if (!PushVmPathContainer(State, *Payload->Segments))
    return 0;
  lua_pushvalue(State, ValueIndex);
  SetVmPathField(State, *Payload->Segments);
  lua_pop(State, 1);
  Payload->Stored = true;
  return 0;
}

[[nodiscard]] bool CallProtectedWrite(lua_State *State, lua_CFunction Function,
                                      void *Payload) {
  lua_pushcfunction(State, Function, "Luna.WriteExposedClassValue");
  lua_pushlightuserdata(State, Payload);
  return lua_pcall(State, 1, 0, 0) == LUA_OK;
}

} // namespace

std::string_view ClassExposureStatusText(ClassExposureStatus Status) noexcept {
  switch (Status) {
  case ClassExposureStatus::Created:
    return "created";
  case ClassExposureStatus::Reused:
    return "reused";
  case ClassExposureStatus::UnavailableRequest:
    return "unavailable_request";
  case ClassExposureStatus::NullStorage:
    return "null_storage";
  case ClassExposureStatus::StorageUnavailable:
    return "storage_unavailable";
  case ClassExposureStatus::ConflictingOwnership:
    return "conflicting_ownership";
  case ClassExposureStatus::IncompatibleType:
    return "incompatible_type";
  case ClassExposureStatus::IncompatibleAccess:
    return "incompatible_access";
  case ClassExposureStatus::UnestablishedOwnership:
    return "unestablished_ownership";
  case ClassExposureStatus::MetatableUnavailable:
    return "metatable_unavailable";
  case ClassExposureStatus::StackCapacityFailure:
    return "stack_capacity_failure";
  case ClassExposureStatus::ProtectedFailure:
    return "protected_failure";
  }
  return "protected_failure";
}

bool DeclaresObjectConstruction(const ClassExposureIntent &Intent) noexcept {
  return Intent.Storage == nullptr && Intent.Allocator.DeclaresAllocation() &&
         (static_cast<bool>(Intent.Construct) ||
          Intent.Allocator.DeclaresConstruction());
}

ClassExposureResult PushExposedClassValue(lua_State *State,
                                          UserdataExposureContext &Context,
                                          RegisteredClass &Registered,
                                          const ClassExposureIntent &Intent) {
  if (!State || !Context.IsUsable() || !Registered.IsComplete() ||
      !ClassRegistry::Matches(Registered, Context.Origin, Registered.Type,
                              Registered.ClassSymbol))
    return Refuse(ClassExposureStatus::UnavailableRequest);

  const bool Creates = DeclaresObjectConstruction(Intent);
  if (Intent.Storage == nullptr && !Creates)
    return Refuse(ClassExposureStatus::NullStorage);

  if (Intent.Ownership == OwnershipModel::Borrowed) {
    if (!Intent.Handle.IsDeclared())
      return Refuse(ClassExposureStatus::UnestablishedOwnership,
                    OwnershipFailure::MissingLifetimeHandle);
    if (!Intent.Handle.IsValid())
      return Refuse(ClassExposureStatus::UnestablishedOwnership,
                    OwnershipFailure::ExpiredLifetimeHandle);
  }

  UserdataCacheRequest Wanted;
  Wanted.Origin = Context.Origin;
  Wanted.Address = Intent.Storage;
  Wanted.DynamicType = Registered.Type;
  Wanted.DeclaredViewType = Registered.Type;
  Wanted.ClassSymbol = Registered.ClassSymbol;
  Wanted.Metatable = Registered.Metatable;
  Wanted.Ownership = Intent.Ownership;
  Wanted.Access = Intent.Access;
  Wanted.DispatchGeneration = Context.DispatchGeneration;

  UserdataCacheLookup Decided;
  if (!Creates) {
    Decided = Context.Cache->Evaluate(Wanted);
    if (!Decided.PermitsCreation() && !Decided.PermitsReuse())
      return Refuse(StatusFor(Decided.Decision));
  }

  if (!lua_checkstack(State, 8))
    return Refuse(ClassExposureStatus::StackCapacityFailure);

  if (!PublishUserdataAccessContext(State, Context.Access))
    return Refuse(ClassExposureStatus::ProtectedFailure);
  if (Intent.Ownership == OwnershipModel::Borrowed)
    Context.Access->HandleProbe = &ObserveLifetimeHandleGeneration;

  if (Decided.PermitsReuse()) {
    if (PushCachedUserdataValue(State, Intent.Storage)) {
      ClassExposureResult Result;
      Result.Status = ClassExposureStatus::Reused;
      Result.Identity = Decided.Entry->Identity;
      return Result;
    }

    static_cast<void>(Context.Cache->Forget(Decided.Entry->Identity));
  }

  void *Storage = Intent.Storage;
  if (Creates) {
    const StorageAllocationOutcome Allocated =
        Context.Ownership->Allocate(Intent.Allocator);
    if (!Allocated.Succeeded())
      return Refuse(ClassExposureStatus::StorageUnavailable);
    Storage = Allocated.Storage;
    Wanted.Address = Storage;
  }

  UserdataHeaderRequest HeaderRequest;
  HeaderRequest.Origin = Context.Origin;
  HeaderRequest.DynamicType = Wanted.DynamicType;
  HeaderRequest.DeclaredViewType = Wanted.DeclaredViewType;
  HeaderRequest.ClassSymbol = Wanted.ClassSymbol;
  HeaderRequest.Metatable = Wanted.Metatable;
  HeaderRequest.Ownership = Wanted.Ownership;
  HeaderRequest.Access = Wanted.Access;
  HeaderRequest.DispatchGeneration = Context.DispatchGeneration;
  UserdataHeader Prepared = MakeUserdataHeader(HeaderRequest);

  StagedStorage Staged;
  Staged.Storage = Storage;
  Staged.Identity.Address = Storage;
  Staged.Identity.Nonce = Context.Nonces->Next();
  Staged.Allocator = Intent.Allocator;

  const OwnershipOutcome Staging = Context.Ownership->Stage(Prepared, Staged);
  if (!Staging.Succeeded) {
    if (Creates)
      Context.Ownership->DiscardStorage(Intent.Allocator, Storage);
    return Refuse(ClassExposureStatus::UnestablishedOwnership, Staging.Failure);
  }

  const OwnershipOutcome Constructed =
      Creates ? Context.Ownership->Construct(Prepared, Intent.Construct)
              : Context.Ownership->Construct(Prepared);
  if (!Constructed.Succeeded) {
    static_cast<void>(Context.Ownership->Release(
        Prepared, ReleaseCause::ConstructionFailure));
    return Refuse(ClassExposureStatus::UnestablishedOwnership,
                  Constructed.Failure);
  }

  const OwnershipOutcome Established =
      Context.Ownership->Establish(Prepared, RequestFrom(Intent));
  if (!Established.Succeeded) {
    static_cast<void>(
        Context.Ownership->Release(Prepared, ReleaseCause::PublicationFailure));
    return Refuse(ClassExposureStatus::UnestablishedOwnership,
                  Established.Failure);
  }

  const int EntryDepth = lua_gettop(State);
  ClassExposureStatus Failed = ClassExposureStatus::ProtectedFailure;
  try {
    void *Block =
        lua_newuserdatatagged(State, sizeof(UserdataHeader), TypedUserdataTag);
    if (Block != nullptr) {
      const int ValueIndex = lua_gettop(State);
      std::memcpy(Block, &Prepared, sizeof(UserdataHeader));

      if (!PushRegisteredClassMetatable(State, Registered)) {
        Failed = ClassExposureStatus::MetatableUnavailable;
      } else {
        lua_setmetatable(State, ValueIndex);

        UserdataCacheEntry Recorded;
        Recorded.Origin = Wanted.Origin;
        Recorded.Identity = Staged.Identity;
        Recorded.DynamicType = Wanted.DynamicType;
        Recorded.DeclaredViewType = Wanted.DeclaredViewType;
        Recorded.ClassSymbol = Wanted.ClassSymbol;
        Recorded.Metatable = Wanted.Metatable;
        Recorded.Ownership = Wanted.Ownership;
        Recorded.Access = Wanted.Access;
        Recorded.DispatchGeneration = Wanted.DispatchGeneration;
        Recorded.IsActive = true;

        UserdataHeader *Written =
            WrittenHeaderOf(Block, sizeof(UserdataHeader));
        if (Written != nullptr && Context.Cache->Record(Recorded)) {
          StoreCachedUserdataValue(State, Storage, ValueIndex);

          const OwnershipOutcome Published =
              Context.Ownership->Publish(*Written);
          if (Published.Succeeded) {
            ClassExposureResult Result;
            Result.Status = ClassExposureStatus::Created;
            Result.Identity = Staged.Identity;
            return Result;
          }

          DropCachedUserdataValue(State, Storage);
          static_cast<void>(Context.Cache->Forget(Staged.Identity));
          Failed = ClassExposureStatus::UnestablishedOwnership;
          lua_settop(State, EntryDepth);
          static_cast<void>(Context.Ownership->Release(
              Prepared, ReleaseCause::PublicationFailure));
          return Refuse(Failed, Published.Failure);
        }
      }
    }
  } catch (...) {
    Failed = ClassExposureStatus::ProtectedFailure;
  }

  lua_settop(State, EntryDepth);
  static_cast<void>(
      Context.Ownership->Release(Prepared, ReleaseCause::PublicationFailure));
  return Refuse(Failed);
}

ClassValueWriteObservation
WriteExposedClassValue(lua_State *State, const TypeGeneration &Types,
                       const StableTypeKey &Key, const std::string &Path,
                       std::shared_ptr<const ClassExposureIntent> Intent) {
  ClassValueWriteObservation Observed;
  Observed.Failure =
      std::string(StructuredFailureText(StructuredFailure::InternalFailure));
  if (!State || Path.empty() || !Intent)
    return Observed;

  const std::vector<std::string> Segments = SplitVmPath(Path);
  if (Segments.empty())
    return Observed;

  Observed.EntryStackDepth = lua_gettop(State);
  Observed.FinalStackDepth = Observed.EntryStackDepth;

  const TypeDescriptor Type = TypeDescriptor::ForClass(Key);

  void *const Exposed = Intent->Storage;
  const bool PermitsMutation = Intent->Access == ConstAccess::Mutable;
  const StructuredValue Staged = StructuredValue::ExposedHandle(
      Exposed, PermitsMutation, std::move(Intent));
  StructuredWriteResult Written;

  StackCheckpoint Checkpoint(State);
  if (!lua_checkstack(State, static_cast<int>(Segments.size()) + 16))
    return Observed;

  WritePayload Payload;
  Payload.Segments = &Segments;
  Payload.Types = &Types;
  Payload.Type = &Type;
  Payload.Staged = &Staged;
  Payload.Written = &Written;
  if (!CallProtectedWrite(State, WriteValue, &Payload) || !Payload.Attempted)
    return Observed;

  Observed.PublishedCount = Written.PublishedCount;
  Observed.Published = Payload.Stored;
  Observed.FinalStackDepth = lua_gettop(State);
  if (Written.IsSuccess()) {
    Observed.Failure =
        std::string(StructuredFailureText(StructuredFailure::None));
    return Observed;
  }

  ConversionSubject Subject;
  Subject.Kind = ConversionSubjectKind::Callable;
  Subject.Name = "userdata exposure";
  Observed.Failure =
      std::string(StructuredFailureText(Written.Diagnostic.Failure));
  Observed.Diagnostic = DescribeConversionFailure(
      Subject, ConversionDirection::Return, 1, Written.Diagnostic);
  return Observed;
}

bool PublishUserdataExposureContext(lua_State *State,
                                    UserdataExposureContext *Context) noexcept {
  if (!State || !Context || !Context->IsUsable())
    return false;
  if (!lua_checkstack(State, 4))
    return false;

  StackCheckpoint Checkpoint(State);
  lua_pushlightuserdata(State, Context);
  lua_rawsetfield(State, LUA_REGISTRYINDEX, ExposureContextSlot);
  return true;
}

UserdataExposureContext *
ObserveUserdataExposureContext(lua_State *State) noexcept {
  if (!State || !lua_checkstack(State, 2))
    return nullptr;

  StackCheckpoint Checkpoint(State);
  lua_rawgetfield(State, LUA_REGISTRYINDEX, ExposureContextSlot);
  auto *Context =
      static_cast<UserdataExposureContext *>(lua_tolightuserdata(State, -1));
  if (Context == nullptr || !Context->IsUsable())
    return nullptr;
  return Context;
}

} // namespace Luna::Detail
