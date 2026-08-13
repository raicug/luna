#include <lua.h>
#include <lualib.h>

#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"
#include "support/harness.hpp"
#include "support/invocation_cases.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using PreparedLunaCall = Luna::Detail::PreparedNativeInvocation;
using Value = Luna::Value;

std::size_t SideEffectCount = 0;

int Touch(lua_State *) {
  ++SideEffectCount;
  return 0;
}
int Add(lua_State *State) {
  int LeftOk = 0;
  int RightOk = 0;
  const int Left = lua_tointegerx(State, 1, &LeftOk);
  const int Right = lua_tointegerx(State, 2, &RightOk);
  if (!LeftOk || !RightOk)
    return 0;
  lua_pushinteger(State, Left + Right);
  return 1;
}
int Dynamic(lua_State *State) {
  int Ok = 0;
  const int Seed = lua_tointegerx(State, 1, &Ok);
  if (!Ok)
    return 0;
  lua_pushinteger(State, Seed);
  lua_pushinteger(State, Seed + 1);
  lua_pushinteger(State, Seed + 2);
  return 3;
}
void LunaTouch() {
  ++SideEffectCount;
}
[[nodiscard]] int LunaAdd(int Left, int Right) {
  return Left + Right;
}
[[nodiscard]] Luna::ReturnPack LunaDynamic(int Seed) {
  Luna::ReturnPack Result;
  Result.AppendInteger(Seed).AppendInteger(Seed + 1).AppendInteger(Seed + 2);
  return Result;
}

struct RawCall final {
  int Reference = LUA_NOREF;
  int Returns = 0;
};

class RawModel final {
public:
  RawModel() : State(luaL_newstate()) {
    if (!State)
      return;
    luaL_openlibs(State);
    Install("Touch", &Touch, "LunaBenchmark.RawPure.Touch");
    Install("Add", &Add, "LunaBenchmark.RawPure.Add");
    Install("Dynamic", &Dynamic, "LunaBenchmark.RawPure.Dynamic");
  }
  ~RawModel() {
    if (State)
      lua_close(State);
  }
  RawModel(const RawModel &) = delete;
  [[nodiscard]] bool IsReady() const noexcept { return State != nullptr; }
  [[nodiscard]] std::optional<RawCall> Prepare(std::string_view Name,
                                               int Returns) {
    if (!State || Returns < 0 || !lua_checkstack(State, 1))
      return std::nullopt;
    const int Depth = lua_gettop(State);
    lua_getglobal(State, Name.data());
    if (!lua_iscfunction(State, -1)) {
      lua_settop(State, Depth);
      return std::nullopt;
    }
    const int Reference = lua_ref(State, -1);
    if (Reference <= LUA_REFNIL) {
      lua_settop(State, Depth);
      return std::nullopt;
    }
    return RawCall{Reference, Returns};
  }
  [[nodiscard]] bool Invoke(const RawCall &Call,
                            std::span<const Value> Arguments) {
    if (!State || Call.Reference <= LUA_REFNIL ||
        !lua_checkstack(State, static_cast<int>(Arguments.size()) + 1))
      return false;
    const int Depth = lua_gettop(State);
    lua_rawgeti(State, LUA_REGISTRYINDEX, Call.Reference);
    for (const Value &Argument : Arguments)
      Push(Argument);
    const int Status =
        lua_pcall(State, static_cast<int>(Arguments.size()), Call.Returns, 0);
    const bool Passed =
        Status == LUA_OK && lua_gettop(State) == Depth + Call.Returns;
    lua_settop(State, Depth);
    return Passed;
  }

private:
  void Install(const char *Name, lua_CFunction Function, const char *Debug) {
    lua_pushcfunction(State, Function, Debug);
    lua_setglobal(State, Name);
  }
  void Push(const Value &Argument) {
    std::visit(
        [this](const auto &Typed) {
          using Type = std::decay_t<decltype(Typed)>;
          if constexpr (std::same_as<Type, bool>)
            lua_pushboolean(State, Typed ? 1 : 0);
          else if constexpr (std::same_as<Type, int>)
            lua_pushinteger(State, Typed);
          else if constexpr (std::same_as<Type, double>)
            lua_pushnumber(State, Typed);
          else
            lua_pushlstring(State, Typed.data(), Typed.size());
        },
        Argument);
  }
  lua_State *State = nullptr;
};

