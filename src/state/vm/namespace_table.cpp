// clang-format off
#include "state/vm/namespace_table.hpp"

#include "state/vm/saved_value.hpp"
#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr char PathSeparator = '.';

struct PathRequest final {
  const std::vector<std::string> *Segments = nullptr;
  SavedVmValue *Saved = nullptr;
  VmPathObservation *Observation = nullptr;
  NamespaceTableInstallation *Installation = nullptr;
  int Reference = 0;
};

[[nodiscard]] int RaiseLiteral(lua_State *State, const char *Message) {
  lua_pushstring(State, Message);
  lua_error(State);
  return 0;
}

[[nodiscard]] int ObservePath(lua_State *State) {
  auto *Request = static_cast<PathRequest *>(lua_tolightuserdata(State, 1));
  if (!Request || !Request->Segments || !Request->Observation)
    return RaiseLiteral(State,
                        "Internal error: invalid path observation request.");

  if (!PushVmPathContainer(State, *Request->Segments))
    return 0;

  PushVmPathField(State, *Request->Segments);
  Request->Observation->Kind = ClassifyVmValue(State, -1);
  if (lua_istable(State, -1))
    Request->Observation->Table = lua_topointer(State, -1);
  lua_pop(State, 2);
  return 0;
}

[[nodiscard]] int CapturePath(lua_State *State) {
  auto *Request = static_cast<PathRequest *>(lua_tolightuserdata(State, 1));
  if (!Request || !Request->Segments || !Request->Saved)
    return RaiseLiteral(State, "Internal error: invalid path capture request.");

  if (!PushVmPathContainer(State, *Request->Segments)) {
    Request->Saved->Kind = VmValueKind::Absent;
    Request->Saved->Reference = LUA_REFNIL;
    Request->Saved->IsCaptured = true;
    return 0;
  }

  PushVmPathField(State, *Request->Segments);
  Request->Saved->Kind = ClassifyVmValue(State, -1);
  Request->Saved->Reference = lua_ref(State, -1);
  Request->Saved->IsCaptured = true;
  lua_pop(State, 2);
  return 0;
}

[[nodiscard]] int RestorePath(lua_State *State) {
  auto *Request = static_cast<PathRequest *>(lua_tolightuserdata(State, 1));
  if (!Request || !Request->Segments)
    return RaiseLiteral(State, "Internal error: invalid path restore request.");

  if (!PushVmPathContainer(State, *Request->Segments))
    return 0;

  lua_getref(State, Request->Reference);
  SetVmPathField(State, *Request->Segments);
  lua_pop(State, 1);
  return 0;
}

[[nodiscard]] int ClearPath(lua_State *State) {
  auto *Request = static_cast<PathRequest *>(lua_tolightuserdata(State, 1));
  if (!Request || !Request->Segments)
    return RaiseLiteral(State, "Internal error: invalid path clear request.");

  if (!PushVmPathContainer(State, *Request->Segments))
    return 0;

  lua_pushnil(State);
  SetVmPathField(State, *Request->Segments);
  lua_pop(State, 1);
  return 0;
}
[[nodiscard]] int InstallTable(lua_State *State) {
  auto *Request = static_cast<PathRequest *>(lua_tolightuserdata(State, 1));
  if (!Request || !Request->Segments || !Request->Installation)
    return RaiseLiteral(State,
                        "Internal error: invalid namespace install request.");

  NamespaceTableInstallation &Installation = *Request->Installation;
  if (!PushVmPathContainer(State, *Request->Segments)) {
    Installation.Status = NamespaceTableStatus::ParentUnavailable;
    return 0;
  }

  PushVmPathField(State, *Request->Segments);
  if (lua_istable(State, -1)) {
    Installation.Status = NamespaceTableStatus::Reopened;
    Installation.Table = lua_topointer(State, -1);
    Installation.Reference = lua_ref(State, -1);
    lua_pop(State, 2);
    return 0;
  }
  if (!lua_isnil(State, -1)) {
    Installation.Status = NamespaceTableStatus::PathOccupied;
    lua_pop(State, 2);
    return 0;
  }

  lua_pop(State, 1);
  lua_newtable(State);
  Installation.Table = lua_topointer(State, -1);
  Installation.Reference = lua_ref(State, -1);
  SetVmPathField(State, *Request->Segments);
  lua_pop(State, 1);
  Installation.Status = NamespaceTableStatus::Created;
  return 0;
}

