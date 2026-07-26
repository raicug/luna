// clang-format off
#include "state/userdata/identity.hpp"

#include <string_view>
// clang-format on

namespace Luna::Detail {

std::string_view OwnershipModelText(OwnershipModel Model) noexcept {
  switch (Model) {
  case OwnershipModel::Borrowed:
    return "borrowed";
  case OwnershipModel::LuaOwned:
    return "lua_owned";
  case OwnershipModel::Shared:
    return "shared";
  }
  return "unknown";
}

std::string_view LifetimeStateText(LifetimeState State) noexcept {
  switch (State) {
  case LifetimeState::Allocated:
    return "allocated";
  case LifetimeState::Constructed:
    return "constructed";
  case LifetimeState::Published:
    return "published";
  case LifetimeState::Invalid:
    return "invalid";
  case LifetimeState::Destroyed:
    return "destroyed";
  case LifetimeState::SharedReleased:
    return "shared_released";
  case LifetimeState::Released:
    return "released";
  }
  return "unknown";
}

std::string_view ConstAccessText(ConstAccess Access) noexcept {
  switch (Access) {
  case ConstAccess::Mutable:
    return "mutable";
  case ConstAccess::Const:
    return "const";
  }
  return "unknown";
}

} // namespace Luna::Detail
