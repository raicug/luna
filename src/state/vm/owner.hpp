#pragma once

// clang-format off
#include <luna/core/results/execution_result.hpp>

#include <optional>
#include <string>
#include <string_view>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class BindingRecord;
class FaultInjector;
class StateTestHooks;
enum class ClosureInstallationStatus;

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
  [[nodiscard]] bool SetIntegerGlobal(const std::string &GlobalName,
                                      int Value) noexcept;
  [[nodiscard]] std::optional<int>
  ObserveIntegerGlobal(const std::string &GlobalName) const noexcept;

private:
  friend class StateTestHooks;

  void Reset() noexcept;

  lua_State *Handle = nullptr;
};

} // namespace Luna::Detail
