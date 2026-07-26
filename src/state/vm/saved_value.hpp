#pragma once

// The exact prior contents of one canonical virtual-machine path. Before the
// installer writes anything, it captures what the path held - a function, a
// table, some other value, or nothing at all - so restoration can put back
// precisely that instead of deleting the path or guessing a replacement.
//
// The captured value itself lives in a protected reference the virtual machine
// owns, so it survives garbage collection until the journal releases it.
// Capture, restoration, and release each run inside a protected call, so
// neither a Luau error nor a C++ exception crosses this boundary.

// clang-format off
#include <string>
#include <string_view>
// clang-format on

struct lua_State;

namespace Luna::Detail {

// Exact category of the value a canonical path held when it was captured.
// `Absent` is the real "no value here" case: a Luau path that holds nil is
// indistinguishable from one that was never assigned, and restoration
// reproduces that absence rather than writing a placeholder.
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

// The exact category of the value at one stack index. Every path operation
// classifies through this one function, so absence and every value kind are
// reported identically no matter which path shape produced them.
[[nodiscard]] VmValueKind ClassifyVmValue(lua_State *State, int Index) noexcept;

struct SavedVmValue final {
  VmValueKind Kind = VmValueKind::Absent;

  // Protected reference to the captured value, or the nil reference when the
  // path was absent.
  int Reference = 0;

  // The capture itself succeeded. A journal entry without a successful capture
  // is never installed over.
  bool IsCaptured = false;

  [[nodiscard]] bool Existed() const noexcept {
    return Kind != VmValueKind::Absent;
  }
};

// Captures the exact current value of one canonical global path, including its
// absence. The stack depth is unchanged on both success and failure.
[[nodiscard]] bool CaptureGlobalValue(lua_State *State, const std::string &Path,
                                      SavedVmValue &Saved) noexcept;

// Writes the captured value back to its path, restoring absence when the path
// held nothing. The stack depth is unchanged on both success and failure.
[[nodiscard]] bool RestoreGlobalValue(lua_State *State, const std::string &Path,
                                      const SavedVmValue &Saved) noexcept;

// Releases the protected reference of a captured value while keeping the
// recorded kind, so a finished journal still reports what each path held.
// Releasing twice, or releasing a value that was never captured, is harmless.
void ReleaseSavedValue(lua_State *State, SavedVmValue &Saved) noexcept;

} // namespace Luna::Detail
