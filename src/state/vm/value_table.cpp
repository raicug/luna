// clang-format off
#include "state/vm/value_table.hpp"

#include "state/registration/plan.hpp"
#include "state/type/structural_types.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/type_generation.hpp"
#include "state/vm/namespace_table.hpp"
#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

// The metatable field that hides a Luna metatable from `getmetatable` and makes
// `setmetatable` refuse: a script can neither read the private backing storage
// through the metatable nor replace the guard that protects it.
constexpr const char *ProtectedMetatableMarker = "Luna immutable value";

struct ValueRequest final {
  const std::vector<std::string> *Segments = nullptr;
  const TypeGeneration *Types = nullptr;
  const std::string *Path = nullptr;
  const PlannedValue *Single = nullptr;
  const PlannedValueTable *Table = nullptr;
  ValueInstallationStatus *Status = nullptr;
};

[[nodiscard]] int RaiseLiteral(lua_State *State, const char *Message) {
  lua_pushstring(State, Message);
  lua_error(State);
  return 0;
}

// Every supported write to a Luna immutable table fails here, before anything
// is stored: the proxy is always empty, so `__newindex` runs for an existing
// field and for a new one alike.
[[nodiscard]] int RejectImmutableWrite(lua_State *State) {
  const char *Path = lua_tostring(State, lua_upvalueindex(1));
  const char *Field = lua_type(State, 2) == LUA_TSTRING
                          ? lua_tostring(State, 2)
                          : lua_typename(State, lua_type(State, 2));
  lua_pushfstringL(State,
                   "Luna: '%s' is an immutable Luna value; the field '%s' "
                   "cannot be assigned.",
                   Path ? Path : "<unknown>", Field ? Field : "<unknown>");
  lua_error(State);
  return 0;
}

// Pushes one staged value converted through its canonical type. On refusal
// nothing is left on the stack above `Checkpoint`.
[[nodiscard]] bool PushConvertedValue(lua_State *State,
                                      const TypeGeneration &Types,
                                      const TypeDescriptor &Type,
                                      const StructuredValue &Staged,
                                      int Checkpoint) {
  const StructuredWriteResult Written =
      WriteStructuredValue(Types, State, Type, Staged);
  if (!Written.IsSuccess() || Written.PublishedCount != 1) {
    lua_settop(State, Checkpoint);
    return false;
  }
  return true;
}

// True when the exact path is free. The observed value is popped either way, so
// the container stays on top.
[[nodiscard]] bool PathIsFree(lua_State *State,
                              const std::vector<std::string> &Segments) {
  PushVmPathField(State, Segments);
  const bool Free = lua_isnil(State, -1);
  lua_pop(State, 1);
  return Free;
}

[[nodiscard]] int InstallValue(lua_State *State) {
  auto *Request = static_cast<ValueRequest *>(lua_tolightuserdata(State, 1));
  if (!Request || !Request->Segments || !Request->Types || !Request->Single ||
      !Request->Status)
    return RaiseLiteral(State,
                        "Internal error: invalid value install request.");

  const int Checkpoint = lua_gettop(State);
  if (!PushVmPathContainer(State, *Request->Segments)) {
    *Request->Status = ValueInstallationStatus::ParentUnavailable;
    return 0;
  }
  if (!PathIsFree(State, *Request->Segments)) {
    // Luna never replaces a value it does not own; the transaction reports the
    // collision instead.
    lua_settop(State, Checkpoint);
    *Request->Status = ValueInstallationStatus::PathOccupied;
    return 0;
  }

  if (!PushConvertedValue(State, *Request->Types, Request->Single->Type,
                          Request->Single->Staged, Checkpoint)) {
    *Request->Status = ValueInstallationStatus::ConversionRefused;
    return 0;
  }

  SetVmPathField(State, *Request->Segments);
  lua_settop(State, Checkpoint);
  *Request->Status = ValueInstallationStatus::Installed;
  return 0;
}

