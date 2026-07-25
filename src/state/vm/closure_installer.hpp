#pragma once

// clang-format off
#include <string>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class BindingRecord;

enum class ClosureInstallationStatus {
  Success,
  StackCapacityFailure,
  ProtectedFailure,
  RollbackFailure
};

[[nodiscard]] ClosureInstallationStatus
InstallBindingClosure(lua_State *State, BindingRecord &Record,
                      bool InjectFailure) noexcept;

[[nodiscard]] const BindingRecord *
ObserveInstalledBinding(lua_State *State,
                        const std::string &GlobalName) noexcept;

} // namespace Luna::Detail
