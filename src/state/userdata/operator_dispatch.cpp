// clang-format off
#include "state/userdata/operator_dispatch.hpp"

#include "state/userdata/class_operators.hpp"

#include <lua.h>

#include <span>
#include <string>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr int ClassTableUpvalue = 1;
constexpr int SegmentUpvalue = 2;
constexpr int ForwardedUpvalue = 3;
constexpr int ResultUpvalue = 4;

// Forwards every argument the call site supplied, however many that is.
constexpr int ForwardsEveryArgument = -1;

[[nodiscard]] int RaiseOperatorRefusal(lua_State *State) {
  static constexpr char Refusal[] =
      "Luna: the operator of this class is no longer published.";
  lua_pushlstring(State, Refusal, sizeof(Refusal) - 1);
  lua_error(State);
  return 0;
}

// One operator metamethod. It holds the class table and the Luna-owned segment
// of its operator rather than any Luna record, so a later registration can move
// every record without invalidating an installed metamethod.
[[nodiscard]] int ForwardClassOperator(lua_State *State) {
  if (State == nullptr)
    return 0;

  const int Supplied = lua_gettop(State);
  const int Declared = static_cast<int>(
      lua_tointeger(State, lua_upvalueindex(ForwardedUpvalue)));
  const int Results =
      static_cast<int>(lua_tointeger(State, lua_upvalueindex(ResultUpvalue)));
  const int ResultBase = Supplied;

  // The virtual machine decides how many values an operator metamethod is
  // entered with, and that count is not always the operand count of the
  // operator. Only the operands the declaration names are forwarded.
  int Forwarded = Declared == ForwardsEveryArgument ? Supplied : Declared;
  if (Forwarded > Supplied)
    Forwarded = Supplied;
  if (Forwarded < 0)
    Forwarded = 0;

  if (!lua_checkstack(State, Forwarded + 3))
    return RaiseOperatorRefusal(State);

  lua_pushvalue(State, lua_upvalueindex(ClassTableUpvalue));
  lua_pushvalue(State, lua_upvalueindex(SegmentUpvalue));
  lua_rawget(State, -2);
  lua_remove(State, -2);
  if (!lua_isfunction(State, -1)) {
    lua_pop(State, 1);
    return RaiseOperatorRefusal(State);
  }

  for (int Index = 1; Index <= Forwarded; ++Index)
    lua_pushvalue(State, Index);

  // The candidate raises its own deterministic diagnostic when it refuses, and
  // that refusal already restored its own callback checkpoint. A call operator
  // keeps the ordinary callable's zero/one/many return shape; every other
  // operator has the fixed result count its descriptor declares.
  lua_call(State, Forwarded, Results);
  if (Results == LUA_MULTRET)
    return lua_gettop(State) - ResultBase;
  return Results;
}

} // namespace

bool InstallClassOperatorDispatch(
    lua_State *State, int MetatableIndex, int ClassTableIndex,
    std::span<const RegisteredOperator> Operators) {
  if (State == nullptr)
    return false;
  if (!lua_checkstack(State, 8))
    return false;

  for (const RegisteredOperator &Published : Operators) {
    const ClassOperatorDescriptor *Described =
        FindClassOperator(Published.Selected);
    if (Described == nullptr || Described->Metamethod.empty())
      continue;

    const int Forwarded = Described->ForwardsEveryArgument
                              ? ForwardsEveryArgument
                              : static_cast<int>(Described->OperandCount) + 1;
    const int Results = Described->ForwardsEveryArgument
                            ? LUA_MULTRET
                            : (Described->ProducesValue ? 1 : 0);
    const std::string Metamethod(Described->Metamethod);

    lua_pushvalue(State, ClassTableIndex);
    lua_pushstring(State, Published.Segment.c_str());
    lua_pushinteger(State, Forwarded);
    lua_pushinteger(State, Results);
    lua_pushcclosure(State, ForwardClassOperator, "Luna.ClassOperator", 4);
    lua_rawsetfield(State, MetatableIndex, Metamethod.c_str());
  }
  return true;
}

} // namespace Luna::Detail
