// clang-format off
#include "state/vm/saved_value.hpp"

#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>

#include <string>
#include <string_view>
// clang-format on

namespace Luna::Detail {
namespace {

struct CaptureRequest final {
  const char *Path = nullptr;
  SavedVmValue *Saved = nullptr;
};

struct RestoreRequest final {
  const char *Path = nullptr;
  int Reference = 0;
};

[[nodiscard]] int RaiseLiteral(lua_State *State, const char *Message) {
  lua_pushstring(State, Message);
  lua_error(State);
  return 0;
}

// Reading a path may run a metamethod and taking a protected reference may
// allocate, so the capture itself runs inside a protected call.
[[nodiscard]] int CaptureValue(lua_State *State) {
  auto *Request = static_cast<CaptureRequest *>(lua_tolightuserdata(State, 1));
  if (!Request || !Request->Path || !Request->Saved)
    return RaiseLiteral(State, "Internal error: invalid capture request.");

  lua_getglobal(State, Request->Path);
  Request->Saved->Kind = ClassifyVmValue(State, -1);
  Request->Saved->Reference = lua_ref(State, -1);
  Request->Saved->IsCaptured = true;
  lua_pop(State, 1);
  return 0;
}

[[nodiscard]] int RestoreValue(lua_State *State) {
  auto *Request = static_cast<RestoreRequest *>(lua_tolightuserdata(State, 1));
  if (!Request || !Request->Path)
    return RaiseLiteral(State, "Internal error: invalid restore request.");

  // The nil reference pushes nil, which restores the absence of the path
  // exactly rather than leaving a stale value behind.
  lua_getref(State, Request->Reference);
  lua_setglobal(State, Request->Path);
  return 0;
}

} // namespace

std::string_view VmValueKindText(VmValueKind Kind) noexcept {
  switch (Kind) {
  case VmValueKind::Absent:
    return "absent";
  case VmValueKind::Boolean:
    return "boolean";
  case VmValueKind::Number:
    return "number";
  case VmValueKind::String:
    return "string";
  case VmValueKind::Table:
    return "table";
  case VmValueKind::Function:
    return "function";
  case VmValueKind::Userdata:
    return "userdata";
  case VmValueKind::Thread:
    return "thread";
  case VmValueKind::Other:
    return "other";
  }
  return "unknown";
}

VmValueKind ClassifyVmValue(lua_State *State, int Index) noexcept {
  switch (lua_type(State, Index)) {
  case LUA_TNIL:
  case LUA_TNONE:
    return VmValueKind::Absent;
  case LUA_TBOOLEAN:
    return VmValueKind::Boolean;
  case LUA_TNUMBER:
    return VmValueKind::Number;
  case LUA_TSTRING:
    return VmValueKind::String;
  case LUA_TTABLE:
    return VmValueKind::Table;
  case LUA_TFUNCTION:
    return VmValueKind::Function;
  case LUA_TUSERDATA:
  case LUA_TLIGHTUSERDATA:
    return VmValueKind::Userdata;
  case LUA_TTHREAD:
    return VmValueKind::Thread;
  default:
    return VmValueKind::Other;
  }
}

bool CaptureGlobalValue(lua_State *State, const std::string &Path,
                        SavedVmValue &Saved) noexcept {
  Saved = SavedVmValue();
  if (!State || Path.empty())
    return false;

  StackCheckpoint Checkpoint(State);
  if (!lua_checkstack(State, 4))
    return false;

  CaptureRequest Request{Path.c_str(), &Saved};
  lua_pushcfunction(State, CaptureValue, "Luna.CaptureVirtualMachinePath");
  lua_pushlightuserdata(State, &Request);
  if (lua_pcall(State, 1, 0, 0) != LUA_OK) {
    Saved = SavedVmValue();
    return false;
  }
  return Saved.IsCaptured;
}

bool RestoreGlobalValue(lua_State *State, const std::string &Path,
                        const SavedVmValue &Saved) noexcept {
  if (!State || Path.empty() || !Saved.IsCaptured)
    return false;

  StackCheckpoint Checkpoint(State);
  if (!lua_checkstack(State, 4))
    return false;

  RestoreRequest Request{Path.c_str(), Saved.Reference};
  lua_pushcfunction(State, RestoreValue, "Luna.RestoreVirtualMachinePath");
  lua_pushlightuserdata(State, &Request);
  return lua_pcall(State, 1, 0, 0) == LUA_OK;
}

void ReleaseSavedValue(lua_State *State, SavedVmValue &Saved) noexcept {
  if (State && Saved.IsCaptured)
    lua_unref(State, Saved.Reference);

  // The recorded kind survives the release so a finished journal still reports
  // what each path held; only the protected reference is given up.
  Saved.Reference = 0;
  Saved.IsCaptured = false;
}

} // namespace Luna::Detail
