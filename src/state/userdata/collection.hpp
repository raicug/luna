#pragma once

// clang-format off
#include "state/transaction/lifecycle.hpp"

#include <cstddef>
#include <cstdint>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class OwnershipRegistry;

inline constexpr int TypedUserdataTag = 1;

struct UserdataCollectionCounters final {
  std::uint64_t Entered = 0;

  std::uint64_t Released = 0;
  std::uint64_t AlreadyReleased = 0;

  std::uint64_t ForeignBlock = 0;
  std::uint64_t UnroutedOrigin = 0;

  std::uint64_t ContainedException = 0;
};

struct StateDestructionObservation final {
  bool Observed = false;

  bool RefusedNewInvocations = false;

  bool RetainedCleanupMetadata = false;

  std::size_t ReleasedDuringClose = 0;
  std::size_t ReleasedAfterClose = 0;

  std::uint64_t IncompleteMetadata = 0;
};

[[nodiscard]] bool InstallUserdataCollector(lua_State *Machine) noexcept;

[[nodiscard]] bool UserdataCollectorIsInstalled(lua_State *Machine) noexcept;

void PublishUserdataCollectionRoute(StateIdentity Origin,
                                    OwnershipRegistry *Gate) noexcept;
void RetireUserdataCollectionRoute(StateIdentity Origin) noexcept;

[[nodiscard]] UserdataCollectionCounters ObserveUserdataCollections() noexcept;
void ResetUserdataCollections() noexcept;

void RecordStateDestruction(const StateDestructionObservation &Observed);
[[nodiscard]] StateDestructionObservation
ObserveLastStateDestruction() noexcept;

} // namespace Luna::Detail
