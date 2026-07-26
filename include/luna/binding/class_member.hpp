#pragma once

// Typed member access of one registered class: properties and fields.
//
// A member is described, never guessed. A property states which directions it
// permits - read, write, or both - and when its value is produced: immediately
// from the object, computed on every read, or computed once and cached until
// something invalidates it. A field states the same directions plus the
// ownership restriction its declared value obeys. Neither one is ever raw
// memory access: Luna generates a getter and a setter descriptor over the
// declared target and reaches the object only through the validated access
// gate, so constness, the origin State, the lifetime, and the dynamic type are
// all decided before the declared target runs.
//
// Nothing here is a virtual-machine value, a stack index, or a Luau type. A
// member read produces one Luna-owned `Value` and a member write consumes one,
// so the whole member boundary is expressible with Luna and standard-library
// types alone.
//
// The lazy policy describes when a value may be reused, never what is cached: a
// cached value belongs to one exposed object under one dispatch generation, it
// is recorded only when the getter succeeded, and reflection describes the
// policy without ever containing cache state.

// clang-format off
#include <luna/binding/value.hpp>

#include <functional>
#include <string>
#include <string_view>
// clang-format on

namespace Luna {

// Which directions one member permits.
enum class MemberAccess { ReadOnly, WriteOnly, ReadWrite };

// When the value of one property is produced.
//
// `Immediate` reads the value the object already holds. `Computed` runs the
// declared getter on every read and never reuses a result. `Lazy` runs the
// declared getter until it succeeds once and then reuses that result for the
// same exposed object under the same dispatch generation.
enum class PropertyEvaluation { Immediate, Computed, Lazy };

// How one field's declared value is owned across the member boundary.
//
// `Copied` is the only restriction Luna honors: the value is copied out of the
// object on a read and copied into it on a write, so no reference into a native
// object Luna does not own ever escapes. `Borrowed` and `Shared` name ownership
// Luna would have to keep alive on the consumer's behalf and are refused
// transactionally instead of reinterpreted.
enum class MemberOwnership { Copied, Borrowed, Shared };

[[nodiscard]] constexpr std::string_view
MemberAccessText(MemberAccess Access) noexcept {
  switch (Access) {
  case MemberAccess::ReadOnly:
    return "read-only";
  case MemberAccess::WriteOnly:
    return "write-only";
  case MemberAccess::ReadWrite:
    return "read-write";
  }
  return "read-only";
}

[[nodiscard]] constexpr std::string_view
PropertyEvaluationText(PropertyEvaluation Evaluation) noexcept {
  switch (Evaluation) {
  case PropertyEvaluation::Immediate:
    return "immediate";
  case PropertyEvaluation::Computed:
    return "computed";
  case PropertyEvaluation::Lazy:
    return "lazy";
  }
  return "immediate";
}

[[nodiscard]] constexpr std::string_view
MemberOwnershipText(MemberOwnership Ownership) noexcept {
  switch (Ownership) {
  case MemberOwnership::Copied:
    return "copied";
  case MemberOwnership::Borrowed:
    return "borrowed";
  case MemberOwnership::Shared:
    return "shared";
  }
  return "copied";
}

[[nodiscard]] constexpr bool PermitsMemberRead(MemberAccess Access) noexcept {
  return Access != MemberAccess::WriteOnly;
}

[[nodiscard]] constexpr bool PermitsMemberWrite(MemberAccess Access) noexcept {
  return Access != MemberAccess::ReadOnly;
}

// The declared policy of one property: which directions it permits and when its
// value is produced.
//
// A default-constructed policy is the plain read-only immediate property, which
// is what a single declared getter means. Every other shape is stated
// explicitly, and a policy that contradicts itself - a write-only property that
// still claims lazy evaluation, a readable property that names no evaluation -
// is refused transactionally rather than reinterpreted.
class PropertyPolicy final {
public:
  PropertyPolicy() = default;

  [[nodiscard]] static PropertyPolicy ReadOnly() {
    return PropertyPolicy(MemberAccess::ReadOnly,
                          PropertyEvaluation::Immediate);
  }

  [[nodiscard]] static PropertyPolicy WriteOnly() {
    return PropertyPolicy(MemberAccess::WriteOnly,
                          PropertyEvaluation::Immediate);
  }

  [[nodiscard]] static PropertyPolicy ReadWrite() {
    return PropertyPolicy(MemberAccess::ReadWrite,
                          PropertyEvaluation::Immediate);
  }

  // One value produced by the declared getter on every read. Nothing is ever
  // reused, so a computed property observes the object exactly as it is.
  [[nodiscard]] static PropertyPolicy Computed() {
    return PropertyPolicy(MemberAccess::ReadOnly, PropertyEvaluation::Computed);
  }

  // One value produced by the declared getter until it succeeds once, then
  // reused for that exposed object under that dispatch generation. A failed
  // getter is never reused.
  [[nodiscard]] static PropertyPolicy Lazy() {
    return PropertyPolicy(MemberAccess::ReadOnly, PropertyEvaluation::Lazy);
  }

  // The same evaluation with a writable direction, so a successful setter
  // invalidates the value the getter cached.
  [[nodiscard]] static PropertyPolicy LazyReadWrite() {
    return PropertyPolicy(MemberAccess::ReadWrite, PropertyEvaluation::Lazy);
  }

  [[nodiscard]] static PropertyPolicy ComputedReadWrite() {
    return PropertyPolicy(MemberAccess::ReadWrite,
                          PropertyEvaluation::Computed);
  }

