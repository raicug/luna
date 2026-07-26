#pragma once

// The virtual-machine half of one member access: `object.Member` and
// `object.Member = value` as Luna-owned metamethods of the class metatable.
//
// The gate in `member_access.hpp` decides everything about a member. This file
// only connects it to the virtual machine, and connecting it is where the two
// promises of Requirement 13.9 and 13.10 actually become observable:
//
//   * A refused access publishes nothing. The stack is returned to exactly the
//     depth the metamethod was entered at before the diagnostic is pushed, so a
//     getter that produced a value Luna could not publish leaves no
//     half-written result behind, and a refused write stores nothing.
//   * A refused access names the class and the member. Every diagnostic is
//     worded by `member_diagnostics.hpp`, so the sentence a script sees is the
//     same sentence a private hook sees.
//   * The side-effect boundary is a value, not a hope. Every dispatch records
//     which half it stopped in: before the declared target began, where Luna
//     states the native object is unchanged, or after it began, where Luna
//     restores the virtual machine and translates the exception but claims
//     nothing about the native object.
//
// The two metamethods are installed on the class metatable the first exposure
// of a class creates, so a class no value was ever created of installs nothing.
// Member lookup still resolves the class table first: a method, a static
// method, a constructor, and a factory of the class are one function value
// reached through the class scope itself, and only a name that is not one of
// those reaches a typed accessor.

// clang-format off
#include "state/userdata/access.hpp"
#include "state/userdata/member_access.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
// clang-format on

struct lua_State;

namespace Luna::Detail {

struct RegisteredClass;

// Which step of one member dispatch decided its outcome. The enumerator order
// is exactly the order the steps run in, so two outcomes can be compared to
// prove which step is earlier without repeating the order.
enum class MemberDispatchStage : std::uint8_t {
  // The complete access ran and its value was published.
  Published,

  // Luna could not start: no access context, no registered class, or no
  // captured type generation.
  Request,

  // The class declares no member of that name, or the name is a declared method
  // rather than a typed accessor.
  UnknownMember,

  // The receiver failed the deterministic access gate.
  Receiver,

  // The member permits no access in the requested direction.
  Direction,

  // The written value does not convert to the member's declared canonical type.
  Value,

  // The declared getter or setter ran and refused, or threw.
  Target,

  // The declared getter produced a value the virtual machine could not publish.
  Publication
};

[[nodiscard]] std::string_view
MemberDispatchStageText(MemberDispatchStage Stage) noexcept;

// Exactly what one member dispatch did. Nothing here is an address, and nothing
// here is a virtual-machine value: it is the deterministic account of one
// access plus the two stack depths that prove the callback checkpoint was
// restored.
struct MemberDispatchObservation final {
  bool Attempted = false;
  bool Succeeded = false;

  // The access was a read. A write records false.
  bool Reading = true;

  MemberDispatchStage Stage = MemberDispatchStage::Request;
  MemberSideEffectBoundary Boundary = MemberSideEffectBoundary::BeforeUserCode;
  MemberAccessFailure Failure = MemberAccessFailure::None;
  UserdataAccessFailure Receiver = UserdataAccessFailure::None;

  std::string ClassName;
  std::string MemberName;
  std::string Diagnostic;

  // The depth the metamethod was entered at, and the depth it was returned to
  // before its diagnostic was pushed. A refused access has them equal.
  int EntryDepth = 0;
  int RestoredDepth = 0;

  // How many virtual-machine values the access published. Every refusal
  // publishes zero.
  int PublishedCount = 0;

  bool ServedFromCache = false;
  bool Recorded = false;
  std::size_t Invalidated = 0;

  // The class declares no accessor of this name but does declare the indexing
  // or assignment operator, so the access was forwarded to that ordinary member
  // candidate. Luna keeps the metamethod either way.
  bool ServedByOperator = false;
};

// The last member dispatch of one State. It is owned by `State::Impl` and
// reached through the access context, so a dispatch can record without knowing
// anything about the State that owns it.
class MemberDispatchRecorder final {
public:
  void Clear() noexcept;
  void Record(MemberDispatchObservation Observed);

  // The depth the refused dispatch was returned to, noted after the stack was
  // restored and before the error is raised. It allocates nothing, because the
  // error tail must hold no object that needs destruction.
  void NoteRestoredDepth(int RestoredDepth) noexcept;

  [[nodiscard]] const MemberDispatchObservation *Last() const noexcept;

private:
  std::optional<MemberDispatchObservation> LastObservation;
};

// Installs Luna's member index and assignment metamethods on the class
// metatable. `MetatableIndex` names the class metatable and `ClassTableIndex`
// names the Luna-owned class table every member of the class is reached
// through. Both indices must be absolute.
[[nodiscard]] bool InstallClassMemberDispatch(lua_State *State,
                                              int MetatableIndex,
                                              int ClassTableIndex,
                                              const std::string &QualifiedName);

} // namespace Luna::Detail