class LunaModel final {
public:
  LunaModel(LunaBenchmark::CacheMode Mode, bool DynamicOnly) {
    if (!Owner.IsReady())
      return;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::RegistrationResult Result =
        DynamicOnly ? Registry.RegisterFunction("Dynamic", &LunaDynamic)
                    : Registry.RegisterFunction("Touch", &LunaTouch);
    if (!Result.IsSuccess())
      return;
    if (!DynamicOnly && !Registry.RegisterFunction("Add", &LunaAdd).IsSuccess())
      return;
    if (Mode == LunaBenchmark::CacheMode::FrozenCached &&
        !Registry.Freeze().IsSuccess())
      return;
    Ready = true;
  }
  ~LunaModel() {
    for (PreparedLunaCall &Call : Calls)
      Hooks::ReleasePreparedBinding(Call);
  }
  [[nodiscard]] bool IsReady() const noexcept { return Ready; }
  [[nodiscard]] std::optional<PreparedLunaCall> Prepare(std::string_view Name,
                                                        int Returns) {
    if (!Ready)
      return std::nullopt;
    auto Call = Hooks::PrepareBindingInvocation(Owner, Name, Returns);
    if (Call)
      Calls.push_back(*Call);
    return Call;
  }
  [[nodiscard]] bool Invoke(const PreparedLunaCall &Call,
                            std::span<const Value> Arguments) {
    return Hooks::InvokePreparedBinding(Call, Arguments);
  }

private:
  Luna::State Owner;
  std::vector<PreparedLunaCall> Calls;
  bool Ready = false;
};

inline constexpr std::string_view VoidCase = "PureVoidCall";
inline constexpr std::string_view ScalarCase = "PureScalarCall";
inline constexpr std::string_view DynamicCase = "PureDynamicPackCall";
inline constexpr std::string_view VoidCorpus =
    "100000 cached C++ calls to an already-bound nullary closure";
inline constexpr std::string_view ScalarCorpus =
    "100000 cached C++ calls to an already-bound two-argument closure";
inline constexpr std::string_view DynamicCorpus =
    "100000 cached C++ calls to an already-bound dynamic-pack closure";

template <class Model, class Call>
std::optional<LunaBenchmark::Measurement>
Measure(LunaBenchmark::Suite &Suite, std::string_view Name,
        std::string_view Corpus, LunaBenchmark::CacheMode Mode,
        Model &ModelValue, const Call &Prepared,
        std::span<const Value> Arguments, std::size_t ExpectedTouches = 0) {
  return Suite.Measure(
      Name, Corpus, Mode, LunaBenchmark::InvocationCases::LoopCount, [&] {
        const std::size_t Before = SideEffectCount;
        for (std::size_t Index = 0;
             Index < LunaBenchmark::InvocationCases::LoopCount; ++Index)
          if (!ModelValue.Invoke(Prepared, Arguments))
            return false;
        return SideEffectCount - Before == ExpectedTouches;
      });
}

struct Measurements final {
  std::optional<LunaBenchmark::Measurement> Void;
  std::optional<LunaBenchmark::Measurement> Scalar;
  std::optional<LunaBenchmark::Measurement> Dynamic;
};

