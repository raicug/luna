#pragma once

// Protected access to one exact canonical virtual-machine path, including the
// nested table paths namespaces use.
//
// A canonical path is a `.`-separated sequence of validated identifier
// segments. The first segment names a global; every later segment names a raw
// field of the table the previous segment holds. Raw field access is
// deliberate: a script-installed metamethod can neither hide what a path really
// holds nor intercept what Luna writes into a namespace table.
//
// Every operation here runs inside a protected call and leaves the stack at its
// entry depth on success and on failure, so neither a Luau error nor a C++
// exception crosses this boundary. Nothing observes or creates a table without
// reporting exactly what it found first, which is what lets the transaction
// journal restore an exact prior value or absence.

// clang-format off
#include "state/vm/saved_value.hpp"

#include <string>
#include <string_view>
#include <vector>
// clang-format on

struct lua_State;

namespace Luna::Detail {

// True when the path names a nested table field rather than one root global.
[[nodiscard]] bool IsNestedVmPath(std::string_view Path) noexcept;

// The canonical segments of one path, in order. An empty path yields no
// segments, which every operation rejects.
[[nodiscard]] std::vector<std::string> SplitVmPath(std::string_view Path);

// What one canonical path holds right now. `Table` is the identity of the table
// the path holds, and is null for every other kind, including absence.
struct VmPathObservation final {
  VmValueKind Kind = VmValueKind::Absent;
  const void *Table = nullptr;

  [[nodiscard]] bool Exists() const noexcept {
    return Kind != VmValueKind::Absent;
  }
};

// Raw path traversal, shared by every operation that installs at an exact
// canonical path. The first segment names a global; every deeper segment is a
// raw field of the table its parent segment holds, so a script-installed
// metamethod can neither hide what a path really holds nor intercept what Luna
// writes into it. Each of these runs only inside an already protected call.
[[nodiscard]] bool
PushVmPathContainer(lua_State *State,
                    const std::vector<std::string> &Segments) noexcept;
void PushVmPathField(lua_State *State,
                     const std::vector<std::string> &Segments) noexcept;
void SetVmPathField(lua_State *State,
                    const std::vector<std::string> &Segments) noexcept;

// Observes one canonical path without retaining anything.
[[nodiscard]] VmPathObservation ObserveVmPath(lua_State *State,
                                              const std::string &Path) noexcept;

// Captures the exact current value of one canonical path, including its absence
// and including a missing parent table.
[[nodiscard]] bool CaptureVmPathValue(lua_State *State, const std::string &Path,
                                      SavedVmValue &Saved) noexcept;

// Writes the captured value back to its path, restoring absence when the path
// held nothing. A path whose parent table no longer exists needs no
// restoration, because the parent's own journal entry restores it.
[[nodiscard]] bool RestoreVmPathValue(lua_State *State, const std::string &Path,
                                      const SavedVmValue &Saved) noexcept;

// Deterministic outcome of installing one namespace table.
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

  // Identity of the installed table and the protected reference that keeps it
  // alive, so its identity can never be recycled while Luna owns the namespace.
  const void *Table = nullptr;
  int Reference = 0;

  [[nodiscard]] bool IsInstalled() const noexcept {
    return Status == NamespaceTableStatus::Created ||
           Status == NamespaceTableStatus::Reopened;
  }
};

// Creates the table of one namespace at its exact path, or reports the table
// already there. The parent table is never created here: a nested namespace
// plans its parents as their own declarations, so installation creates them
// from parent to child.
[[nodiscard]] NamespaceTableInstallation
InstallNamespaceTable(lua_State *State, const std::string &Path) noexcept;

// Retains the table one canonical path holds and reports its identity, so
// publication can record which table Luna owns as that namespace.
[[nodiscard]] NamespaceTableInstallation
RetainNamespaceTable(lua_State *State, const std::string &Path) noexcept;

// Releases a protected namespace-table reference.
void ReleaseNamespaceTable(lua_State *State, int Reference) noexcept;

} // namespace Luna::Detail
