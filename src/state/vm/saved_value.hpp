#pragma once

// clang-format off
#include <string>
#include <string_view>
// clang-format on

struct lua_State;

namespace Luna::Detail {

enum class VmValueKind {
  Absent,
  Boolean,
  Number,
  String,
  Table,
  Function,
  Userdata,
  Thread,
  Other
};

[[nodiscard]] std::string_view VmValueKindText(VmValueKind Kind) noexcept;

[[nodiscard]] VmValueKind ClassifyVmValue(lua_State *State, int Index) noexcept;

struct SavedVmValue final {
  VmValueKind Kind = VmValueKind::Absent;

  int Reference = 0;

  bool IsCaptured = false;

  [[nodiscard]] bool Existed() const noexcept {
    return Kind != VmValueKind::Absent;
  }
};

[[nodiscard]] bool CaptureGlobalValue(lua_State *State, const std::string &Path,
                                      SavedVmValue &Saved) noexcept;

[[nodiscard]] bool RestoreGlobalValue(lua_State *State, const std::string &Path,
                                      const SavedVmValue &Saved) noexcept;

void ReleaseSavedValue(lua_State *State, SavedVmValue &Saved) noexcept;

} // namespace Luna::Detail
