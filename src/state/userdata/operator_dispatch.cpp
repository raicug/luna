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

constexpr int ForwardsEveryArgument = -1;

[[nodiscard]] int RaiseOperatorRefusal(lua_State *State) {
  static constexpr char Refusal[] =
      "Luna: the operator of this class is no longer published.";
  lua_pushlstring(State, Refusal, sizeof(Refusal) - 1);
  lua_error(State);
  return 0;
}

[[nodiscard]] int ForwardClassOperator(lua_State *State) {
  if (State == nullptr)
    return 0;

  const int Supplied = lua_gettop(State);
  const int Declared = static_cast<int>(
      lua_tointeger(State, lua_upvalueindex(ForwardedUpvalue)));
  const int Results =
      static_cast<int>(lua_tointeger(State, lua_upvalueindex(ResultUpvalue)));
  const int ResultBase = Supplied;

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

  lua_call(State, Forwarded, Results);
  if (Results == LUA_MULTRET)
    return lua_gettop(State) - ResultBase;
  return Results;
}

// Luau asks `__iter` for the loop's step function, its state, and its first
// control value. Luna answers with the declared iteration step, the receiver
// the loop is iterating, and no control value, so the first step observes an
// omitted control exactly the way its declared optional operand describes.
[[nodiscard]] int PublishClassIterator(lua_State *State) {
  if (State == nullptr)
    return 0;
  if (lua_gettop(State) < 1 || !lua_checkstack(State, 4))
    return RaiseOperatorRefusal(State);

  lua_pushvalue(State, lua_upvalueindex(1));
  lua_pushvalue(State, 1);
  lua_pushnil(State);
  return 3;
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
    const int Results =
        Described->ForwardsEveryArgument || Described->PublishesPack
            ? LUA_MULTRET
            : (Described->ProducesValue ? 1 : 0);
    const std::string Metamethod(Described->Metamethod);

    lua_pushvalue(State, ClassTableIndex);
    lua_pushstring(State, Published.Segment.c_str());
    lua_pushinteger(State, Forwarded);
    lua_pushinteger(State, Results);
    lua_pushcclosure(State, ForwardClassOperator, "Luna.ClassOperator", 4);
    if (Described->PublishesIterator)
      lua_pushcclosure(State, PublishClassIterator, "Luna.ClassIterator", 1);
    lua_rawsetfield(State, MetatableIndex, Metamethod.c_str());
  }
  return true;
}

} // namespace Luna::Detail
