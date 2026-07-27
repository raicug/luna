#pragma once

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

enum class MemberDispatchStage : std::uint8_t {
  Published,

  Request,

  UnknownMember,

  Receiver,

  Direction,

  Value,

  Target,

  Publication
};

[[nodiscard]] std::string_view
MemberDispatchStageText(MemberDispatchStage Stage) noexcept;

struct MemberDispatchObservation final {
  bool Attempted = false;
  bool Succeeded = false;

  bool Reading = true;

  MemberDispatchStage Stage = MemberDispatchStage::Request;
  MemberSideEffectBoundary Boundary = MemberSideEffectBoundary::BeforeUserCode;
  MemberAccessFailure Failure = MemberAccessFailure::None;
  UserdataAccessFailure Receiver = UserdataAccessFailure::None;

  std::string ClassName;
  std::string MemberName;
  std::string Diagnostic;

  int EntryDepth = 0;
  int RestoredDepth = 0;

  int PublishedCount = 0;

  bool ServedFromCache = false;
  bool Recorded = false;
  std::size_t Invalidated = 0;

  bool ServedByOperator = false;
};

class MemberDispatchRecorder final {
public:
  void Clear() noexcept;
  void Record(MemberDispatchObservation Observed);

  void NoteRestoredDepth(int RestoredDepth) noexcept;

  [[nodiscard]] const MemberDispatchObservation *Last() const noexcept;

private:
  std::optional<MemberDispatchObservation> LastObservation;
};

[[nodiscard]] bool InstallClassMemberDispatch(lua_State *State,
                                              int MetatableIndex,
                                              int ClassTableIndex,
                                              const std::string &QualifiedName);

} // namespace Luna::Detail
