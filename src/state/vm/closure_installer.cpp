// clang-format off
#include "state/vm/closure_installer.hpp"

#include "state/binding/record.hpp"
#include "state/invocation/trampoline.hpp"
#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>
// clang-format on

namespace Luna::Detail {
namespace {

struct InstallationRequest final {
  BindingRecord *Record = nullptr;
  const char *GlobalName = nullptr;
  bool InjectFailure = false;
};

[[nodiscard]] int RaiseLiteral(lua_State *State, const char *Message) {
  lua_pushstring(State, Message);
  lua_error(State);
  return 0;
}

[[nodiscard]] int InstallClosure(lua_State *State) {
  auto *Request =
      static_cast<InstallationRequest *>(lua_tolightuserdata(State, 1));
  if (!Request || !Request->Record || !Request->GlobalName)
    return RaiseLiteral(State, "Internal error: invalid installation request.");

  lua_pushlightuserdata(State, Request->Record);
  lua_pushcclosure(State, NativeTrampoline, Request->GlobalName, 1);
  lua_setglobal(State, Request->GlobalName);

  if (Request->InjectFailure)
    return RaiseLiteral(State, "Injected binding installation failure.");
  return 0;
}

[[nodiscard]] int RestoreGlobal(lua_State *State) {
  auto *Request =
      static_cast<InstallationRequest *>(lua_tolightuserdata(State, 1));
  if (!Request || !Request->GlobalName)
    return RaiseLiteral(State, "Internal error: invalid rollback request.");

  lua_pushvalue(State, 2);
  lua_setglobal(State, Request->GlobalName);
  return 0;
}

} // namespace

ClosureInstallationStatus InstallBindingClosure(lua_State *State,
                                                BindingRecord &Record,
                                                bool InjectFailure) noexcept {
  if (!State)
    return ClosureInstallationStatus::ProtectedFailure;

  StackCheckpoint Checkpoint(State);
  if (!lua_checkstack(State, 5))
    return ClosureInstallationStatus::StackCapacityFailure;

  InstallationRequest Request{&Record, Record.GlobalName().c_str(),
                              InjectFailure};
  lua_getglobal(State, Request.GlobalName);
  lua_pushcfunction(State, InstallClosure, "Luna.InstallBindingClosure");
  lua_pushlightuserdata(State, &Request);

  if (lua_pcall(State, 1, 0, 0) == LUA_OK)
    return ClosureInstallationStatus::Success;

  lua_settop(State, Checkpoint.EntryDepth() + 1);
  lua_pushcfunction(State, RestoreGlobal, "Luna.RestoreBindingGlobal");
  lua_pushlightuserdata(State, &Request);
  lua_pushvalue(State, Checkpoint.EntryDepth() + 1);
  if (lua_pcall(State, 2, 0, 0) != LUA_OK)
    return ClosureInstallationStatus::RollbackFailure;

  return ClosureInstallationStatus::ProtectedFailure;
}

const BindingRecord *
ObserveInstalledBinding(lua_State *State,
                        const std::string &GlobalName) noexcept {
  if (!State)
    return nullptr;

  StackCheckpoint Checkpoint(State);
  if (!lua_checkstack(State, 2))
    return nullptr;

  lua_getglobal(State, GlobalName.c_str());
  if (!lua_iscfunction(State, -1) || !lua_getupvalue(State, -1, 1))
    return nullptr;
  return static_cast<const BindingRecord *>(lua_tolightuserdata(State, -1));
}

} // namespace Luna::Detail
