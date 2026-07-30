#pragma once

// clang-format off
#include <luna/core/results/execution_result.hpp>

#include "state/dispatch/generation.hpp"
#include "state/vm/namespace_table.hpp"
#include "state/vm/saved_value.hpp"
#include "state/vm/value_table.hpp"

#include <optional>
#include <string>
#include <string_view>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class AsyncCallRegistry;
class BindingRecord;
class EnumItemRegistry;
class FaultInjector;
class ProfilingRegistry;
class StateTestHooks;
class TypeGeneration;
class VmDelegateRegistry;
class VmUserdataCaptureRegistry;
enum class ClosureInstallationStatus;
struct NativeIdentity;
struct UserdataAccessContext;
struct UserdataExposureContext;

class VirtualMachineOwner final {
public:
  VirtualMachineOwner() noexcept;
  ~VirtualMachineOwner();

  VirtualMachineOwner(const VirtualMachineOwner &) = delete;
  VirtualMachineOwner &operator=(const VirtualMachineOwner &) = delete;
  VirtualMachineOwner(VirtualMachineOwner &&Other) noexcept;
  VirtualMachineOwner &operator=(VirtualMachineOwner &&Other) noexcept;

  [[nodiscard]] bool IsReady() const noexcept;
  [[nodiscard]] int StackDepth() const noexcept;
  [[nodiscard]] bool SetStackDepth(int Depth) noexcept;
  [[nodiscard]] ExecutionResult ExecuteSource(std::string_view Source,
                                              FaultInjector &Faults,
                                              AsyncCallRegistry *Async);
  [[nodiscard]] bool
  PublishAsyncCallRegistry(AsyncCallRegistry *Async) noexcept;
  [[nodiscard]] bool
  PublishDelegateRegistry(VmDelegateRegistry *Handlers) noexcept;
  [[nodiscard]] bool
  PublishUserdataCaptureRegistry(VmUserdataCaptureRegistry *Captures) noexcept;
  [[nodiscard]] bool PublishEnumItemRegistry(EnumItemRegistry *Items) noexcept;
  [[nodiscard]] bool
  PublishProfilingRegistry(ProfilingRegistry *Profiling) noexcept;
  [[nodiscard]] ClosureInstallationStatus
  InstallBindingClosure(BindingRecord &Record, bool InjectFailure) noexcept;
  [[nodiscard]] const BindingRecord *
  ObserveInstalledBinding(const std::string &GlobalName) const noexcept;

  [[nodiscard]] DispatchSlotId
  ObserveInstalledDispatchSlot(const std::string &GlobalName) const noexcept;
  [[nodiscard]] bool CaptureGlobalValue(const std::string &GlobalName,
                                        SavedVmValue &Saved) noexcept;
  [[nodiscard]] bool RestoreGlobalValue(const std::string &GlobalName,
                                        const SavedVmValue &Saved) noexcept;
  void ReleaseSavedValue(SavedVmValue &Saved) noexcept;

  [[nodiscard]] bool CaptureVmPath(const std::string &Path,
                                   SavedVmValue &Saved) noexcept;
  [[nodiscard]] bool RestoreVmPath(const std::string &Path,
                                   const SavedVmValue &Saved) noexcept;

  [[nodiscard]] bool ClearVmPath(const std::string &Path) noexcept;

  [[nodiscard]] VmPathObservation
  ObserveVmPath(const std::string &Path) const noexcept;

  [[nodiscard]] NamespaceTableInstallation
  InstallNamespaceTable(const std::string &Path) noexcept;
  [[nodiscard]] NamespaceTableInstallation
  RetainNamespaceTable(const std::string &Path) noexcept;
  void ReleaseNamespaceTable(int Reference) noexcept;

  [[nodiscard]] ValueInstallationStatus
  InstallValue(const std::string &Path, const TypeGeneration &Types,
               const PlannedValue &Planned) noexcept;
  [[nodiscard]] ValueInstallationStatus
  InstallImmutableTable(const std::string &Path, const TypeGeneration &Types,
                        const PlannedValueTable &Planned) noexcept;

  [[nodiscard]] bool SetIntegerGlobal(const std::string &GlobalName,
                                      int Value) noexcept;
  [[nodiscard]] std::optional<int>
  ObserveIntegerGlobal(const std::string &GlobalName) const noexcept;

  [[nodiscard]] bool
  PublishUserdataContexts(UserdataAccessContext &Access,
                          UserdataExposureContext &Exposure) noexcept;

  [[nodiscard]] bool
  RetireExposedValue(UserdataAccessContext &Access,
                     const NativeIdentity &Identity) noexcept;

  [[nodiscard]] bool CollectGarbage() noexcept;

  [[nodiscard]] bool HasUserdataCollector() const noexcept;

  void Finalize() noexcept;

private:
  friend class StateTestHooks;

  void Reset() noexcept;

  lua_State *Handle = nullptr;
};

} // namespace Luna::Detail
