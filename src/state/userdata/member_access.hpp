#pragma once

// clang-format off
#include <luna/binding/class_member.hpp>
#include <luna/binding/conversion.hpp>
#include <luna/binding/value.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/userdata/access.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/lazy_cache.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna::Detail {

class TypeGeneration;

struct RegisteredMember final {
  SymbolId Member;
  std::string Segment;
  std::string QualifiedName;

  std::string ClassName;

  SymbolKind Kind = SymbolKind::Property;
  MemberAccess Access = MemberAccess::ReadOnly;
  PropertyEvaluation Evaluation = PropertyEvaluation::Immediate;
  MemberOwnership Ownership = MemberOwnership::Copied;

  TypeId ValueType;
  TypeDescriptor ValueDescriptor;

  bool ReadRequiresMutableReceiver = false;

  MemberReadOperation Read;
  MemberWriteOperation Write;
  MemberChangeOperation Change;

  MemberConvertedReadOperation ConvertedRead;
  MemberConvertedWriteOperation ConvertedWrite;

  MemberInstanceReadOperation InstanceRead;
  MemberInstanceWriteOperation InstanceWrite;

  bool InstanceWriteRequiresMutation = false;

  [[nodiscard]] bool IsConverted() const noexcept {
    return ConvertedRead != nullptr || ConvertedWrite != nullptr;
  }

  [[nodiscard]] bool IsInstance() const noexcept {
    return InstanceRead != nullptr;
  }

  [[nodiscard]] bool IsInstanceWritable() const noexcept {
    return InstanceWrite != nullptr;
  }

  [[nodiscard]] bool HasChangeHandler() const noexcept {
    return Change != nullptr;
  }

  [[nodiscard]] bool PermitsRead() const noexcept {
    return PermitsMemberRead(Access) &&
           (Read != nullptr || ConvertedRead != nullptr ||
            InstanceRead != nullptr);
  }

  [[nodiscard]] bool PermitsWrite() const noexcept {
    return PermitsMemberWrite(Access) &&
           (Write != nullptr || ConvertedWrite != nullptr ||
            InstanceWrite != nullptr);
  }

  [[nodiscard]] bool IsLazy() const noexcept {
    return Evaluation == PropertyEvaluation::Lazy;
  }

  [[nodiscard]] bool IsComplete() const noexcept {
    return Member.IsValid() && !QualifiedName.empty() && ValueType.IsValid() &&
           (Read != nullptr || Write != nullptr || ConvertedRead != nullptr ||
            ConvertedWrite != nullptr || InstanceRead != nullptr ||
            InstanceWrite != nullptr);
  }
};

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

enum class MemberSideEffectBoundary : std::uint8_t {
  BeforeUserCode,
  AfterUserCode
};

[[nodiscard]] std::string_view
MemberSideEffectBoundaryText(MemberSideEffectBoundary Boundary) noexcept;

[[nodiscard]] MemberSideEffectBoundary
MemberSideEffectBoundaryOf(MemberAccessFailure Failure) noexcept;

struct MemberAccessContext final {
  UserdataAccessRequest Receiver;
  LazyPropertyCache *Lazy = nullptr;

  const TypeGeneration *Types = nullptr;

  std::uint64_t DispatchGeneration = 0;

  [[nodiscard]] bool IsUsable() const noexcept { return Receiver.IsComplete(); }
};

struct MemberReadResult final {
  MemberAccessFailure Failure = MemberAccessFailure::UnavailableRequest;

  UserdataAccessFailure Receiver = UserdataAccessFailure::None;

  Value Produced;

  std::optional<OwnedValue> ConvertedValue;

  std::optional<ConstructedInstance> Instance;

  bool ServedFromCache = false;

  bool Recorded = false;

  std::string Refusal;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Failure == MemberAccessFailure::None;
  }
};

struct MemberWriteResult final {
  MemberAccessFailure Failure = MemberAccessFailure::UnavailableRequest;
  UserdataAccessFailure Receiver = UserdataAccessFailure::None;

  std::size_t Invalidated = 0;

  std::string Refusal;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Failure == MemberAccessFailure::None;
  }
};

[[nodiscard]] TypeDescriptor CanonicalMemberValueType(const Value &Held);

struct MemberValueOutcome final {
  bool Succeeded = false;
  Value Converted;
  std::optional<OwnedValue> ConvertedValue;
  void *InstanceStorage = nullptr;
  std::string Refusal;

  [[nodiscard]] static MemberValueOutcome Accept(Value Held) {
    MemberValueOutcome Outcome;
    Outcome.Succeeded = true;
    Outcome.Converted = std::move(Held);
    return Outcome;
  }

  [[nodiscard]] static MemberValueOutcome AcceptConverted(OwnedValue Held) {
    MemberValueOutcome Outcome;
    Outcome.Succeeded = true;
    Outcome.ConvertedValue = std::move(Held);
    return Outcome;
  }

  [[nodiscard]] static MemberValueOutcome AcceptInstance(void *Held) {
    MemberValueOutcome Outcome;
    Outcome.Succeeded = true;
    Outcome.InstanceStorage = Held;
    return Outcome;
  }

  [[nodiscard]] static MemberValueOutcome Refuse(std::string Reason) {
    MemberValueOutcome Outcome;
    Outcome.Refusal = std::move(Reason);
    return Outcome;
  }
};

using MemberValueSource = std::function<MemberValueOutcome()>;

[[nodiscard]] MemberReadResult ReadClassMember(MemberAccessContext &Context,
                                               UserdataHeader &Header,
                                               const RegisteredMember &Member);

[[nodiscard]] MemberWriteResult WriteClassMember(MemberAccessContext &Context,
                                                 UserdataHeader &Header,
                                                 const RegisteredMember &Member,
                                                 const Value &Incoming);

[[nodiscard]] MemberWriteResult
WriteClassMember(MemberAccessContext &Context, UserdataHeader &Header,
                 const RegisteredMember &Member,
                 const MemberValueSource &Incoming);

std::size_t InvalidateClassMemberCache(LazyPropertyCache &Cache,
                                       UserdataHeader &Header);

} // namespace Luna::Detail
