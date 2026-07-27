#pragma once

// clang-format off
#include <luna/binding/instance_receiver.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/type/type_generation.hpp"

#include <string>
#include <string_view>
// clang-format on

struct lua_State;

namespace Luna::Detail {

enum class ReceiverStatus {
  Bound,
  MissingReceiver,
  RefusedAccess,
  UnavailableClass
};

[[nodiscard]] std::string_view
ReceiverStatusText(ReceiverStatus Status) noexcept;

struct ValidatedReceiver final {
  ReceiverStatus Status = ReceiverStatus::MissingReceiver;
  InstanceReceiver Bound;
  std::string Diagnostic;

  [[nodiscard]] bool IsBound() const noexcept {
    return Status == ReceiverStatus::Bound && Bound.IsBound();
  }
};

[[nodiscard]] ValidatedReceiver ValidateInstanceReceiver(
    lua_State *State, std::string_view MemberName, const TypeGeneration &Types,
    const TypeDescriptor &Class, bool RequiresMutation, int StackIndex);

[[nodiscard]] std::string
ReceiverConstRejectionText(const TypeGeneration &Types,
                           const TypeDescriptor &Class);

[[nodiscard]] std::string
ReceiverAbsenceRejectionText(const TypeGeneration &Types,
                             const TypeDescriptor &Class);

} // namespace Luna::Detail
