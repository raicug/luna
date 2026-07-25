// clang-format off
#include "state/vm/owner.hpp"

#include "state/execution/executor.hpp"
#include "state/testing/test_control.hpp"
#include "state/vm/closure_installer.hpp"
#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>
#include <lualib.h>

#include <utility>
// clang-format on

namespace Luna::Detail {

VirtualMachineOwner::VirtualMachineOwner() noexcept {
  StateTestControl::RecordCreationAttempt();
  if (StateTestControl::ConsumeCreationFailure())
    return;

  Handle = luaL_newstate();
  if (Handle) {
    luaL_openlibs(Handle);
    StateTestControl::RecordSuccessfulCreation();
  }
}

VirtualMachineOwner::~VirtualMachineOwner() { Reset(); }

VirtualMachineOwner::VirtualMachineOwner(VirtualMachineOwner &&Other) noexcept
    : Handle(std::exchange(Other.Handle, nullptr)) {}

VirtualMachineOwner &
VirtualMachineOwner::operator=(VirtualMachineOwner &&Other) noexcept {
  if (this != &Other) {
    Reset();
    Handle = std::exchange(Other.Handle, nullptr);
  }
  return *this;
}

bool VirtualMachineOwner::IsReady() const noexcept { return Handle != nullptr; }
int VirtualMachineOwner::StackDepth() const noexcept {
  return Handle ? lua_gettop(Handle) : 0;
}

bool VirtualMachineOwner::SetStackDepth(int Depth) noexcept {
  if (!Handle || Depth < 0)
    return false;

  const int CurrentDepth = lua_gettop(Handle);
  if (Depth > CurrentDepth && !lua_checkstack(Handle, Depth - CurrentDepth))
    return false;

  lua_settop(Handle, Depth);
  return lua_gettop(Handle) == Depth;
}

ExecutionResult VirtualMachineOwner::ExecuteSource(std::string_view Source,
                                                   FaultInjector &Faults) {
  return Luna::Detail::ExecuteSource(Handle, Source, Faults);
}

ClosureInstallationStatus
VirtualMachineOwner::InstallBindingClosure(BindingRecord &Record,
                                           bool InjectFailure) noexcept {
  return Luna::Detail::InstallBindingClosure(Handle, Record, InjectFailure);
}

const BindingRecord *VirtualMachineOwner::ObserveInstalledBinding(
    const std::string &GlobalName) const noexcept {
  return Luna::Detail::ObserveInstalledBinding(Handle, GlobalName);
}

bool VirtualMachineOwner::SetIntegerGlobal(const std::string &GlobalName,
                                           int Value) noexcept {
  if (!Handle || !lua_checkstack(Handle, 1))
    return false;
  StackCheckpoint Checkpoint(Handle);
  lua_pushinteger(Handle, Value);
  lua_setglobal(Handle, GlobalName.c_str());
  return true;
}

std::optional<int> VirtualMachineOwner::ObserveIntegerGlobal(
    const std::string &GlobalName) const noexcept {
  if (!Handle || !lua_checkstack(Handle, 1))
    return std::nullopt;
  StackCheckpoint Checkpoint(Handle);
  lua_getglobal(Handle, GlobalName.c_str());
  int IsNumber = 0;
  const int Value = lua_tointegerx(Handle, -1, &IsNumber);
  if (!IsNumber)
    return std::nullopt;
  return Value;
}

void VirtualMachineOwner::Reset() noexcept {
  if (!Handle)
    return;

  lua_close(Handle);
  Handle = nullptr;
  StateTestControl::RecordRelease();
}

} // namespace Luna::Detail