[[nodiscard]] int InstallImmutableTable(lua_State *State) {
  auto *Request = static_cast<ValueRequest *>(lua_tolightuserdata(State, 1));
  if (!Request || !Request->Segments || !Request->Types || !Request->Table ||
      !Request->Path || !Request->Status)
    return RaiseLiteral(State,
                        "Internal error: invalid table install request.");

  const int Checkpoint = lua_gettop(State);
  if (!PushVmPathContainer(State, *Request->Segments)) {
    *Request->Status = ValueInstallationStatus::ParentUnavailable;
    return 0;
  }
  if (!PathIsFree(State, *Request->Segments)) {
    lua_settop(State, Checkpoint);
    *Request->Status = ValueInstallationStatus::PathOccupied;
    return 0;
  }

  // The private backing storage. It is never published: only the proxy's
  // metatable refers to it, and that metatable is itself protected.
  lua_newtable(State);
  const int Backing = lua_gettop(State);
  for (const PlannedValueField &Field : Request->Table->Fields) {
    if (!PushConvertedValue(State, *Request->Types, Request->Table->Type,
                            Field.Staged, Checkpoint)) {
      *Request->Status = ValueInstallationStatus::ConversionRefused;
      return 0;
    }
    lua_rawsetfield(State, Backing, Field.Name.c_str());
  }

  // The public proxy stays empty, so every write reaches the guard and every
  // read is routed to the backing storage.
  lua_newtable(State);
  const int Proxy = lua_gettop(State);

  lua_newtable(State);
  const int Meta = lua_gettop(State);
  lua_pushvalue(State, Backing);
  lua_rawsetfield(State, Meta, "__index");
  lua_pushstring(State, Request->Path->c_str());
  lua_pushcclosure(State, RejectImmutableWrite, "Luna.ImmutableValue", 1);
  lua_rawsetfield(State, Meta, "__newindex");
  lua_pushstring(State, ProtectedMetatableMarker);
  lua_rawsetfield(State, Meta, "__metatable");
  lua_setmetatable(State, Proxy);

  // The container must sit directly below the published proxy.
  lua_remove(State, Backing);
  SetVmPathField(State, *Request->Segments);
  lua_settop(State, Checkpoint);
  *Request->Status = ValueInstallationStatus::Installed;
  return 0;
}

// The protected budget one installation needs: the container chain, the staged
// value or the backing, proxy, and metatable triple, and the protected call.
[[nodiscard]] bool ReserveStack(lua_State *State, std::size_t Segments) {
  return lua_checkstack(State, static_cast<int>(Segments) + 12);
}

[[nodiscard]] ValueInstallationStatus CallProtected(lua_State *State,
                                                    lua_CFunction Function,
                                                    const char *Debug,
                                                    ValueRequest &Request) {
  ValueInstallationStatus Status = ValueInstallationStatus::ProtectedFailure;
  Request.Status = &Status;
  lua_pushcfunction(State, Function, Debug);
  lua_pushlightuserdata(State, &Request);
  if (lua_pcall(State, 1, 0, 0) != LUA_OK)
    return ValueInstallationStatus::ProtectedFailure;
  return Status;
}

} // namespace

std::string_view
ValueInstallationStatusText(ValueInstallationStatus Status) noexcept {
  switch (Status) {
  case ValueInstallationStatus::Installed:
    return "installed";
  case ValueInstallationStatus::ParentUnavailable:
    return "parent_unavailable";
  case ValueInstallationStatus::PathOccupied:
    return "path_occupied";
  case ValueInstallationStatus::ConversionRefused:
    return "conversion_refused";
  case ValueInstallationStatus::ProtectedFailure:
    return "protected_failure";
  case ValueInstallationStatus::StackCapacityFailure:
    return "stack_capacity_failure";
  }
  return "unknown";
}

ValueInstallationStatus
InstallValueAtVmPath(lua_State *State, const std::string &Path,
                     const TypeGeneration &Types,
                     const PlannedValue &Planned) noexcept {
  if (!State || Path.empty())
    return ValueInstallationStatus::ProtectedFailure;

  const std::vector<std::string> Segments = SplitVmPath(Path);
  if (Segments.empty())
    return ValueInstallationStatus::ProtectedFailure;

  StackCheckpoint Checkpoint(State);
  if (!ReserveStack(State, Segments.size()))
    return ValueInstallationStatus::StackCapacityFailure;

  ValueRequest Request;
  Request.Segments = &Segments;
  Request.Types = &Types;
  Request.Path = &Path;
  Request.Single = &Planned;
  return CallProtected(State, InstallValue, "Luna.InstallConstantValue",
                       Request);
}

ValueInstallationStatus
InstallImmutableTableAtVmPath(lua_State *State, const std::string &Path,
                              const TypeGeneration &Types,
                              const PlannedValueTable &Planned) noexcept {
  if (!State || Path.empty())
    return ValueInstallationStatus::ProtectedFailure;

  const std::vector<std::string> Segments = SplitVmPath(Path);
  if (Segments.empty())
    return ValueInstallationStatus::ProtectedFailure;

  StackCheckpoint Checkpoint(State);
  if (!ReserveStack(State, Segments.size()))
    return ValueInstallationStatus::StackCapacityFailure;

  ValueRequest Request;
  Request.Segments = &Segments;
  Request.Types = &Types;
  Request.Path = &Path;
  Request.Table = &Planned;
  return CallProtected(State, InstallImmutableTable,
                       "Luna.InstallImmutableTable", Request);
}

} // namespace Luna::Detail
