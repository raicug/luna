#pragma once

// clang-format off
#include "state/vm/saved_value.hpp"

#include <string>
#include <string_view>
#include <vector>
// clang-format on

struct lua_State;

namespace Luna::Detail {

[[nodiscard]] bool IsNestedVmPath(std::string_view Path) noexcept;

[[nodiscard]] std::vector<std::string> SplitVmPath(std::string_view Path);

struct VmPathObservation final {
  VmValueKind Kind = VmValueKind::Absent;
  const void *Table = nullptr;

  [[nodiscard]] bool Exists() const noexcept {
    return Kind != VmValueKind::Absent;
  }
};

[[nodiscard]] bool
PushVmPathContainer(lua_State *State,
                    const std::vector<std::string> &Segments) noexcept;
void PushVmPathField(lua_State *State,
                     const std::vector<std::string> &Segments) noexcept;
void SetVmPathField(lua_State *State,
                    const std::vector<std::string> &Segments) noexcept;

[[nodiscard]] VmPathObservation ObserveVmPath(lua_State *State,
                                              const std::string &Path) noexcept;

[[nodiscard]] bool CaptureVmPathValue(lua_State *State, const std::string &Path,
                                      SavedVmValue &Saved) noexcept;

[[nodiscard]] bool RestoreVmPathValue(lua_State *State, const std::string &Path,
                                      const SavedVmValue &Saved) noexcept;

enum class NamespaceTableStatus {
  Created,
  Reopened,
  ParentUnavailable,
  PathOccupied,
  ProtectedFailure,
  StackCapacityFailure
};

[[nodiscard]] std::string_view
NamespaceTableStatusText(NamespaceTableStatus Status) noexcept;

struct NamespaceTableInstallation final {
  NamespaceTableStatus Status = NamespaceTableStatus::ProtectedFailure;

  const void *Table = nullptr;
  int Reference = 0;

  [[nodiscard]] bool IsInstalled() const noexcept {
    return Status == NamespaceTableStatus::Created ||
           Status == NamespaceTableStatus::Reopened;
  }
};

[[nodiscard]] NamespaceTableInstallation
InstallNamespaceTable(lua_State *State, const std::string &Path) noexcept;

[[nodiscard]] NamespaceTableInstallation
RetainNamespaceTable(lua_State *State, const std::string &Path) noexcept;

void ReleaseNamespaceTable(lua_State *State, int Reference) noexcept;

} // namespace Luna::Detail
