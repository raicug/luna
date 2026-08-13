#include <Luau/Compiler.h>
#include <lua.h>
#include <lualib.h>

#include <luna/luna.hpp>

#include "support/harness.hpp"
#include "support/invocation_cases.hpp"
#include "support/model.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace {

std::size_t SideEffectCount = 0;

int Touch(lua_State *) {
  ++SideEffectCount;
  return 0;
}

int Add(lua_State *State) {
  int LeftIsInteger = 0;
  int RightIsInteger = 0;
  const int Left = lua_tointegerx(State, 1, &LeftIsInteger);
  const int Right = lua_tointegerx(State, 2, &RightIsInteger);
  if (!LeftIsInteger || !RightIsInteger)
    return 0;
  lua_pushinteger(State, Left + Right);
  return 1;
}

int Dynamic(lua_State *State) {
  int IsInteger = 0;
  const int Seed = lua_tointegerx(State, 1, &IsInteger);
  if (!IsInteger)
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

[[nodiscard]] Luna::RegistrationResult
RegisterLunaFixed(Luna::BindingRegistry &Registry) {
  if (const Luna::RegistrationResult Result =
          Registry.RegisterFunction("Touch", &LunaTouch);
      !Result.IsSuccess())
    return Result;
  return Registry.RegisterFunction("Add", &LunaAdd);
}

[[nodiscard]] Luna::RegistrationResult
RegisterLunaDynamic(Luna::BindingRegistry &Registry) {
  return Registry.RegisterFunction("Dynamic", &LunaDynamic);
}

class RawInvocationModel final {
public:
  RawInvocationModel() : Root(luaL_newstate()) {
    if (!Root)
      return;
    luaL_openlibs(Root);
    Install("Touch", &Touch, "LunaBenchmark.RawLuau.Touch");
    Install("Add", &Add, "LunaBenchmark.RawLuau.Add");
    Install("Dynamic", &Dynamic, "LunaBenchmark.RawLuau.Dynamic");
  }

  ~RawInvocationModel() {
    if (Root)
      lua_close(Root);
  }

  RawInvocationModel(const RawInvocationModel &) = delete;
  RawInvocationModel &operator=(const RawInvocationModel &) = delete;

  [[nodiscard]] bool IsReady() const noexcept { return Root != nullptr; }

  [[nodiscard]] bool Run(const std::string &Script) {
    if (!Root)
      return false;

    const std::string Bytecode = Luau::compile(Script);
    const int EntryDepth = lua_gettop(Root);
    lua_State *Thread = lua_newthread(Root);
    if (!Thread)
      return false;
    const int Reference = lua_ref(Root, -1);
    if (Reference <= LUA_REFNIL) {
      lua_settop(Root, EntryDepth);
      return false;
    }

    const int Loaded =
        luau_load(Thread, "=RawLuau", Bytecode.data(), Bytecode.size(), 0);
    const int Resumed =
        Loaded == LUA_OK ? lua_resume(Thread, nullptr, 0) : Loaded;
    const bool Succeeded = Resumed == LUA_OK && lua_gettop(Thread) == 0;
    lua_resetthread(Thread);
    lua_unref(Root, Reference);
    lua_settop(Root, EntryDepth);
    return Succeeded;
  }

private:
  void Install(const char *Name, lua_CFunction Function,
               const char *DebugName) {
    lua_pushcfunction(Root, Function, DebugName);
    lua_setglobal(Root, Name);
  }

  lua_State *Root = nullptr;
};

struct InvocationMeasurements final {
  std::optional<LunaBenchmark::Measurement> Void;
  std::optional<LunaBenchmark::Measurement> Scalar;
  std::optional<LunaBenchmark::Measurement> Dynamic;
};

std::optional<LunaBenchmark::Measurement>
MeasureRaw(LunaBenchmark::Suite &Suite, RawInvocationModel &Model,
           std::string_view Name, std::string_view Corpus,
           std::size_t Operations, const std::string &Script) {
  if (!Model.IsReady()) {
    Suite.Block(Name, Corpus, LunaBenchmark::CacheMode::RawLuau,
                "raw Luau state creation failed");
    return std::nullopt;
  }
  return Suite.Measure(Name, Corpus, LunaBenchmark::CacheMode::RawLuau,
                       Operations,
                       [&Model, &Script] { return Model.Run(Script); });
}

InvocationMeasurements MeasureLuna(LunaBenchmark::Suite &Suite,
                                   LunaBenchmark::CacheMode Mode,
                                   const std::string &Void,
                                   const std::string &Scalar,
                                   const std::string &DynamicPack) {
  using LunaBenchmark::InvocationCases::DynamicCorpus;
  using LunaBenchmark::InvocationCases::DynamicName;
  using LunaBenchmark::InvocationCases::LoopCount;
  using LunaBenchmark::InvocationCases::ScalarCorpus;
  using LunaBenchmark::InvocationCases::ScalarName;
  using LunaBenchmark::InvocationCases::VoidCorpus;
  using LunaBenchmark::InvocationCases::VoidName;

  LunaBenchmark::ScenarioModel Fixed(Mode, &RegisterLunaFixed);
  LunaBenchmark::ScenarioModel Dynamic(Mode, &RegisterLunaDynamic);
  InvocationMeasurements Measured;
  if (!Fixed.IsPrepared()) {
    Suite.Block(VoidName, VoidCorpus, Mode, Fixed.Blocker());
    Suite.Block(ScalarName, ScalarCorpus, Mode, Fixed.Blocker());
  } else {
    Measured.Void =
        Suite.Measure(VoidName, VoidCorpus, Mode, LoopCount, [&Fixed, &Void] {
          const std::size_t Before = SideEffectCount;
          return Fixed.Run(Void) && SideEffectCount - Before == LoopCount;
        });
    Measured.Scalar =
        Suite.Measure(ScalarName, ScalarCorpus, Mode, LoopCount,
                      [&Fixed, &Scalar] { return Fixed.Run(Scalar); });
  }
  if (!Dynamic.IsPrepared()) {
    Suite.Block(DynamicName, DynamicCorpus, Mode, Dynamic.Blocker());
  } else {
    Measured.Dynamic = Suite.Measure(
        DynamicName, DynamicCorpus, Mode, LoopCount,
        [&Dynamic, &DynamicPack] { return Dynamic.Run(DynamicPack); });
  }
  return Measured;
}

void Compare(LunaBenchmark::Suite &Suite, std::string_view Name,
             std::string_view Corpus,
             const std::optional<LunaBenchmark::Measurement> &Raw,
             const std::optional<LunaBenchmark::Measurement> &Luna) {
  if (Raw && Luna)
    Suite.Compare(Name, Corpus, LunaBenchmark::CacheMode::RawLuau, *Raw,
                  LunaBenchmark::CacheMode::UnfrozenUncached, *Luna);
}

void CompareFrozen(LunaBenchmark::Suite &Suite, std::string_view Name,
                   std::string_view Corpus,
                   const std::optional<LunaBenchmark::Measurement> &Raw,
                   const std::optional<LunaBenchmark::Measurement> &Luna) {
  if (Raw && Luna)
    Suite.Compare(Name, Corpus, LunaBenchmark::CacheMode::RawLuau, *Raw,
                  LunaBenchmark::CacheMode::FrozenCached, *Luna);
}

} // namespace

