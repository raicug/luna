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

[[nodiscard]] OwnershipRegistry *FindGate(std::uint64_t Origin) noexcept {
  if (Origin == 0)
    return nullptr;
  for (const CollectionRoute &Route : Routes()) {
    if (Route.Origin == Origin)
      return Route.Gate;
  }
  return nullptr;
}

void CollectTypedUserdata(lua_State *, void *Block) noexcept {
  try {
    UserdataHeader *Header = nullptr;
    OwnershipRegistry *Gate = nullptr;
    {
      CollectionGuard Guard;
      UserdataCollectionCounters &Counted = Counters();
      ++Counted.Entered;

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

    const bool Released = Gate->ReleaseCollected(*Header);

    CollectionGuard Guard;
    if (Released)
      ++Counters().Released;
    else
      ++Counters().AlreadyReleased;
  } catch (...) {
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
