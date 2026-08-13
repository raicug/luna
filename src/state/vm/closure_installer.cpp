// clang-format off
#include "state/vm/closure_installer.hpp"

#include "state/dispatch/closure_slot.hpp"
#include "state/registration/record.hpp"
#include "state/invocation/trampoline.hpp"
#include "state/vm/namespace_table.hpp"
#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>

#include <cstddef>
#include <string>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

struct InstallationRequest final {
  DispatchSlotId Slot;
  const DispatchTable *Dispatch = nullptr;
  const char *GlobalName = nullptr;
  const std::vector<std::string> *Segments = nullptr;
  bool InjectFailure = false;
};

struct ObservationRequest final {
  const std::vector<std::string> *Segments = nullptr;
  DispatchSlotId *Found = nullptr;
};

[[nodiscard]] int RaiseLiteral(lua_State *State, const char *Message) {
  lua_pushstring(State, Message);
  lua_error(State);
  return 0;
}

[[nodiscard]] int InstallClosure(lua_State *State) {
  auto *Request =
      static_cast<InstallationRequest *>(lua_tolightuserdata(State, 1));
  if (!Request || !Request->Slot.IsValid() || !Request->Dispatch ||
      !Request->GlobalName)
    return RaiseLiteral(State, "Internal error: invalid installation request.");

  PushDispatchSlot(State, Request->Slot);
  lua_pushlightuserdata(State, const_cast<DispatchTable *>(Request->Dispatch));
  lua_pushcclosurek(State, NativeTrampoline, Request->GlobalName, 2,
                    NativeTrampolineContinuation);
  lua_setglobal(State, Request->GlobalName);

  if (Request->InjectFailure)
    return RaiseLiteral(State, "Injected binding installation failure.");
  return 0;
}

[[nodiscard]] int InstallScopedClosure(lua_State *State) {
  auto *Request =
      static_cast<InstallationRequest *>(lua_tolightuserdata(State, 1));
  if (!Request || !Request->Slot.IsValid() || !Request->Dispatch ||
      !Request->GlobalName || !Request->Segments)
    return RaiseLiteral(State, "Internal error: invalid installation request.");

  const int Checkpoint = lua_gettop(State);
  if (!PushVmPathContainer(State, *Request->Segments)) {
    lua_settop(State, Checkpoint);
    return RaiseLiteral(
        State,
        "Internal error: the parent scope of a callable is unavailable.");
  }

  PushDispatchSlot(State, Request->Slot);
  lua_pushlightuserdata(State, const_cast<DispatchTable *>(Request->Dispatch));
  lua_pushcclosurek(State, NativeTrampoline, Request->GlobalName, 2,
                    NativeTrampolineContinuation);
  SetVmPathField(State, *Request->Segments);
  lua_settop(State, Checkpoint);

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

[[nodiscard]] int ObserveScopedClosure(lua_State *State) {
  auto *Request =
      static_cast<ObservationRequest *>(lua_tolightuserdata(State, 1));
  if (!Request || !Request->Segments || !Request->Found)
    return RaiseLiteral(State, "Internal error: invalid observation request.");

  if (!PushVmPathContainer(State, *Request->Segments))
    return 0;

  PushVmPathField(State, *Request->Segments);
  if (lua_iscfunction(State, -1) && lua_getupvalue(State, -1, 1))
    *Request->Found = DispatchSlotAt(State, -1);
  return 0;
}

[[nodiscard]] bool ReserveStack(lua_State *State, std::size_t Segments) {
  return lua_checkstack(State, static_cast<int>(Segments) + 8);
}

[[nodiscard]] ClosureInstallationStatus
InstallAtRootScope(lua_State *State, BindingRecord &Record,
                   bool InjectFailure) noexcept {
  StackCheckpoint Checkpoint(State);
  if (!lua_checkstack(State, 5))
    return ClosureInstallationStatus::StackCapacityFailure;

  InstallationRequest Request{Record.Slot(), Record.Dispatch(),
                              Record.GlobalName().c_str(), nullptr,
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

[[nodiscard]] ClosureInstallationStatus
InstallAtScopedPath(lua_State *State, BindingRecord &Record,
                    bool InjectFailure) noexcept {
  const std::vector<std::string> Segments = SplitVmPath(Record.GlobalName());
  if (Segments.empty())
    return ClosureInstallationStatus::ProtectedFailure;

  StackCheckpoint Checkpoint(State);
  if (!ReserveStack(State, Segments.size()))
    return ClosureInstallationStatus::StackCapacityFailure;

  InstallationRequest Request{Record.Slot(), Record.Dispatch(),
                              Record.GlobalName().c_str(), &Segments,
                              InjectFailure};
  lua_pushcfunction(State, InstallScopedClosure,
                    "Luna.InstallScopedBindingClosure");
  lua_pushlightuserdata(State, &Request);
  if (lua_pcall(State, 1, 0, 0) == LUA_OK)
    return ClosureInstallationStatus::Success;
  return ClosureInstallationStatus::ProtectedFailure;
}

} // namespace

ClosureInstallationStatus InstallBindingClosure(lua_State *State,
                                                BindingRecord &Record,
                                                bool InjectFailure) noexcept {
  if (!State)
    return ClosureInstallationStatus::ProtectedFailure;
  if (!Record.Slot().IsValid() || !Record.Dispatch())
    return ClosureInstallationStatus::ProtectedFailure;

  if (!PublishDispatchTable(State, Record.Dispatch()))
    return ClosureInstallationStatus::StackCapacityFailure;

  if (!IsNestedVmPath(Record.GlobalName()))
    return InstallAtRootScope(State, Record, InjectFailure);
  return InstallAtScopedPath(State, Record, InjectFailure);
}

DispatchSlotId
ObserveInstalledDispatchSlot(lua_State *State,
                             const std::string &GlobalName) noexcept {
  if (!State || GlobalName.empty())
    return DispatchSlotId{};

  StackCheckpoint Checkpoint(State);

  if (!IsNestedVmPath(GlobalName)) {
    if (!lua_checkstack(State, 2))
      return DispatchSlotId{};

    lua_getglobal(State, GlobalName.c_str());
    if (!lua_iscfunction(State, -1) || !lua_getupvalue(State, -1, 1))
      return DispatchSlotId{};
    return DispatchSlotAt(State, -1);
  }

  const std::vector<std::string> Segments = SplitVmPath(GlobalName);
  if (Segments.empty() || !ReserveStack(State, Segments.size()))
    return DispatchSlotId{};

  DispatchSlotId Found;
  ObservationRequest Request{&Segments, &Found};
  lua_pushcfunction(State, ObserveScopedClosure,
                    "Luna.ObserveScopedBindingClosure");
  lua_pushlightuserdata(State, &Request);
  if (lua_pcall(State, 1, 0, 0) != LUA_OK)
    return DispatchSlotId{};
  return Found;
}

const BindingRecord *
ObserveInstalledBinding(lua_State *State,
                        const std::string &GlobalName) noexcept {
  const DispatchSlotId Slot = ObserveInstalledDispatchSlot(State, GlobalName);
  if (!Slot.IsValid())
    return nullptr;

  const DispatchTable *Table = ObserveDispatchTable(State);
  if (!Table)
    return nullptr;
  const BindingRecord *Target = Table->Resolve(Slot);
  if (!Target || Target->GlobalName() != GlobalName)
    return nullptr;
  return Target;
}

} // namespace Luna::Detail
