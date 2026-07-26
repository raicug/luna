#pragma once

// The receiver of one instance member, validated before anything else.
//
// An instance member operates on one object, and that object is rank position
// zero of the call rather than one of its arguments. Its presence, origin
// State, layout, metatable identity, payload and lifetime, dynamic type, and
// const access are all decided here - through exactly the conversion path an
// ordinary class argument takes, and therefore through the one access gate
// whose check order is fixed - before one ordinary argument is inspected,
// probed, or converted.
//
// That ordering is the whole point of this file: `object:Member(args)`,
// `object.Member(object, args)`, and `Class.Member(object, args)` are one call,
// and a dot call that supplied no receiver at all fails right here rather than
// as a shifted argument diagnostic.
//
// Nothing here converts an ordinary argument, invokes a native target, or
// mutates anything: a validated receiver is one borrowed native pointer plus
// whether the view it arrived through permits mutation.

// clang-format off
#include <luna/binding/instance_receiver.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/type/type_generation.hpp"

#include <string>
#include <string_view>
// clang-format on

struct lua_State;

namespace Luna::Detail {

// Why one instance member did or did not receive the object it operates on.
enum class ReceiverStatus {
  Bound,
  MissingReceiver,
  RefusedAccess,
  UnavailableClass
};

[[nodiscard]] std::string_view
ReceiverStatusText(ReceiverStatus Status) noexcept;

// One validated receiver, or the one deterministic diagnostic of its refusal.
struct ValidatedReceiver final {
  ReceiverStatus Status = ReceiverStatus::MissingReceiver;
  InstanceReceiver Bound;
  std::string Diagnostic;

  [[nodiscard]] bool IsBound() const noexcept {
    return Status == ReceiverStatus::Bound && Bound.IsBound();
  }
};

// Validates the value at `StackIndex` as the receiver of `MemberName`.
//
// `RequiresMutation` states whether the member mutates its object. It is
// decided by the member's own declaration, so a const member accepts a const
// value of its class and a non-const one refuses it before any native code
// runs.
[[nodiscard]] ValidatedReceiver ValidateInstanceReceiver(
    lua_State *State, std::string_view MemberName, const TypeGeneration &Types,
    const TypeDescriptor &Class, bool RequiresMutation, int StackIndex);

// The rejection tail one candidate records when the receiver it was given is a
// const view but the candidate mutates its object. It is worded exactly as the
// access gate words the same refusal, so one overload set and one single
// candidate never explain the same refusal differently.
[[nodiscard]] std::string
ReceiverConstRejectionText(const TypeGeneration &Types,
                           const TypeDescriptor &Class);

// The rejection tail one candidate records when it declares a receiver the call
// never supplied.
[[nodiscard]] std::string
ReceiverAbsenceRejectionText(const TypeGeneration &Types,
                             const TypeDescriptor &Class);

} // namespace Luna::Detail
