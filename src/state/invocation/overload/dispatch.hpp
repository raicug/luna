#pragma once

// clang-format off
#include <luna/binding/instance_receiver.hpp>

#include "state/invocation/overload/resolution.hpp"
#include "state/type/type_generation.hpp"

#include <cstddef>
#include <string>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class BindingRecord;

struct OverloadDispatchResult final {
  OverloadSelectionStatus Status = OverloadSelectionStatus::NoViableCandidate;

  std::size_t SelectedCandidate = 0;

  std::string Diagnostic;

  std::size_t Considered = 0;
  std::size_t Viable = 0;

  [[nodiscard]] bool HasSelection() const noexcept {
    return Status == OverloadSelectionStatus::Selected;
  }
};

[[nodiscard]] OverloadDispatchResult
ResolveOverloadedCall(const BindingRecord &Record, lua_State *State,
                      const TypeGeneration &Types,
                      const InstanceReceiver *Receiver = nullptr);

} // namespace Luna::Detail
