#pragma once

// clang-format off
#include "state/registration/plan.hpp"

#include <string>
#include <string_view>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class TypeGeneration;

enum class ValueInstallationStatus {
  Installed,
  ParentUnavailable,
  PathOccupied,
  ConversionRefused,
  ProtectedFailure,
  StackCapacityFailure
};

[[nodiscard]] std::string_view
ValueInstallationStatusText(ValueInstallationStatus Status) noexcept;

[[nodiscard]] ValueInstallationStatus
InstallValueAtVmPath(lua_State *State, const std::string &Path,
                     const TypeGeneration &Types,
                     const PlannedValue &Planned) noexcept;

[[nodiscard]] ValueInstallationStatus
InstallImmutableTableAtVmPath(lua_State *State, const std::string &Path,
                              const TypeGeneration &Types,
                              const PlannedValueTable &Planned) noexcept;

} // namespace Luna::Detail
