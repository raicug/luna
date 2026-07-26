// clang-format off
#include "state/userdata/construction.hpp"

#include <luna/binding/class_allocator.hpp>
#include <luna/binding/class_construction.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/type/structural_types.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/type_generation.hpp"
#include "state/userdata/allocator.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/identity.hpp"
#include "state/userdata/ownership.hpp"
#include "state/userdata/value_exposure.hpp"

#include <lua.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] OwnershipModel ModelOf(ConstructionOwnership Ownership) noexcept {
  switch (Ownership) {
  case ConstructionOwnership::LuaOwned:
    return OwnershipModel::LuaOwned;
  case ConstructionOwnership::Borrowed:
    return OwnershipModel::Borrowed;
  case ConstructionOwnership::Shared:
    return OwnershipModel::Shared;
  }
  return OwnershipModel::Borrowed;
}

// The ownership statement of one staged construction result, in exactly the
// shape the exposure and release paths take. An object Luna creates arrives
// without storage: the protocol's allocation step produces it and the
// construction step builds into it, so every milestone of that object belongs
// to the one gate that also releases it.
[[nodiscard]] std::shared_ptr<const ClassExposureIntent>
IntentFrom(const ConstructedInstance &Produced) {
  auto Intent = std::make_shared<ClassExposureIntent>();
  Intent->Storage = Produced.Storage;
  Intent->Ownership = ModelOf(Produced.Ownership);
  Intent->Access =
      Produced.PermitsMutation ? ConstAccess::Mutable : ConstAccess::Const;
  Intent->Handle = Produced.Lifetime;
  Intent->SharedOwnership = Produced.SharedOwnership;
  Intent->Allocator = Produced.Allocator;

  if (Produced.Construct) {
    ClassAllocator::ConstructOperation Step = Produced.Construct;
    Intent->Construct = [Step](void *Storage) {
      const AllocatorStepResult Built = Step(Storage);
      return Built.Performed;
    };
  }
  return Intent;
}

// The object the value on top of the stack carries. An object Luna created has
// no address until its allocation step produced one, so its own published
// header is the only thing that names it.
[[nodiscard]] void *PublishedObjectOnTop(lua_State *State) noexcept {
  if (State == nullptr || lua_type(State, -1) != LUA_TUSERDATA)
    return nullptr;
  void *const Block = lua_touserdata(State, -1);
  const std::size_t ByteCount =
      Block != nullptr ? static_cast<std::size_t>(lua_objlen(State, -1)) : 0;
  const UserdataHeader *Header = InspectUserdataHeader(Block, ByteCount);
  return Header != nullptr ? Header->Payload.Storage : nullptr;
}

[[nodiscard]] InstancePublication Refuse(InstancePublicationStatus Status,
                                         std::string Diagnostic) {
  InstancePublication Result;
  Result.Status = Status;
  Result.Diagnostic = std::move(Diagnostic);
  return Result;
}

} // namespace

InstancePublication
PublishConstructedInstance(lua_State *State, const TypeGeneration &Types,
                           const StableTypeKey &Class,
                           const ConstructedInstance &Produced) noexcept {
  try {
    // Either the object already exists, or the staged result declares the
    // protocol and the construction step that will create it. A result that
    // declares neither produced nothing at all.
    const bool Creates = Produced.Storage == nullptr &&
                         Produced.Allocator.DeclaresAllocation() &&
                         (static_cast<bool>(Produced.Construct) ||
                          Produced.Allocator.DeclaresConstruction());
    if (Produced.Storage == nullptr && !Creates)
      return Refuse(InstancePublicationStatus::MissingObject,
                    "the declaration produced no object.");

    // The class is resolved through the canonical type registry of the captured
    // generation, never through anything the target returned.
    const TypeDescriptor Declared = TypeDescriptor::ForClass(Class);
    if (!Types.IsAvailableForWrite(Declared))
      return Refuse(InstancePublicationStatus::UnavailableClass,
                    "the registered class of the result is unavailable in the "
                    "captured type registry.");

    const UserdataExposureContext *Context =
        ObserveUserdataExposureContext(State);
    if (Context == nullptr)
      return Refuse(InstancePublicationStatus::UnavailableContext,
                    "the userdata exposure context of this State is "
                    "unavailable.");

    std::shared_ptr<const ClassExposureIntent> Intent = IntentFrom(Produced);
    const StructuredValue Staged = StructuredValue::ExposedHandle(
        Produced.Storage, Produced.PermitsMutation, std::move(Intent));

    // How many values this State owned before the publication. Exactly one more
    // means this publication established the owner of its value; the same
    // number means the identity cache handed back a value that was already
    // owned, and that one is not this publication's to release.
    const std::size_t OwnedBefore =
        Context->Ownership != nullptr ? Context->Ownership->RecordCount() : 0;

    // Exactly the canonical class conversion a returned object passes: the
    // identity cache, the release gate, the metatable, and the protected
    // publication are all the ordinary ones, so a refusal anywhere releases
    // precisely what the completed milestones warrant and publishes nothing.
    const StructuredWriteResult Written =
        WriteStructuredValue(Types, State, Declared, Staged);
    if (Written.IsSuccess() && Written.PublishedCount == 1) {
      InstancePublication Result;
      Result.Status = InstancePublicationStatus::Published;
      Result.PublishedCount = 1;
      void *const Object = PublishedObjectOnTop(State);
      Result.Storage = Object != nullptr ? Object : Produced.Storage;
      Result.EstablishedOwner = Context->Ownership != nullptr &&
                                Context->Ownership->RecordCount() > OwnedBefore;
      return Result;
    }

    ConversionSubject Subject;
    Subject.Kind = ConversionSubjectKind::Callable;
    Subject.Name = "class construction";
    return Refuse(InstancePublicationStatus::RefusedExposure,
                  DescribeConversionFailure(Subject,
                                            ConversionDirection::Return, 1,
                                            Written.Diagnostic));
  } catch (...) {
    try {
      return Refuse(InstancePublicationStatus::RefusedExposure,
                    "Unexpected internal failure while publishing a "
                    "constructed object.");
    } catch (...) {
      return InstancePublication();
    }
  }
}

bool ReleasePublishedInstance(lua_State *State, void *Storage) noexcept {
  if (Storage == nullptr)
    return false;
  const UserdataExposureContext *Context =
      ObserveUserdataExposureContext(State);
  if (Context == nullptr || Context->Ownership == nullptr)
    return false;
  return Context->Ownership->ReleaseByStorage(Storage,
                                              ReleaseCause::PublicationFailure);
}

} // namespace Luna::Detail
