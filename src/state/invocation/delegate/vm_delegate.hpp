#pragma once

// clang-format off
#include <luna/binding/delegate.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
// clang-format on

struct lua_State;

namespace Luna::Detail {

struct DelegateCounters final {
  std::size_t Adopted = 0;
  std::size_t Released = 0;
  std::size_t Invalidated = 0;
  std::size_t Invocations = 0;
  std::size_t Failures = 0;
  std::size_t ForeignThreadRefusals = 0;
};

struct DelegateLink final {
  mutable std::mutex Barrier;

  lua_State *Thread = nullptr;
  std::thread::id Owner;
  bool Alive = false;
  std::uint64_t Epoch = 1;
  std::uint64_t NextIdentity = 1;

  std::vector<int> Outstanding;
  DelegateCounters Counters;
};

class VmDelegateRegistry final {
public:
  VmDelegateRegistry();
  ~VmDelegateRegistry();

  VmDelegateRegistry(const VmDelegateRegistry &) = delete;
  VmDelegateRegistry &operator=(const VmDelegateRegistry &) = delete;

  void Bind(lua_State *Root) noexcept;

  [[nodiscard]] std::shared_ptr<DelegateTarget>
  Adopt(lua_State *State, int StackIndex, const DelegateShape &Declared);

  std::size_t InvalidateEverything() noexcept;

  void Retire() noexcept;

  [[nodiscard]] std::size_t OutstandingCount() const noexcept;
  [[nodiscard]] DelegateCounters Counters() const noexcept;

private:
  std::shared_ptr<DelegateLink> LinkValue;
};

[[nodiscard]] bool
PublishDelegateRegistry(lua_State *State,
                        VmDelegateRegistry *Registry) noexcept;

[[nodiscard]] VmDelegateRegistry *
ObserveDelegateRegistry(lua_State *State) noexcept;

} // namespace Luna::Detail
