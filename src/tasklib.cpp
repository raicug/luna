#include "runtime.hpp"

#include <luna/luna.hpp>

#include <lua.h>

#include <optional>
#include <stdexcept>

namespace luna {
namespace {
// Starts the function at Function, with every argument after it, as a task.
double Launch(void *VM, detail::StartMode Mode, double Delay, int Function) {
  auto *L = static_cast<lua_State *>(VM);
  if (lua_type(L, Function) != LUA_TFUNCTION)
    throw std::runtime_error("expected a function to run");

  const int Top = lua_gettop(L);
  return static_cast<double>(
      detail::StartTask(L, Mode, Delay, [L, Function, Top](lua_State *Co) {
        lua_checkstack(Co, Top - Function + 2);
        for (int Index = Function; Index <= Top; ++Index)
          lua_xpush(L, Co, Index);
        return Top - Function;
      }));
}
} // namespace

RegistrationResult RegisterTaskLibrary(BindingRegistry Registry,
                                       bool GlobalAliases) {
  const auto Spawn = [](ArgumentPack Args) {
    return Launch(Args.GetVM(), detail::StartMode::Now, 0.0, 1);
  };
  const auto Defer = [](ArgumentPack Args) {
    return Launch(Args.GetVM(), detail::StartMode::Next, 0.0, 1);
  };
  const auto Delay = [](double Seconds, ArgumentPack Rest) {
    return Launch(Rest.GetVM(), detail::StartMode::After, Seconds, 2);
  };
  const auto Cancel = [](ArgumentPack Args) {
    State *Host = StateFromVM(Args.GetVM());
    return Host &&
           Host->Cancel(static_cast<std::uint64_t>(Args.View(0).AsNumber()));
  };
  const auto Wait = [](std::optional<double> Seconds) {
    SuspendMeasured(Seconds && *Seconds > 0.0 ? *Seconds : 0.0);
  };

  NamespaceBuilder Task = Registry.RegisterNamespace("task");
  Task.RegisterFunction("spawn", Spawn)
      .RegisterFunction("defer", Defer)
      .RegisterFunction("delay", Delay)
      .RegisterFunction("cancel", Cancel)
      .RegisterFunction("wait", Wait);
  RegistrationResult Result = Task.Commit();
  if (!Result.IsSuccess() || !GlobalAliases)
    return Result;

  for (const RegistrationResult &Alias :
       {Registry.Register("wait", Wait), Registry.Register("spawn", Spawn),
        Registry.Register("delay", Delay)}) {
    if (!Alias.IsSuccess())
      return Alias;
  }
  return Result;
}
} // namespace luna
