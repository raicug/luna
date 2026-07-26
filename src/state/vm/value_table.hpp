#pragma once

// Protected installation of one converted value, and of one Luna-owned
// immutable table, at an exact canonical virtual-machine path.
//
// A constant installs one value converted through its canonical type's
// registered writer. An enumeration installs one immutable table: the public
// table a script sees is an empty proxy whose metatable routes reads to private
// backing storage, refuses every supported write with one deterministic
// immutable-value error, and hides itself from `getmetatable` and
// `setmetatable`. Nothing a script can reach is the raw storage, so a refused
// write leaves both the table and Luna's metadata exactly as they were.
//
// Both operations run inside a protected call, leave the stack at its entry
// depth on success and on failure, and never replace a value they do not own:
// an occupied path is reported instead of overwritten, so the transaction
// journal decides what happens next.

// clang-format off
#include "state/registration/plan.hpp"

#include <string>
#include <string_view>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class TypeGeneration;

// Deterministic outcome of installing one value or immutable table.
enum class ValueInstallationStatus {
  Installed,
  // The parent table of the path does not exist.
  ParentUnavailable,
  // The exact path already holds a value; Luna never replaces it here.
  PathOccupied,
  // The canonical type has no available writer in the captured generation, or
  // the writer refused the staged value.
  ConversionRefused,
  ProtectedFailure,
  StackCapacityFailure
};

[[nodiscard]] std::string_view
ValueInstallationStatusText(ValueInstallationStatus Status) noexcept;

// Installs one converted value at its exact canonical path.
[[nodiscard]] ValueInstallationStatus
InstallValueAtVmPath(lua_State *State, const std::string &Path,
                     const TypeGeneration &Types,
                     const PlannedValue &Planned) noexcept;

// Installs one Luna-owned immutable table at its exact canonical path. Every
// field is converted through the table's canonical type before the proxy is
// published, so a refused field publishes nothing at all.
[[nodiscard]] ValueInstallationStatus
InstallImmutableTableAtVmPath(lua_State *State, const std::string &Path,
                              const TypeGeneration &Types,
                              const PlannedValueTable &Planned) noexcept;

} // namespace Luna::Detail
