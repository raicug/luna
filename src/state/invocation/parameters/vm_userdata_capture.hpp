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

// What a capture records about the value's own class and its owning scope.
// The origin identity and the lifetime-handle probe are kept so that handing
// the native object out later can run the same access gate a receiver runs,
// rather than trusting a pointer cached at capture time.
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

// Everything a captured userdata value shares with the State that owns it.
// Targets keep it alive, so a captured value that outlives its owning State
// refuses deterministically instead of touching a closed virtual machine —
// the same shape VmDelegateRegistry already establishes for a subscribed
// handler.
struct UserdataCaptureLink final {
  mutable std::mutex Barrier;

  lua_State *Thread = nullptr;
  std::thread::id Owner;
  bool Alive = false;
  std::uint64_t Epoch = 1;

  std::vector<int> Outstanding;
  UserdataCaptureCounters Counters;
};

// Owner-thread-only registry of every registered-class instance captured
// from the Luau stack into an OwnedValue's Userdata category (directly, or
// nested inside a table read as a variadic argument). It holds each value
// through Luna's own reference mechanism, the same way VmDelegateRegistry
// holds a subscribed handler, and invalidates every outstanding capture
// deterministically when the owning scope goes away.
class VmUserdataCaptureRegistry final {
public:
  VmUserdataCaptureRegistry();
  ~VmUserdataCaptureRegistry();

  VmUserdataCaptureRegistry(const VmUserdataCaptureRegistry &) = delete;
  VmUserdataCaptureRegistry &
  operator=(const VmUserdataCaptureRegistry &) = delete;

  void Bind(lua_State *Root) noexcept;

  // Adopts the userdata at StackIndex. The stack is left exactly as it was.
  // Returns nullptr if the value at StackIndex is not userdata. `Described`
  // names the registered class the block reported, so a consumer that knows
  // the concrete C++ type can confirm the identity before recovering the
  // native object.
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

// Pushes a previously captured userdata value back onto the stack. Returns
// false when the capture has been released or belongs to a foreign thread,
// in which case nothing is pushed.
[[nodiscard]] bool
PushCapturedUserdataValue(lua_State *State,
                          const CapturedUserdataTarget &Target);

} // namespace Luna::Detail