int main(int ArgumentCount, char **ArgumentValues) {
  LunaBenchmark::Suite Suite("invocation", ArgumentCount, ArgumentValues);
  RawInvocationModel Model;
  const std::string Void = LunaBenchmark::InvocationCases::VoidScript();
  const std::string Scalar = LunaBenchmark::InvocationCases::ScalarScript();
  const std::string DynamicPack =
      LunaBenchmark::InvocationCases::DynamicScript();

  InvocationMeasurements Raw;
  if (!Model.IsReady()) {
    Suite.Block(LunaBenchmark::InvocationCases::VoidName,
                LunaBenchmark::InvocationCases::VoidCorpus,
                LunaBenchmark::CacheMode::RawLuau,
                "raw Luau state creation failed");
  } else {
    Raw.Void = Suite.Measure(
        LunaBenchmark::InvocationCases::VoidName,
        LunaBenchmark::InvocationCases::VoidCorpus,
        LunaBenchmark::CacheMode::RawLuau,
        LunaBenchmark::InvocationCases::LoopCount, [&Model, &Void] {
          const std::size_t Before = SideEffectCount;
          return Model.Run(Void) &&
                 SideEffectCount - Before ==
                     LunaBenchmark::InvocationCases::LoopCount;
        });
  }
  Raw.Scalar =
      MeasureRaw(Suite, Model, LunaBenchmark::InvocationCases::ScalarName,
                 LunaBenchmark::InvocationCases::ScalarCorpus,
                 LunaBenchmark::InvocationCases::LoopCount, Scalar);
  Raw.Dynamic =
      MeasureRaw(Suite, Model, LunaBenchmark::InvocationCases::DynamicName,
                 LunaBenchmark::InvocationCases::DynamicCorpus,
                 LunaBenchmark::InvocationCases::LoopCount, DynamicPack);

  const InvocationMeasurements Unfrozen =
      MeasureLuna(Suite, LunaBenchmark::CacheMode::UnfrozenUncached, Void,
                  Scalar, DynamicPack);
  const InvocationMeasurements Frozen = MeasureLuna(
      Suite, LunaBenchmark::CacheMode::FrozenCached, Void, Scalar, DynamicPack);

  Compare(Suite, LunaBenchmark::InvocationCases::VoidName,
          LunaBenchmark::InvocationCases::VoidCorpus, Raw.Void, Unfrozen.Void);
  CompareFrozen(Suite, LunaBenchmark::InvocationCases::VoidName,
                LunaBenchmark::InvocationCases::VoidCorpus, Raw.Void,
                Frozen.Void);
  Compare(Suite, LunaBenchmark::InvocationCases::ScalarName,
          LunaBenchmark::InvocationCases::ScalarCorpus, Raw.Scalar,
          Unfrozen.Scalar);
  CompareFrozen(Suite, LunaBenchmark::InvocationCases::ScalarName,
                LunaBenchmark::InvocationCases::ScalarCorpus, Raw.Scalar,
                Frozen.Scalar);
  Compare(Suite, LunaBenchmark::InvocationCases::DynamicName,
          LunaBenchmark::InvocationCases::DynamicCorpus, Raw.Dynamic,
          Unfrozen.Dynamic);
  CompareFrozen(Suite, LunaBenchmark::InvocationCases::DynamicName,
                LunaBenchmark::InvocationCases::DynamicCorpus, Raw.Dynamic,
                Frozen.Dynamic);
  LunaBenchmark::Suite::Consume(SideEffectCount);
  return Suite.Finish();
}