  [[nodiscard]] MemberAccess Access() const noexcept { return AccessValue; }

  [[nodiscard]] PropertyEvaluation Evaluation() const noexcept {
    return EvaluationValue;
  }

  [[nodiscard]] bool PermitsRead() const noexcept {
    return PermitsMemberRead(AccessValue);
  }

  [[nodiscard]] bool PermitsWrite() const noexcept {
    return PermitsMemberWrite(AccessValue);
  }

  [[nodiscard]] bool IsLazy() const noexcept {
    return EvaluationValue == PropertyEvaluation::Lazy;
  }

  // A computed or lazy property must be readable: there is nothing to compute
  // or to reuse on a write-only member.
  [[nodiscard]] bool IsCoherent() const noexcept {
    if (EvaluationValue == PropertyEvaluation::Immediate)
      return true;
    return PermitsMemberRead(AccessValue);
  }

private:
  PropertyPolicy(MemberAccess Selected, PropertyEvaluation Produced) noexcept
      : AccessValue(Selected), EvaluationValue(Produced) {}

  MemberAccess AccessValue = MemberAccess::ReadOnly;
  PropertyEvaluation EvaluationValue = PropertyEvaluation::Immediate;
};

// The declared policy of one field: which directions it permits and how its
// declared value is owned across the member boundary.
//
// A default-constructed policy is the plain read-write copied field, which is
// what a single declared data member means. A const-qualified field is
// read-only, and any ownership other than copied is refused.
class FieldPolicy final {
public:
  FieldPolicy() = default;

  [[nodiscard]] static FieldPolicy ReadOnly() {
    return FieldPolicy(MemberAccess::ReadOnly, MemberOwnership::Copied, true);
  }

  [[nodiscard]] static FieldPolicy ReadWrite() {
    return FieldPolicy(MemberAccess::ReadWrite, MemberOwnership::Copied, true);
  }

  // An explicit ownership statement. Only a copied field is honored; every
  // other statement is a deterministic refusal of the declaration.
  [[nodiscard]] static FieldPolicy Owned(MemberOwnership Selected) {
    return FieldPolicy(MemberAccess::ReadWrite, Selected, true);
  }

  [[nodiscard]] MemberAccess Access() const noexcept { return AccessValue; }

  // The declaration stated its directions rather than accepting the default. A
  // const data member is read-only either way; asking for writes explicitly is
  // what turns that into a deterministic refusal instead of a narrowing.
  [[nodiscard]] bool DeclaresDirection() const noexcept {
    return DirectionIsExplicit;
  }

  [[nodiscard]] MemberOwnership Ownership() const noexcept {
    return OwnershipValue;
  }

  [[nodiscard]] bool PermitsRead() const noexcept {
    return PermitsMemberRead(AccessValue);
  }

  [[nodiscard]] bool PermitsWrite() const noexcept {
    return PermitsMemberWrite(AccessValue);
  }

  // Luna copies a field's value across the member boundary, so no reference
  // into a native object it does not own can escape.
  [[nodiscard]] bool IsCoherent() const noexcept {
    return OwnershipValue == MemberOwnership::Copied;
  }

private:
  FieldPolicy(MemberAccess Selected, MemberOwnership Owning,
              bool IsExplicit) noexcept
      : AccessValue(Selected), OwnershipValue(Owning),
        DirectionIsExplicit(IsExplicit) {}

  MemberAccess AccessValue = MemberAccess::ReadWrite;
  MemberOwnership OwnershipValue = MemberOwnership::Copied;
  bool DirectionIsExplicit = false;
};

namespace Detail {

// What one generated getter produced. A refusal names its own deterministic
// reason and carries no value at all, which is why a failed lazy getter can
// never be mistaken for a cacheable result.
struct MemberReadOutcome final {
  bool Succeeded = false;
  Value Produced;
  std::string Refusal;

  [[nodiscard]] static MemberReadOutcome Accept(Value Result) {
    MemberReadOutcome Outcome;
    Outcome.Succeeded = true;
    Outcome.Produced = std::move(Result);
    return Outcome;
  }

  [[nodiscard]] static MemberReadOutcome Refuse(std::string Reason) {
    MemberReadOutcome Outcome;
    Outcome.Refusal = std::move(Reason);
    return Outcome;
  }
};

// What one generated setter did. A refused write changes nothing, so the value
// a lazy getter cached earlier stays exactly as it was.
struct MemberWriteOutcome final {
  bool Succeeded = false;
  std::string Refusal;

  [[nodiscard]] static MemberWriteOutcome Accept() {
    MemberWriteOutcome Outcome;
    Outcome.Succeeded = true;
    return Outcome;
  }

  [[nodiscard]] static MemberWriteOutcome Refuse(std::string Reason) {
    MemberWriteOutcome Outcome;
    Outcome.Refusal = std::move(Reason);
    return Outcome;
  }
};

// One generated getter descriptor over an already validated native object. The
// object arrived through the access gate, so the descriptor never validates a
// receiver again and never sees a virtual-machine value.
using MemberReadOperation =
    std::function<MemberReadOutcome(const void *Object)>;

// One generated setter descriptor over an already validated mutable object and
// one converted Luna-owned value.
using MemberWriteOperation =
    std::function<MemberWriteOutcome(void *Object, const Value &Incoming)>;

} // namespace Detail

} // namespace Luna
