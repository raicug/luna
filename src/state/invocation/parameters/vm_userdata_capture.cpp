// clang-format off
#include "state/invocation/parameters/vm_userdata_capture.hpp"

#include "state/userdata/header.hpp"
#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr const char *UserdataCaptureRegistrySlot = "Luna.UserdataCaptures";

class VmCapturedUserdataTarget final : public CapturedUserdataTarget {
public:
  VmCapturedUserdataTarget(std::shared_ptr<UserdataCaptureLink> Link,
                           int Reference, std::uint64_t Epoch,
                           CapturedUserdataIdentity Described,
                           const void *Block, std::size_t ByteCount) noexcept
      : LinkValue(std::move(Link)), ReferenceValue(Reference),
        EpochValue(Epoch), DescribedValue(std::move(Described)),
        BlockValue(Block), ByteCountValue(ByteCount) {}

  ~VmCapturedUserdataTarget() override { Release(); }

  [[nodiscard]] bool IsLive() const noexcept override {
    if (!LinkValue || ReleasedValue)
      return false;
    const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
    return LinkValue->Alive && LinkValue->Epoch == EpochValue;
  }

  [[nodiscard]] std::string_view ClassName() const noexcept override {
    return DescribedValue.ClassName;
  }

  [[nodiscard]] TypeId CapturedType() const noexcept override {
    return DescribedValue.CapturedType;
  }

  [[nodiscard]] void *Storage() const noexcept override {
    const UserdataAccessResult Access = InspectAccess(false);
    return Access.IsPermitted() ? Access.Storage : nullptr;
  }

  [[nodiscard]] bool PermitsMutation() const noexcept override {
    return InspectAccess(true).IsPermitted();
  }

  void Release() noexcept override {
    if (!LinkValue || ReleasedValue)
      return;
    ReleasedValue = true;

    const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);

    if (LinkValue->Epoch != EpochValue)
      return;

    const auto Found = std::find(LinkValue->Outstanding.begin(),
                                 LinkValue->Outstanding.end(), ReferenceValue);
    if (Found == LinkValue->Outstanding.end())
      return;
    LinkValue->Outstanding.erase(Found);
    LinkValue->Counters.Released += 1;
    if (LinkValue->Alive && LinkValue->Thread &&
        std::this_thread::get_id() == LinkValue->Owner)
      lua_unref(LinkValue->Thread, ReferenceValue);
  }

  [[nodiscard]] bool PushOnto(lua_State *State) const {
    if (!LinkValue)
      return false;

    lua_State *Thread = nullptr;
    int Reference = 0;
    {
      const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
      if (ReleasedValue || !LinkValue->Alive || LinkValue->Epoch != EpochValue)
        return false;
      if (std::this_thread::get_id() != LinkValue->Owner)
        return false;
      Thread = LinkValue->Thread;
      Reference = ReferenceValue;
    }

    if (!Thread || Thread != State || !lua_checkstack(State, 1))
      return false;

    lua_getref(State, Reference);
    if (lua_type(State, -1) != LUA_TUSERDATA) {
      lua_pop(State, 1);
      return false;
    }
    return true;
  }

private:
  [[nodiscard]] UserdataAccessResult
  InspectAccess(bool RequiresMutation) const noexcept {
    if (!IsLive() || BlockValue == nullptr)
      return UserdataAccessResult();

    const UserdataHeader *Header =
        InspectUserdataHeader(BlockValue, ByteCountValue);
    if (Header == nullptr)
      return UserdataAccessResult();

    UserdataAccessRequest Request;
    Request.Origin = DescribedValue.Origin;
    Request.Metatable = Header->Metatable;
    Request.RequestedType = Header->DynamicType;
    Request.RequiresMutation = RequiresMutation;
    Request.HandleProbe = DescribedValue.HandleProbe;
    return ValidateUserdataAccess(*Header, Request);
  }

  std::shared_ptr<UserdataCaptureLink> LinkValue;
  int ReferenceValue = 0;
  std::uint64_t EpochValue = 0;
  CapturedUserdataIdentity DescribedValue;
  const void *BlockValue = nullptr;
  std::size_t ByteCountValue = 0;
  bool ReleasedValue = false;
};

} // namespace

