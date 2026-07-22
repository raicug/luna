// clang-format off
#include <Luna/Runtime.hpp>

#include <lua.h>
#include <lualib.h>

#include <memory>
// clang-format on

namespace Luna {
class Runtime::Impl {
public:
  Impl() : State(luaL_newstate()) {}

  ~Impl() {
    if (State)
      lua_close(State);
  }

  [[nodiscard]] bool IsReady() const noexcept { return State != nullptr; }

private:
  lua_State *State;
};

Runtime::Runtime() : Implementation(std::make_unique<Impl>()) {}

Runtime::~Runtime() = default;
Runtime::Runtime(Runtime &&) noexcept = default;
Runtime &Runtime::operator=(Runtime &&) noexcept = default;

bool Runtime::IsReady() const noexcept {
  return Implementation && Implementation->IsReady();
}
} // namespace Luna
