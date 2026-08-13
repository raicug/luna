// clang-format off
#include "state/vm/owner.hpp"

#include "state/execution/chunk_host.hpp"
#include "state/execution/executor.hpp"
#include "state/execution/interrupt.hpp"
#include "state/invocation/async/suspended_call.hpp"
#include "state/invocation/delegate/vm_delegate.hpp"
#include "state/invocation/parameters/vm_userdata_capture.hpp"
#include "state/testing/test_control.hpp"
#include "state/tooling/profiling_registry.hpp"
#include "state/userdata/collection.hpp"
#include "state/userdata/exposure.hpp"
#include "state/userdata/value_exposure.hpp"
#include "state/vm/closure_installer.hpp"
#include "state/vm/enum_item.hpp"
#include "state/vm/namespace_table.hpp"
#include "state/vm/saved_value.hpp"
#include "state/vm/stack_checkpoint.hpp"
#include "state/vm/value_table.hpp"

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

    static_cast<void>(InstallUserdataCollector(Handle));
    StateTestControl::RecordSuccessfulCreation();
  }
}

VirtualMachineOwner::~VirtualMachineOwner() {
  Reset();
}

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

bool VirtualMachineOwner::IsReady() const noexcept {
  return Handle != nullptr;
}
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

ExecutionResult VirtualMachineOwner::ExecuteSource(
    std::string_view Source, const ExecutionPolicy &Policy,
    FaultInjector &Faults, AsyncCallRegistry *Async) {
  return Luna::Detail::ExecuteSource(Handle, Source, Policy, Faults, Async);
}

bool VirtualMachineOwner::PublishAsyncCallRegistry(
    AsyncCallRegistry *Async) noexcept {
  return Luna::Detail::PublishAsyncRegistry(Handle, Async);
}

bool VirtualMachineOwner::PublishDelegateRegistry(
    VmDelegateRegistry *Handlers) noexcept {
  if (Handlers)
    Handlers->Bind(Handle);
  return Luna::Detail::PublishDelegateRegistry(Handle, Handlers);
}

bool VirtualMachineOwner::PublishUserdataCaptureRegistry(
    VmUserdataCaptureRegistry *Captures) noexcept {
  if (Captures)
    Captures->Bind(Handle);
  return Luna::Detail::PublishUserdataCaptureRegistry(Handle, Captures);
}

bool VirtualMachineOwner::PublishEnumItemRegistry(
    EnumItemRegistry *Items) noexcept {
  if (Items)
    Items->Bind(Handle);
  return Luna::Detail::PublishEnumItemRegistry(Handle, Items);
}

bool VirtualMachineOwner::PublishProfilingRegistry(
    ProfilingRegistry *Profiling) noexcept {
  return Luna::Detail::PublishProfilingRegistry(Handle, Profiling);
}

bool VirtualMachineOwner::PublishInterruptRequest(
    InterruptRequest *Pending) noexcept {
  if (!Handle || !Pending)
    return false;
  InstallInterruptCallback(Handle, Pending);
  return ObserveInterruptRequest(Handle) == Pending;
}

void VirtualMachineOwner::BindChunkHost(ChunkHost &Hosting,
                                        FaultInjector &Faults,
                                        AsyncCallRegistry &Async) noexcept {
  Hosting.Bind(Handle, &Faults, &Async);
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

DispatchSlotId VirtualMachineOwner::ObserveInstalledDispatchSlot(
    const std::string &GlobalName) const noexcept {
  return Luna::Detail::ObserveInstalledDispatchSlot(Handle, GlobalName);
}

bool VirtualMachineOwner::CaptureGlobalValue(const std::string &GlobalName,
                                             SavedVmValue &Saved) noexcept {
  return Luna::Detail::CaptureGlobalValue(Handle, GlobalName, Saved);
}

bool VirtualMachineOwner::RestoreGlobalValue(
    const std::string &GlobalName, const SavedVmValue &Saved) noexcept {
  return Luna::Detail::RestoreGlobalValue(Handle, GlobalName, Saved);
}

void VirtualMachineOwner::ReleaseSavedValue(SavedVmValue &Saved) noexcept {
  Luna::Detail::ReleaseSavedValue(Handle, Saved);
}

bool VirtualMachineOwner::CaptureVmPath(const std::string &Path,
                                        SavedVmValue &Saved) noexcept {
  if (!IsNestedVmPath(Path))
    return Luna::Detail::CaptureGlobalValue(Handle, Path, Saved);
  return CaptureVmPathValue(Handle, Path, Saved);
}

bool VirtualMachineOwner::RestoreVmPath(const std::string &Path,
                                        const SavedVmValue &Saved) noexcept {
  if (!IsNestedVmPath(Path))
    return Luna::Detail::RestoreGlobalValue(Handle, Path, Saved);
  return RestoreVmPathValue(Handle, Path, Saved);
}

bool VirtualMachineOwner::ClearVmPath(const std::string &Path) noexcept {
  if (!IsReady() || Path.empty())
    return false;
  return ClearVmPathValue(Handle, Path);
}
VmPathObservation
VirtualMachineOwner::ObserveVmPath(const std::string &Path) const noexcept {
  return Luna::Detail::ObserveVmPath(Handle, Path);
}

NamespaceTableInstallation
VirtualMachineOwner::InstallNamespaceTable(const std::string &Path) noexcept {
  return Luna::Detail::InstallNamespaceTable(Handle, Path);
}

ValueInstallationStatus
VirtualMachineOwner::InstallValue(const std::string &Path,
                                  const TypeGeneration &Types,
                                  const PlannedValue &Planned) noexcept {
  return Luna::Detail::InstallValueAtVmPath(Handle, Path, Types, Planned);
}

ValueInstallationStatus VirtualMachineOwner::InstallImmutableTable(
    const std::string &Path, const TypeGeneration &Types,
    const PlannedValueTable &Planned) noexcept {
  return Luna::Detail::InstallImmutableTableAtVmPath(Handle, Path, Types,
                                                     Planned);
}

NamespaceTableInstallation
VirtualMachineOwner::RetainNamespaceTable(const std::string &Path) noexcept {
  return Luna::Detail::RetainNamespaceTable(Handle, Path);
}

void VirtualMachineOwner::ReleaseNamespaceTable(int Reference) noexcept {
  Luna::Detail::ReleaseNamespaceTable(Handle, Reference);
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

bool VirtualMachineOwner::PublishUserdataContexts(
    UserdataAccessContext &Access, UserdataExposureContext &Exposure) noexcept {
  if (!Handle)
    return false;
  const bool PublishedAccess = PublishUserdataAccessContext(Handle, &Access);
  const bool PublishedExposure =
      PublishUserdataExposureContext(Handle, &Exposure);
  return PublishedAccess && PublishedExposure;
}

bool VirtualMachineOwner::RetireExposedValue(
    UserdataAccessContext &Access, const NativeIdentity &Identity) noexcept {
  if (!Handle)
    return false;
  return RetireExposedUserdata(Handle, Access, Identity);
}

bool VirtualMachineOwner::CollectGarbage() noexcept {
  if (!Handle)
    return false;
  lua_gc(Handle, LUA_GCCOLLECT, 0);
  return true;
}

bool VirtualMachineOwner::HasUserdataCollector() const noexcept {
  return UserdataCollectorIsInstalled(Handle);
}

void VirtualMachineOwner::Finalize() noexcept {
  Reset();
}

void VirtualMachineOwner::Reset() noexcept {
  if (!Handle)
    return;

  lua_close(Handle);
  Handle = nullptr;
  StateTestControl::RecordRelease();
}

} // namespace Luna::Detail