VmUserdataCaptureRegistry::VmUserdataCaptureRegistry()
    : LinkValue(std::make_shared<UserdataCaptureLink>()) {}

VmUserdataCaptureRegistry::~VmUserdataCaptureRegistry() { Retire(); }

void VmUserdataCaptureRegistry::Bind(lua_State *Root) noexcept {
  if (!LinkValue)
    return;
  const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
  LinkValue->Thread = Root;
  LinkValue->Owner = std::this_thread::get_id();
  LinkValue->Alive = Root != nullptr;
}

std::shared_ptr<CapturedUserdataTarget>
VmUserdataCaptureRegistry::Adopt(lua_State *State, int StackIndex,
                                 CapturedUserdataIdentity Described) {
  if (!LinkValue || !State)
    return nullptr;

  StackCheckpoint Checkpoint(State);
  if (!lua_checkstack(State, 2))
    return nullptr;
  if (lua_type(State, StackIndex) != LUA_TUSERDATA)
    return nullptr;

  const void *Block = lua_touserdata(State, StackIndex);
  const auto ByteCount =
      static_cast<std::size_t>(lua_objlen(State, StackIndex));

  lua_pushvalue(State, StackIndex);
  const int Reference = lua_ref(State, -1);
  if (Reference <= 0)
    return nullptr;

  std::uint64_t Epoch = 0;
  {
    const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
    if (!LinkValue->Alive) {
      lua_unref(State, Reference);
      return nullptr;
    }
    Epoch = LinkValue->Epoch;
    LinkValue->Outstanding.push_back(Reference);
    LinkValue->Counters.Adopted += 1;
  }

  return std::make_shared<VmCapturedUserdataTarget>(
      LinkValue, Reference, Epoch, std::move(Described), Block, ByteCount);
}

std::size_t VmUserdataCaptureRegistry::InvalidateEverything() noexcept {
  if (!LinkValue)
    return 0;
  const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
  const std::size_t Invalidated = LinkValue->Outstanding.size();
  if (LinkValue->Alive && LinkValue->Thread &&
      std::this_thread::get_id() == LinkValue->Owner) {
    for (const int Reference : LinkValue->Outstanding)
      lua_unref(LinkValue->Thread, Reference);
  }
  LinkValue->Outstanding.clear();
  LinkValue->Epoch += 1;
  LinkValue->Counters.Invalidated += Invalidated;
  return Invalidated;
}

void VmUserdataCaptureRegistry::Retire() noexcept {
  if (!LinkValue)
    return;
  const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
  LinkValue->Counters.Invalidated += LinkValue->Outstanding.size();
  LinkValue->Outstanding.clear();
  LinkValue->Alive = false;
  LinkValue->Thread = nullptr;
}

std::size_t VmUserdataCaptureRegistry::OutstandingCount() const noexcept {
  if (!LinkValue)
    return 0;
  const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
  return LinkValue->Outstanding.size();
}

UserdataCaptureCounters VmUserdataCaptureRegistry::Counters() const noexcept {
  if (!LinkValue)
    return UserdataCaptureCounters();
  const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
  return LinkValue->Counters;
}

bool PublishUserdataCaptureRegistry(
    lua_State *State, VmUserdataCaptureRegistry *Registry) noexcept {
  if (!State || !Registry)
    return false;
  if (!lua_checkstack(State, 4))
    return false;

  StackCheckpoint Checkpoint(State);
  lua_pushlightuserdata(State, Registry);
  lua_rawsetfield(State, LUA_REGISTRYINDEX, UserdataCaptureRegistrySlot);
  return true;
}

VmUserdataCaptureRegistry *
ObserveUserdataCaptureRegistry(lua_State *State) noexcept {
  if (!State || !lua_checkstack(State, 2))
    return nullptr;

  StackCheckpoint Checkpoint(State);
  lua_rawgetfield(State, LUA_REGISTRYINDEX, UserdataCaptureRegistrySlot);
  return static_cast<VmUserdataCaptureRegistry *>(
      lua_tolightuserdata(State, -1));
}

bool PushCapturedUserdataValue(lua_State *State,
                               const CapturedUserdataTarget &Target) {
  return static_cast<const VmCapturedUserdataTarget &>(Target).PushOnto(State);
}

} // namespace Luna::Detail