[[nodiscard]] int RetainTable(lua_State *State) {
  auto *Request = static_cast<PathRequest *>(lua_tolightuserdata(State, 1));
  if (!Request || !Request->Segments || !Request->Installation)
    return RaiseLiteral(State,
                        "Internal error: invalid namespace retain request.");

  NamespaceTableInstallation &Installation = *Request->Installation;
  if (!PushVmPathContainer(State, *Request->Segments)) {
    Installation.Status = NamespaceTableStatus::ParentUnavailable;
    return 0;
  }

  PushVmPathField(State, *Request->Segments);
  if (!lua_istable(State, -1)) {
    Installation.Status = NamespaceTableStatus::PathOccupied;
    lua_pop(State, 2);
    return 0;
  }

  Installation.Status = NamespaceTableStatus::Reopened;
  Installation.Table = lua_topointer(State, -1);
  Installation.Reference = lua_ref(State, -1);
  lua_pop(State, 2);
  return 0;
}

[[nodiscard]] bool ReserveStack(lua_State *State, std::size_t Segments) {
  return lua_checkstack(State, static_cast<int>(Segments) + 8);
}

[[nodiscard]] bool CallProtected(lua_State *State, lua_CFunction Function,
                                 const char *Debug, PathRequest &Request) {
  lua_pushcfunction(State, Function, Debug);
  lua_pushlightuserdata(State, &Request);
  return lua_pcall(State, 1, 0, 0) == LUA_OK;
}

} // namespace

bool PushVmPathContainer(lua_State *State,
                         const std::vector<std::string> &Segments) noexcept {
  lua_pushvalue(State, LUA_GLOBALSINDEX);
  for (std::size_t Index = 0; Index + 1 < Segments.size(); ++Index) {
    if (Index == 0)
      lua_getfield(State, -1, Segments[Index].c_str());
    else
      lua_rawgetfield(State, -1, Segments[Index].c_str());
    lua_remove(State, -2);
    if (!lua_istable(State, -1)) {
      lua_pop(State, 1);
      return false;
    }
  }
  return true;
}

void PushVmPathField(lua_State *State,
                     const std::vector<std::string> &Segments) noexcept {
  const std::string &Final = Segments.back();
  if (Segments.size() == 1)
    lua_getfield(State, -1, Final.c_str());
  else
    lua_rawgetfield(State, -1, Final.c_str());
}

void SetVmPathField(lua_State *State,
                    const std::vector<std::string> &Segments) noexcept {
  const std::string &Final = Segments.back();
  if (Segments.size() == 1)
    lua_setfield(State, -2, Final.c_str());
  else
    lua_rawsetfield(State, -2, Final.c_str());
}

bool IsNestedVmPath(std::string_view Path) noexcept {
  return Path.find(PathSeparator) != std::string_view::npos;
}

std::vector<std::string> SplitVmPath(std::string_view Path) {
  std::vector<std::string> Segments;
  if (Path.empty())
    return Segments;

  std::size_t Start = 0;
  while (true) {
    const std::size_t Separator = Path.find(PathSeparator, Start);
    if (Separator == std::string_view::npos) {
      Segments.emplace_back(Path.substr(Start));
      return Segments;
    }
    Segments.emplace_back(Path.substr(Start, Separator - Start));
    Start = Separator + 1;
  }
}

std::string_view
NamespaceTableStatusText(NamespaceTableStatus Status) noexcept {
  switch (Status) {
  case NamespaceTableStatus::Created:
    return "created";
  case NamespaceTableStatus::Reopened:
    return "reopened";
  case NamespaceTableStatus::ParentUnavailable:
    return "parent_unavailable";
  case NamespaceTableStatus::PathOccupied:
    return "path_occupied";
  case NamespaceTableStatus::ProtectedFailure:
    return "protected_failure";
  case NamespaceTableStatus::StackCapacityFailure:
    return "stack_capacity_failure";
  }
  return "unknown";
}

