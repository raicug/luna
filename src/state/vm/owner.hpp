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

class BindingRecord;
class FaultInjector;
class StateTestHooks;
class TypeGeneration;
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
                                              FaultInjector &Faults);
  [[nodiscard]] ClosureInstallationStatus
  InstallBindingClosure(BindingRecord &Record, bool InjectFailure) noexcept;
  [[nodiscard]] const BindingRecord *
  ObserveInstalledBinding(const std::string &GlobalName) const noexcept;

  // The dispatch slot the closure installed at one canonical path carries. It
  // is the whole payload of that closure.
  [[nodiscard]] DispatchSlotId
  ObserveInstalledDispatchSlot(const std::string &GlobalName) const noexcept;
  // Journal support: the exact prior value of one canonical path, its exact
  // restoration, and the release of the protected reference that held it.
  [[nodiscard]] bool CaptureGlobalValue(const std::string &GlobalName,
                                        SavedVmValue &Saved) noexcept;
  [[nodiscard]] bool RestoreGlobalValue(const std::string &GlobalName,
                                        const SavedVmValue &Saved) noexcept;
  void ReleaseSavedValue(SavedVmValue &Saved) noexcept;

  // The same journal support for any canonical path, including the nested table
  // paths namespaces use. A root-scope path keeps the exact global behavior the
  // foundation established.
  [[nodiscard]] bool CaptureVmPath(const std::string &Path,
                                   SavedVmValue &Saved) noexcept;
  [[nodiscard]] bool RestoreVmPath(const std::string &Path,
                                   const SavedVmValue &Saved) noexcept;

  // What one canonical path holds right now, including the identity of the
  // table it holds. Nothing is retained.
  [[nodiscard]] VmPathObservation
  ObserveVmPath(const std::string &Path) const noexcept;

  // Creates or reopens the table of one namespace at its exact path, and
  // retains the table one path already holds. Neither operation ever replaces a
  // value it does not own; ownership is decided by Luna's records, not here.
  [[nodiscard]] NamespaceTableInstallation
  InstallNamespaceTable(const std::string &Path) noexcept;
  [[nodiscard]] NamespaceTableInstallation
  RetainNamespaceTable(const std::string &Path) noexcept;
  void ReleaseNamespaceTable(int Reference) noexcept;

  // Installs one converted constant value, and one Luna-owned immutable table,
  // at an exact canonical path. Neither ever replaces a value it does not own.
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

  // Names one State's userdata access and exposure contexts in this virtual
  // machine, so a validated access and a value exposure both resolve them from
  // one Luna-private slot instead of carrying them through every signature.
  // Publishing them again changes nothing.
  [[nodiscard]] bool
  PublishUserdataContexts(UserdataAccessContext &Access,
                          UserdataExposureContext &Exposure) noexcept;

  // Retires one exposed value ahead of its payload release: access is
  // invalidated and its cache entry and weak slot are removed. Nothing is
  // destroyed and nothing is deallocated here.
  [[nodiscard]] bool
  RetireExposedValue(UserdataAccessContext &Access,
                     const NativeIdentity &Identity) noexcept;

  // Runs one complete collection now. Luau exposes no script-visible collector,
  // so this is how a test proves what survives collection and what does not.
  [[nodiscard]] bool CollectGarbage() noexcept;

  // Whether Luna's typed-userdata collector is installed in this machine. It is
  // installed when the machine is created, so every value of every class is
  // collected through the one release gate.
  [[nodiscard]] bool HasUserdataCollector() const noexcept;

  // Closes the machine now and runs every remaining userdata finalizer. State
  // destruction calls it explicitly, while the class, allocator, type,
  // dispatch, and release metadata each finalizer needs is all still valid.
  // Calling it again does nothing.
  void Finalize() noexcept;

private:
  friend class StateTestHooks;

  void Reset() noexcept;

  lua_State *Handle = nullptr;
};

} // namespace Luna::Detail
