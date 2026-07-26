#pragma once

// Protected collection of typed userdata, and the destruction ordering that
// makes it safe.
//
// A collected value must end exactly the way an explicitly released one does:
// through the one idempotent release gate, with access invalidated before
// anything is released, and with nothing thrown escaping into the virtual
// machine. Luau expresses that boundary as a destructor of the userdata's tag
// rather than as a `__gc` field a script could see or replace, so Luna owns one
// tag for its typed userdata and installs exactly one collector for it per
// virtual machine. A block of any other tag is never Luna's and is never
// touched here.
//
// The collector runs inside the collector's own traversal, so it may not
// re-enter the virtual machine at all: no stack operation, no protected call,
// no registry access. Everything it needs is Luna-owned and outside the
// machine. It finds the release gate of the State the value came from through
// the value's logical State identity - a process-monotonic number that is never
// an address and never reused - so a stale block can never reach a gate that is
// gone, and it then looks the value up by its own native identity, so a value
// that was already released is a no-op rather than a second release.
//
// Destruction ordering is the other half of the same guarantee. A State first
// refuses every new invocation, then closes and finalizes its virtual machine
// while its class, allocator, type, dispatch, and release metadata are all
// still valid - which is what lets each collected value take the route above -
// and only then releases whatever the machine never held. The route dies last,
// so nothing can reach the gate after the gate stops being valid.

// clang-format off
#include "state/transaction/lifecycle.hpp"

#include <cstddef>
#include <cstdint>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class OwnershipRegistry;

// The one userdata tag Luna's typed values carry. Luau calls the destructor of
// a tag immediately before freeing a value of that tag, which is exactly the
// `__gc` boundary of a typed userdata; nothing else in the process creates a
// value of this tag, so the collector below only ever sees Luna blocks.
inline constexpr int TypedUserdataTag = 1;

// Exactly what the collection boundary did, across every State in the process.
// Every counter is a boundary count, so a test can predict all of them from the
// values it exposed and collected.
struct UserdataCollectionCounters final {
  // How many times the collector was entered at all.
  std::uint64_t Entered = 0;

  // Blocks that routed into a release gate and released one value, and blocks
  // whose value the gate had already released.
  std::uint64_t Released = 0;
  std::uint64_t AlreadyReleased = 0;

  // Blocks that do not carry Luna's marker and layout version, and blocks whose
  // logical State no longer has a route.
  std::uint64_t ForeignBlock = 0;
  std::uint64_t UnroutedOrigin = 0;

  // Exceptions the boundary contained instead of letting them reach the virtual
  // machine. It must stay possible for this to be non-zero without anything
  // else changing.
  std::uint64_t ContainedException = 0;
};

// What the destruction of one State observed about its own ordering. It is
// recorded after the State is gone, which is the only moment the whole ordering
// is knowable.
struct StateDestructionObservation final {
  bool Observed = false;

  // Readiness and both userdata contexts refused before the machine closed, so
  // no new invocation, registration, access, or exposure could start.
  bool RefusedNewInvocations = false;

  // Every value the gate still owned retained the type, allocator, metatable,
  // and dispatch metadata its cleanup needs, checked while the machine was
  // still open.
  bool RetainedCleanupMetadata = false;

  // Values released by the machine's finalizers during close, and values the
  // machine never held that the explicit final sweep released.
  std::size_t ReleasedDuringClose = 0;
  std::size_t ReleasedAfterClose = 0;

  // Cleanup steps that ran without the metadata they require. It must stay
  // zero.
  std::uint64_t IncompleteMetadata = 0;
};

// Installs Luna's collector for its typed userdata tag in one virtual machine.
// It is installed once, when the machine is created, so every value of every
// class is collected through it.
[[nodiscard]] bool InstallUserdataCollector(lua_State *Machine) noexcept;

// Whether one virtual machine has Luna's collector installed.
[[nodiscard]] bool UserdataCollectorIsInstalled(lua_State *Machine) noexcept;

// The release gate a collected value of one logical State routes into.
// Publishing is idempotent; retiring makes every later collection of that
// State's values a no-op instead of a use of a gate that is gone.
void PublishUserdataCollectionRoute(StateIdentity Origin,
                                    OwnershipRegistry *Gate) noexcept;
void RetireUserdataCollectionRoute(StateIdentity Origin) noexcept;

[[nodiscard]] UserdataCollectionCounters ObserveUserdataCollections() noexcept;
void ResetUserdataCollections() noexcept;

void RecordStateDestruction(const StateDestructionObservation &Observed);
[[nodiscard]] StateDestructionObservation
ObserveLastStateDestruction() noexcept;

} // namespace Luna::Detail
