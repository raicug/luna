#pragma once

// The one gate between a Luau member access and a native property or field.
//
// A member access is the ordinary userdata access plus two more questions, and
// they are always asked in this order:
//
//   1. the request names one member of one registered class;
//   2. the receiver passes the whole deterministic access gate - layout, origin
//      State, metatable, lifetime, dynamic type, and const permission - so a
//      missing, foreign, stale, or const receiver is refused before anything
//      else, and before any native code runs;
//   3. the member permits the requested direction;
//   4. a written value converts to the member's declared canonical type;
//   5. the generated descriptor runs.
//
// The order is observable: a value that fails several checks always reports the
// earliest one. Steps 1 through 4 leave the native object untouched, which is
// what makes a refused access safe; once step 5 begins, Luna contains
// everything the consumer's code throws but never claims to reverse what it
// already did.
//
// The lazy cache is consulted only between steps 4 and 5 and only for a member
// that declared lazy evaluation. A cached value is returned without running the
// getter again; a getter that runs and succeeds records its value; a getter
// that refuses or throws records nothing at all.
//
// This header names no Luau type. It validates one header value and one member
// record, so it is equally usable from the member dispatch path and from a
// private test hook.

// clang-format off
#include <luna/binding/class_member.hpp>
#include <luna/binding/value.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/userdata/access.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/lazy_cache.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna::Detail {

class TypeGeneration;

// One published property or field of one registered class. It holds the two
// generated descriptors and the policies the declaration stated; it never holds
// a native object, a virtual-machine value, or a cached value.
struct RegisteredMember final {
  SymbolId Member;
  std::string Segment;
  std::string QualifiedName;

  // The qualified name of the class this member belongs to. It is what a
  // refusal names as the class the receiver was expected to be a value of.
  std::string ClassName;

  SymbolKind Kind = SymbolKind::Property;
  MemberAccess Access = MemberAccess::ReadOnly;
  PropertyEvaluation Evaluation = PropertyEvaluation::Immediate;
  MemberOwnership Ownership = MemberOwnership::Copied;

  // The canonical declared value type this member carries across the boundary,
  // by identity and by complete descriptor. The descriptor is what lets a
  // refusal name the type through the registry's own public name.
  TypeId ValueType;
  TypeDescriptor ValueDescriptor;

  // The declared getter reaches the object through a mutable receiver, so a
  // const view refuses even the read.
  bool ReadRequiresMutableReceiver = false;

  MemberReadOperation Read;
  MemberWriteOperation Write;

  [[nodiscard]] bool PermitsRead() const noexcept {
    return PermitsMemberRead(Access) && Read != nullptr;
  }

  [[nodiscard]] bool PermitsWrite() const noexcept {
    return PermitsMemberWrite(Access) && Write != nullptr;
  }

  [[nodiscard]] bool IsLazy() const noexcept {
    return Evaluation == PropertyEvaluation::Lazy;
  }

  [[nodiscard]] bool IsComplete() const noexcept {
    return Member.IsValid() && !QualifiedName.empty() && ValueType.IsValid() &&
           (Read != nullptr || Write != nullptr);
  }
};

// Why one member access never reached the declared target, or did. The
// enumerator order is exactly the order the checks run in.
enum class MemberAccessFailure : std::uint8_t {
  None,
  UnavailableRequest,
  UnknownMember,
  RefusedReceiver,
  UnreadableMember,
  UnwritableMember,
  IncompatibleValue,
  RefusedTarget,
  ContainedException
};

[[nodiscard]] std::string_view
MemberAccessFailureText(MemberAccessFailure Failure) noexcept;

// Which half of the member boundary one outcome belongs to. Requirement 13.10
// promises two different things on either side of it, so the two halves are
// separate values rather than one degree of confidence:
//
//   * `BeforeUserCode` - nothing the consumer declared has run. The request,
//   the
//     member, the receiver, the direction, and the value type were all decided
//     by Luna alone, so the native object is exactly as it was and Luna states
//     that.
//   * `AfterUserCode` - the declared getter or setter began. Luna still removes
//     every virtual-machine-visible partial result, restores the callback stack
//     exactly, and translates whatever the target threw, but it never claims to
//     reverse what the consumer's own code already did to the native object.
enum class MemberSideEffectBoundary : std::uint8_t {
  BeforeUserCode,
  AfterUserCode
};