template <class Model>
Measurements MeasureModel(LunaBenchmark::Suite &Suite,
                          LunaBenchmark::CacheMode Mode, Model &Fixed,
                          Model &DynamicModel) {
  const std::vector<Value> None;
  const std::vector<Value> AddArguments{Value(7), Value(11)};
  const std::vector<Value> DynamicArguments{Value(7)};
  Measurements Result;
  const auto Touch = Fixed.Prepare("Touch", 0);
  const auto AddCall = Fixed.Prepare("Add", 1);
  const auto Dynamic = DynamicModel.Prepare("Dynamic", 3);
  if (Touch)
    Result.Void = Measure(Suite, VoidCase, VoidCorpus, Mode, Fixed, *Touch,
                          None, LunaBenchmark::InvocationCases::LoopCount);
  else
    Suite.Block(VoidCase, VoidCorpus, Mode,
                "could not cache the bound Touch closure");
  if (AddCall)
    Result.Scalar = Measure(Suite, ScalarCase, ScalarCorpus, Mode, Fixed,
                            *AddCall, AddArguments);
  else
    Suite.Block(ScalarCase, ScalarCorpus, Mode,
                "could not cache the bound Add closure");
  if (Dynamic)
    Result.Dynamic = Measure(Suite, DynamicCase, DynamicCorpus, Mode,
                             DynamicModel, *Dynamic, DynamicArguments);
  else
    Suite.Block(DynamicCase, DynamicCorpus, Mode,
                "could not cache the bound Dynamic closure");
  return Result;
}

template <class Model>
Measurements MeasureModel(LunaBenchmark::Suite &Suite,
                          LunaBenchmark::CacheMode Mode, Model &Raw) {
  return MeasureModel(Suite, Mode, Raw, Raw);
}

void Compare(LunaBenchmark::Suite &Suite, std::string_view Name,
             std::string_view Corpus,
             const std::optional<LunaBenchmark::Measurement> &Raw,
             const std::optional<LunaBenchmark::Measurement> &Candidate,
             LunaBenchmark::CacheMode Mode) {
  if (Raw && Candidate)
    Suite.Compare(Name, Corpus, LunaBenchmark::CacheMode::RawLuau, *Raw, Mode,
                  *Candidate);
}

} // namespace

int main(int Count, char **Values) {
  LunaBenchmark::Suite Suite("pure-call", Count, Values);
  RawModel Raw;
  LunaModel UnfrozenFixed(LunaBenchmark::CacheMode::UnfrozenUncached, false);
  LunaModel UnfrozenDynamic(LunaBenchmark::CacheMode::UnfrozenUncached, true);
  LunaModel FrozenFixed(LunaBenchmark::CacheMode::FrozenCached, false);
  LunaModel FrozenDynamic(LunaBenchmark::CacheMode::FrozenCached, true);
  if (!Raw.IsReady()) {
    Suite.Block(VoidCase, VoidCorpus, LunaBenchmark::CacheMode::RawLuau,
                "raw Luau state creation failed");
  }
  const Measurements RawMeasured =
      Raw.IsReady()
          ? MeasureModel(Suite, LunaBenchmark::CacheMode::RawLuau, Raw)
          : Measurements{};
  const Measurements Unfrozen =
      MeasureModel(Suite, LunaBenchmark::CacheMode::UnfrozenUncached,
                   UnfrozenFixed, UnfrozenDynamic);
  const Measurements Frozen =
      MeasureModel(Suite, LunaBenchmark::CacheMode::FrozenCached, FrozenFixed,
                   FrozenDynamic);
  Compare(Suite, VoidCase, VoidCorpus, RawMeasured.Void, Unfrozen.Void,
          LunaBenchmark::CacheMode::UnfrozenUncached);
  Compare(Suite, VoidCase, VoidCorpus, RawMeasured.Void, Frozen.Void,
          LunaBenchmark::CacheMode::FrozenCached);
  Compare(Suite, ScalarCase, ScalarCorpus, RawMeasured.Scalar, Unfrozen.Scalar,
          LunaBenchmark::CacheMode::UnfrozenUncached);
  Compare(Suite, ScalarCase, ScalarCorpus, RawMeasured.Scalar, Frozen.Scalar,
          LunaBenchmark::CacheMode::FrozenCached);
  Compare(Suite, DynamicCase, DynamicCorpus, RawMeasured.Dynamic,
          Unfrozen.Dynamic, LunaBenchmark::CacheMode::UnfrozenUncached);
  Compare(Suite, DynamicCase, DynamicCorpus, RawMeasured.Dynamic,
          Frozen.Dynamic, LunaBenchmark::CacheMode::FrozenCached);
  LunaBenchmark::Suite::Consume(SideEffectCount);
  return Suite.Finish();
}
