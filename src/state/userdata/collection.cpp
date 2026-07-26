// clang-format off
#include "state/userdata/collection.hpp"

#include "state/transaction/lifecycle.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/ownership.hpp"

#include <lua.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

// The process-wide collection tables are guarded by a lock that cannot throw
// and cannot allocate: the collector holds it from inside the collector's own
// traversal, where neither is acceptable.
std::atomic_flag CollectionLock = ATOMIC_FLAG_INIT;

class CollectionGuard final {
public:
  CollectionGuard() noexcept {
    while (CollectionLock.test_and_set(std::memory_order_acquire)) {
    }
  }

  ~CollectionGuard() { CollectionLock.clear(std::memory_order_release); }

  CollectionGuard(const CollectionGuard &) = delete;
  CollectionGuard &operator=(const CollectionGuard &) = delete;
  CollectionGuard(CollectionGuard &&) = delete;
  CollectionGuard &operator=(CollectionGuard &&) = delete;
};

// One logical State's release gate. The identity is a process-monotonic number
// that is never reused, so a retired route can never be matched again.
struct CollectionRoute final {
  std::uint64_t Origin = 0;
  OwnershipRegistry *Gate = nullptr;
};

[[nodiscard]] std::vector<CollectionRoute> &Routes() {
  static std::vector<CollectionRoute> Table;
  return Table;
}

[[nodiscard]] UserdataCollectionCounters &Counters() noexcept {
  static UserdataCollectionCounters Counted;
  return Counted;
}

[[nodiscard]] StateDestructionObservation &LastDestruction() noexcept {
  static StateDestructionObservation Observed;
  return Observed;
}

// The gate one logical State's values route into, or null when that State has
// no route any more.
[[nodiscard]] OwnershipRegistry *FindGate(std::uint64_t Origin) noexcept {
  if (Origin == 0)
    return nullptr;
  for (const CollectionRoute &Route : Routes()) {
    if (Route.Origin == Origin)
      return Route.Gate;
  }
  return nullptr;
}

// The `__gc` boundary of one typed userdata, as Luau spells it: the destructor
// of Luna's own userdata tag, called immediately before the block is freed.
//
// Nothing here touches the virtual machine, and nothing here throws. The
// virtual-machine handle is deliberately unused: the collector is
// mid-traversal, so the machine is not re-entrant, and every piece of state
// this needs is Luna-owned and lives outside it.
void CollectTypedUserdata(lua_State *, void *Block) noexcept {
  try {
    UserdataHeader *Header = nullptr;
    OwnershipRegistry *Gate = nullptr;
    {
      CollectionGuard Guard;
      UserdataCollectionCounters &Counted = Counters();
      ++Counted.Entered;

      // The block is only a candidate until its marker and layout version say
      // otherwise, exactly as on the access path.
      if (InspectUserdataHeader(Block, sizeof(UserdataHeader)) == nullptr) {
        ++Counted.ForeignBlock;
        return;
      }

      Header = static_cast<UserdataHeader *>(Block);
      Gate = FindGate(Header->Origin.Value());
      if (Gate == nullptr) {
        ++Counted.UnroutedOrigin;
        return;
      }
    }

    // The gate runs outside Luna's own collection lock, because it calls the
    // consumer's destruction and deallocation steps and nothing those do may be
    // able to block against another State's destruction.
    //
    // The gate invalidates access before it releases anything, and it releases
    // one value at most once, so a value an explicit release already ended is a
    // no-op here rather than a second release.
    const bool Released = Gate->ReleaseCollected(*Header);

    CollectionGuard Guard;
    if (Released)
      ++Counters().Released;
    else
      ++Counters().AlreadyReleased;
  } catch (...) {
    // Nothing may cross this boundary into the virtual machine, so the
    // exception ends here and is counted.
    CollectionGuard Guard;
    ++Counters().ContainedException;
  }
}

} // namespace

bool InstallUserdataCollector(lua_State *Machine) noexcept {
  if (Machine == nullptr)
    return false;
  lua_setuserdatadtor(Machine, TypedUserdataTag, &CollectTypedUserdata);
  return lua_getuserdatadtor(Machine, TypedUserdataTag) != nullptr;
}

bool UserdataCollectorIsInstalled(lua_State *Machine) noexcept {
  if (Machine == nullptr)
    return false;
  return lua_getuserdatadtor(Machine, TypedUserdataTag) ==
         &CollectTypedUserdata;
}

void PublishUserdataCollectionRoute(StateIdentity Origin,
                                    OwnershipRegistry *Gate) noexcept {
  if (!Origin.IsValid() || Gate == nullptr)
    return;

  try {
    CollectionGuard Guard;
    for (CollectionRoute &Route : Routes()) {
      if (Route.Origin == Origin.Value()) {
        Route.Gate = Gate;
        return;
      }
    }

    CollectionRoute Added;
    Added.Origin = Origin.Value();
    Added.Gate = Gate;
    Routes().push_back(Added);
  } catch (...) {
    // A route Luna could not record only means collection releases nothing:
    // the State's own final sweep still releases every value exactly once.
  }
}

void RetireUserdataCollectionRoute(StateIdentity Origin) noexcept {
  if (!Origin.IsValid())
    return;

  CollectionGuard Guard;
  std::vector<CollectionRoute> &Table = Routes();
  for (std::size_t Index = 0; Index < Table.size(); ++Index) {
    if (Table[Index].Origin != Origin.Value())
      continue;
    Table.erase(Table.begin() + static_cast<std::ptrdiff_t>(Index));
    return;
  }
}

UserdataCollectionCounters ObserveUserdataCollections() noexcept {
  CollectionGuard Guard;
  return Counters();
}

void ResetUserdataCollections() noexcept {
  CollectionGuard Guard;
  Counters() = UserdataCollectionCounters{};
  LastDestruction() = StateDestructionObservation{};
}

void RecordStateDestruction(const StateDestructionObservation &Observed) {
  CollectionGuard Guard;
  LastDestruction() = Observed;
}

StateDestructionObservation ObserveLastStateDestruction() noexcept {
  CollectionGuard Guard;
  return LastDestruction();
}

} // namespace Luna::Detail