[[nodiscard]] std::string_view
MemberSideEffectBoundaryText(MemberSideEffectBoundary Boundary) noexcept;

// The half of the boundary one gate failure belongs to. It is derived from the
// failure alone, because the gate's check order is exactly what decides whether
// user code had started.
[[nodiscard]] MemberSideEffectBoundary
MemberSideEffectBoundaryOf(MemberAccessFailure Failure) noexcept;

// What one member access needs from the State the value came from.
struct MemberAccessContext final {
  UserdataAccessRequest Receiver;
  LazyPropertyCache *Lazy = nullptr;

  // The immutable type generation this access captured. Every diagnostic of the
  // access names its types through exactly that generation, so one access never
  // mixes two generations' public names. It is optional: a gate reached without
  // one still decides identically and names the declared type canonically.
  const TypeGeneration *Types = nullptr;

  // The dispatch generation the access runs under. A cached lazy value belongs
  // to exactly one generation, so a replacement invalidates by mismatch.
  std::uint64_t DispatchGeneration = 0;

  [[nodiscard]] bool IsUsable() const noexcept { return Receiver.IsComplete(); }
};

// The outcome of one member read.
struct MemberReadResult final {
  MemberAccessFailure Failure = MemberAccessFailure::UnavailableRequest;

  // The deterministic reason the receiver was refused, when it was.
  UserdataAccessFailure Receiver = UserdataAccessFailure::None;

  Value Produced;

  // The value came from the lazy cache, so the declared getter did not run.
  bool ServedFromCache = false;

  // The successful value was recorded for later reuse.
  bool Recorded = false;

  std::string Refusal;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Failure == MemberAccessFailure::None;
  }
};

// The outcome of one member write.
struct MemberWriteResult final {
  MemberAccessFailure Failure = MemberAccessFailure::UnavailableRequest;
  UserdataAccessFailure Receiver = UserdataAccessFailure::None;

  // How many cached values the successful write invalidated.
  std::size_t Invalidated = 0;

  std::string Refusal;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Failure == MemberAccessFailure::None;
  }
};

// The canonical type one Luna-owned value converts through. It is what a
// written value is compared against before the declared setter ever runs.
[[nodiscard]] TypeDescriptor CanonicalMemberValueType(const Value &Held);

// One incoming value of one member write, converted only once the gate reaches
// the value step.
//
// A write that starts from a virtual-machine value has to convert that value
// through the member's declared canonical type, and that conversion must happen
// neither before the receiver was validated nor after the setter began. Handing
// the gate a source instead of a finished value is what keeps the fixed order
// intact: the gate calls this exactly once, exactly where the value step is.
struct MemberValueOutcome final {
  bool Succeeded = false;
  Value Converted;
  std::string Refusal;

  [[nodiscard]] static MemberValueOutcome Accept(Value Held) {
    MemberValueOutcome Outcome;
    Outcome.Succeeded = true;
    Outcome.Converted = std::move(Held);
    return Outcome;
  }

  [[nodiscard]] static MemberValueOutcome Refuse(std::string Reason) {
    MemberValueOutcome Outcome;
    Outcome.Refusal = std::move(Reason);
    return Outcome;
  }
};

using MemberValueSource = std::function<MemberValueOutcome()>;

// Reads one member of the value one header describes.
[[nodiscard]] MemberReadResult ReadClassMember(MemberAccessContext &Context,
                                               UserdataHeader &Header,
                                               const RegisteredMember &Member);

// Writes one member of the value one header describes. A successful write
// invalidates every cached value of that object, because Luna cannot know which
// computed values a native mutation changed.
[[nodiscard]] MemberWriteResult WriteClassMember(MemberAccessContext &Context,
                                                 UserdataHeader &Header,
                                                 const RegisteredMember &Member,
                                                 const Value &Incoming);

// The same write, with the incoming value produced only when the gate reaches
// its value step. Every earlier check has already refused by then, so a refused
// receiver never converts a virtual-machine value at all.
[[nodiscard]] MemberWriteResult
WriteClassMember(MemberAccessContext &Context, UserdataHeader &Header,
                 const RegisteredMember &Member,
                 const MemberValueSource &Incoming);

// Drops every cached value of one exposed object explicitly. It is what an
// explicit invalidation performs, and it is also what must happen before that
// object's payload is released.
std::size_t InvalidateClassMemberCache(LazyPropertyCache &Cache,
                                       UserdataHeader &Header);

} // namespace Luna::Detail