VmPathObservation ObserveVmPath(lua_State *State,
                                const std::string &Path) noexcept {
  VmPathObservation Observation;
  if (!State || Path.empty())
    return Observation;

  const std::vector<std::string> Segments = SplitVmPath(Path);
  if (Segments.empty())
    return Observation;

  StackCheckpoint Checkpoint(State);
  if (!ReserveStack(State, Segments.size()))
    return Observation;

  PathRequest Request;
  Request.Segments = &Segments;
  Request.Observation = &Observation;
  if (!CallProtected(State, ObservePath, "Luna.ObserveVirtualMachinePath",
                     Request))
    return VmPathObservation();
  return Observation;
}

bool CaptureVmPathValue(lua_State *State, const std::string &Path,
                        SavedVmValue &Saved) noexcept {
  Saved = SavedVmValue();
  if (!State || Path.empty())
    return false;

  const std::vector<std::string> Segments = SplitVmPath(Path);
  if (Segments.empty())
    return false;

  StackCheckpoint Checkpoint(State);
  if (!ReserveStack(State, Segments.size()))
    return false;

  PathRequest Request;
  Request.Segments = &Segments;
  Request.Saved = &Saved;
  if (!CallProtected(State, CapturePath, "Luna.CaptureVirtualMachinePath",
                     Request)) {
    Saved = SavedVmValue();
    return false;
  }
  return Saved.IsCaptured;
}

bool RestoreVmPathValue(lua_State *State, const std::string &Path,
                        const SavedVmValue &Saved) noexcept {
  if (!State || Path.empty() || !Saved.IsCaptured)
    return false;

  const std::vector<std::string> Segments = SplitVmPath(Path);
  if (Segments.empty())
    return false;

  StackCheckpoint Checkpoint(State);
  if (!ReserveStack(State, Segments.size()))
    return false;

  PathRequest Request;
  Request.Segments = &Segments;
  Request.Reference = Saved.Reference;
  return CallProtected(State, RestorePath, "Luna.RestoreVirtualMachinePath",
                       Request);
}

bool ClearVmPathValue(lua_State *State, const std::string &Path) noexcept {
  if (!State || Path.empty())
    return false;

  const std::vector<std::string> Segments = SplitVmPath(Path);
  if (Segments.empty())
    return false;

  StackCheckpoint Checkpoint(State);
  if (!ReserveStack(State, Segments.size()))
    return false;

  PathRequest Request;
  Request.Segments = &Segments;
  return CallProtected(State, ClearPath, "Luna.ClearVirtualMachinePath",
                       Request);
}
NamespaceTableInstallation
InstallNamespaceTable(lua_State *State, const std::string &Path) noexcept {
  NamespaceTableInstallation Installation;
  if (!State || Path.empty())
    return Installation;

  const std::vector<std::string> Segments = SplitVmPath(Path);
  if (Segments.empty())
    return Installation;

  StackCheckpoint Checkpoint(State);
  if (!ReserveStack(State, Segments.size())) {
    Installation.Status = NamespaceTableStatus::StackCapacityFailure;
    return Installation;
  }

  PathRequest Request;
  Request.Segments = &Segments;
  Request.Installation = &Installation;
  if (!CallProtected(State, InstallTable, "Luna.InstallNamespaceTable",
                     Request))
    return NamespaceTableInstallation();
  return Installation;
}

NamespaceTableInstallation
RetainNamespaceTable(lua_State *State, const std::string &Path) noexcept {
  NamespaceTableInstallation Installation;
  if (!State || Path.empty())
    return Installation;

  const std::vector<std::string> Segments = SplitVmPath(Path);
  if (Segments.empty())
    return Installation;

  StackCheckpoint Checkpoint(State);
  if (!ReserveStack(State, Segments.size())) {
    Installation.Status = NamespaceTableStatus::StackCapacityFailure;
    return Installation;
  }

  PathRequest Request;
  Request.Segments = &Segments;
  Request.Installation = &Installation;
  if (!CallProtected(State, RetainTable, "Luna.RetainNamespaceTable", Request))
    return NamespaceTableInstallation();
  return Installation;
}

void ReleaseNamespaceTable(lua_State *State, int Reference) noexcept {
  if (State && Reference != LUA_REFNIL)
    lua_unref(State, Reference);
}

} // namespace Luna::Detail
