// clang-format off
#include "state/invocation/parameters/vm_userdata_capture.hpp"

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

// One captured userdata value the virtual machine owns, held through Luna's
// reference mechanism. It never exposes the reference or the stack it came
// from, mirroring VmDelegateTarget exactly.
class VmCapturedUserdataTarget final : public CapturedUserdataTarget {
public:
  VmCapturedUserdataTarget(std::shared_ptr<UserdataCaptureLink> Link,
                           int Reference, std::uint64_t Epoch,
                           std::string ClassName) noexcept
      : LinkValue(std::move(Link)), ReferenceValue(Reference),
        EpochValue(Epoch), ClassNameValue(std::move(ClassName)) {}

  ~VmCapturedUserdataTarget() override { Release(); }

  [[nodiscard]] bool IsLive() const noexcept override {
    if (!LinkValue || ReleasedValue)
      return false;
    const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
    return LinkValue->Alive && LinkValue->Epoch == EpochValue;
  }

  [[nodiscard]] std::string_view ClassName() const noexcept override {
    return ClassNameValue;
  }

  void Release() noexcept override {
    if (!LinkValue || ReleasedValue)
      return;
    ReleasedValue = true;

    const std::lock_guard<std::mutex> Guard(LinkValue->Barrier);
    // The reference slot number is reused once a mass invalidation unrefs
    // it, so a target from an epoch that has already moved on must never
    // touch `Outstanding` or unref by number again, the same reasoning
    // VmDelegateTarget::Release already documents.
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
  std::shared_ptr<UserdataCaptureLink> LinkValue;
  int ReferenceValue = 0;
  std::uint64_t EpochValue = 0;
  std::string ClassNameValue;
  bool ReleasedValue = false;
};

} // namespace

VmUserdataCaptureRegistry::VmUserdataCaptureRegistry()
    : LinkValue(std::make_shared<UserdataCaptureLink>()) {}

VmUserdataCaptureRegistry::~VmUserdataCaptureRegistry() {
  Retire();
}

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
                                 std::string ClassName) {
  if (!LinkValue || !State)
    return nullptr;

  StackCheckpoint Checkpoint(State);
  if (!lua_checkstack(State, 2))
    return nullptr;
  if (lua_type(State, StackIndex) != LUA_TUSERDATA)
    return nullptr;

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

  return std::make_shared<VmCapturedUserdataTarget>(LinkValue, Reference, Epoch,
                                                    std::move(ClassName));
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
