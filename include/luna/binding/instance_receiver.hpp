#pragma once

// The receiver of one instance member.
//
// An instance method operates on one object, and that object is not an ordinary
// argument: it is rank position zero of the call. Luna describes it exactly
// once, here, and the description is all a declaration ever states about it -
// the registered class the object must be a value of, and whether the member
// only reads it.
//
// A receiver reaches native code only after every access check has already
// passed: the value is present, it was exposed by this logical State, it
// carries exactly the metatable identity of its class, its payload and lifetime
// are live, its dynamic type is the requested one, and a mutating member was
// given a mutable view. `InstanceReceiver` is therefore never something a
// declaration builds: it is what a validated access hands to the one member it
// validated for, and it owns nothing, keeps nothing alive, and outlives
// nothing.
//
// Nothing here is a virtual-machine value, a stack index, or a Luau type.

// clang-format off
#include <luna/type/stable_type_key.hpp>

#include <utility>
// clang-format on

namespace Luna {

// What one instance member declares about the object it operates on.
//
// The class is named by its validated stable key, exactly the way an instance
// return names it, so the receiver is resolved through the canonical type
// registry of the generation the invocation captured rather than through
// anything the call site supplied. A const member states that it only reads its
// object, which is what lets a const view accept it and a mutable view prefer
// its non-const sibling.
class ReceiverMetadata final {
public:
  ReceiverMetadata() = default;

  [[nodiscard]] static ReceiverMetadata ForInstance(StableTypeKey Class,
                                                    bool ReadsOnly) {
    ReceiverMetadata Declared;
    Declared.ClassValue = std::move(Class);
    Declared.ReadsOnlyValue = ReadsOnly;
    return Declared;
  }

  // The registered class the receiver must be a value of.
  [[nodiscard]] const StableTypeKey &Class() const noexcept {
    return ClassValue;
  }

  // The member reads its object without mutating it.
  [[nodiscard]] bool IsConst() const noexcept { return ReadsOnlyValue; }

  // The member needs a mutable view of its object, so a const view refuses it
  // before any native code runs.
  [[nodiscard]] bool RequiresMutation() const noexcept {
    return !ReadsOnlyValue;
  }

  [[nodiscard]] bool IsDeclared() const noexcept {
    return ClassValue.IsValid();
  }

private:
  StableTypeKey ClassValue;
  bool ReadsOnlyValue = false;
};

// One receiver that already passed every access check.
//
// It is borrowed for the extent of the one invocation that validated it: it
// owns no object, holds no ownership reference, and keeps nothing alive. A
// bound receiver is proof that the value was neither missing, foreign, stale,
// wrongly typed, nor const-violating, because validated access is the only
// thing that produces one.
class InstanceReceiver final {
public:
  InstanceReceiver() = default;

  [[nodiscard]] static InstanceReceiver Validated(void *Object,
                                                  bool PermitsMutation) {
    InstanceReceiver Bound;
    Bound.StorageValue = Object;
    Bound.PermitsMutationValue = PermitsMutation;
    return Bound;
  }

  [[nodiscard]] void *Storage() const noexcept { return StorageValue; }

  // The view the receiver arrived through permits mutation of its object.
  [[nodiscard]] bool PermitsMutation() const noexcept {
    return PermitsMutationValue;
  }

  [[nodiscard]] bool IsBound() const noexcept {
    return StorageValue != nullptr;
  }

private:
  void *StorageValue = nullptr;
  bool PermitsMutationValue = false;
};

} // namespace Luna
