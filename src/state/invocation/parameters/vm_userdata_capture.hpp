#pragma once

// clang-format off
#include <luna/binding/conversion.hpp>
#include <luna/reflection/ids.hpp>

#include "state/userdata/access.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
// clang-format on

struct lua_State;

namespace Luna::Detail {

struct CapturedUserdataIdentity final {
  std::string ClassName;
  TypeId CapturedType;
  StateIdentity Origin;
  LifetimeHandleGenerationProbe HandleProbe = nullptr;
};

struct UserdataCaptureCounters final {
  std::size_t Adopted = 0;
  std::size_t Released = 0;
  std::size_t Invalidated = 0;
};

struct UserdataCaptureLink final {
  mutable std::mutex Barrier;

  lua_State *Thread = nullptr;
  std::thread::id Owner;
  bool Alive = false;
  std::uint64_t Epoch = 1;

  std::vector<int> Outstanding;
  UserdataCaptureCounters Counters;
};

class VmUserdataCaptureRegistry final {
public:
  VmUserdataCaptureRegistry();
  ~VmUserdataCaptureRegistry();

  VmUserdataCaptureRegistry(const VmUserdataCaptureRegistry &) = delete;
  VmUserdataCaptureRegistry &
  operator=(const VmUserdataCaptureRegistry &) = delete;

  void Bind(lua_State *Root) noexcept;

  [[nodiscard]] std::shared_ptr<CapturedUserdataTarget>
  Adopt(lua_State *State, int StackIndex, CapturedUserdataIdentity Described);

  std::size_t InvalidateEverything() noexcept;

  void Retire() noexcept;

  [[nodiscard]] std::size_t OutstandingCount() const noexcept;
  [[nodiscard]] UserdataCaptureCounters Counters() const noexcept;

private:
  std::shared_ptr<UserdataCaptureLink> LinkValue;
};

[[nodiscard]] bool
PublishUserdataCaptureRegistry(lua_State *State,
                               VmUserdataCaptureRegistry *Registry) noexcept;

[[nodiscard]] VmUserdataCaptureRegistry *
ObserveUserdataCaptureRegistry(lua_State *State) noexcept;

[[nodiscard]] bool
PushCapturedUserdataValue(lua_State *State,
                          const CapturedUserdataTarget &Target);

} // namespace Luna::Detail
