// clang-format off
#include "state/userdata/member_access.hpp"

#include <luna/binding/class_member.hpp>
#include <luna/binding/conversion.hpp>
#include <luna/binding/value.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/type/conversion_frame.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"
#include "state/userdata/access.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/lazy_cache.hpp"
#include "state/userdata/member_diagnostics.hpp"

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] MemberReadResult RefuseRead(MemberAccessFailure Failure,
                                          UserdataAccessFailure Receiver,
                                          std::string Reason) {
  MemberReadResult Result;
  Result.Failure = Failure;
  Result.Receiver = Receiver;
  Result.Refusal = std::move(Reason);
  return Result;
}

[[nodiscard]] MemberWriteResult RefuseWrite(MemberAccessFailure Failure,
                                            UserdataAccessFailure Receiver,
                                            std::string Reason) {
  MemberWriteResult Result;
  Result.Failure = Failure;
  Result.Receiver = Receiver;
  Result.Refusal = std::move(Reason);
  return Result;
}

[[nodiscard]] UserdataAccessResult
ValidateReceiver(const MemberAccessContext &Context, UserdataHeader &Header,
                 bool RequiresMutation) {
  UserdataAccessRequest Request = Context.Receiver;
  Request.RequiresMutation = RequiresMutation;
  return ValidateUserdataAccess(Header, Request);
}

[[nodiscard]] std::string ReceiverRefusalText(const RegisteredMember &Member,
                                              UserdataAccessFailure Failure) {
  return DescribeMemberReceiverRefusal(Member.QualifiedName, Member.ClassName,
                                       Failure);
}

[[nodiscard]] std::string DeclaredValueText(const MemberAccessContext &Context,
                                            const RegisteredMember &Member) {
  if (Context.Types != nullptr) {
    const std::string_view Public =
        Context.Types->PublicNameOf(Member.ValueDescriptor);
    if (!Public.empty())
      return std::string(Public);
  }
  return CanonicalTypeText(Member.ValueDescriptor);
}

} // namespace

std::string_view MemberAccessFailureText(MemberAccessFailure Failure) noexcept {
  switch (Failure) {
  case MemberAccessFailure::None:
    return "none";
  case MemberAccessFailure::UnavailableRequest:
    return "unavailable_request";
  case MemberAccessFailure::UnknownMember:
    return "unknown_member";
  case MemberAccessFailure::RefusedReceiver:
    return "refused_receiver";
  case MemberAccessFailure::UnreadableMember:
    return "unreadable_member";
  case MemberAccessFailure::UnwritableMember:
    return "unwritable_member";
  case MemberAccessFailure::IncompatibleValue:
    return "incompatible_value";
  case MemberAccessFailure::RefusedTarget:
    return "refused_target";
  case MemberAccessFailure::ContainedException:
    return "contained_exception";
  }
  return "unavailable_request";
}

std::string_view
MemberSideEffectBoundaryText(MemberSideEffectBoundary Boundary) noexcept {
  switch (Boundary) {
  case MemberSideEffectBoundary::BeforeUserCode:
    return "before_user_code";
  case MemberSideEffectBoundary::AfterUserCode:
    return "after_user_code";
  }
  return "before_user_code";
}

MemberSideEffectBoundary
MemberSideEffectBoundaryOf(MemberAccessFailure Failure) noexcept {
  switch (Failure) {
  case MemberAccessFailure::RefusedTarget:
  case MemberAccessFailure::ContainedException:
    return MemberSideEffectBoundary::AfterUserCode;
  case MemberAccessFailure::None:
  case MemberAccessFailure::UnavailableRequest:
  case MemberAccessFailure::UnknownMember:
  case MemberAccessFailure::RefusedReceiver:
  case MemberAccessFailure::UnreadableMember:
  case MemberAccessFailure::UnwritableMember:
  case MemberAccessFailure::IncompatibleValue:
    break;
  }
  return MemberSideEffectBoundary::BeforeUserCode;
}

TypeDescriptor CanonicalMemberValueType(const Value &Held) {
  switch (Held.index()) {
  case 0:
    return TypeDescriptor::ForFixed(FixedTypeKey::Boolean);
  case 1:
    return TypeDescriptor::ForFixed(FixedTypeKey::Int32);
  case 2:
    return TypeDescriptor::ForFixed(FixedTypeKey::Double);
  default:
    break;
  }
  return TypeDescriptor::ForFixed(FixedTypeKey::String);
}

MemberReadResult ReadClassMember(MemberAccessContext &Context,
                                 UserdataHeader &Header,
                                 const RegisteredMember &Member) {
  if (!Context.IsUsable())
    return RefuseRead(MemberAccessFailure::UnavailableRequest,
                      UserdataAccessFailure::UnavailableRequest,
                      DescribeMemberInternalRefusal(
                          Member.QualifiedName,
                          "was reached through an incomplete access request."));
  if (!Member.IsComplete())
    return RefuseRead(MemberAccessFailure::UnknownMember,
                      UserdataAccessFailure::None,
                      DescribeUnknownMember(Member.ClassName, Member.Segment));

  const UserdataAccessResult Receiver =
      ValidateReceiver(Context, Header, Member.ReadRequiresMutableReceiver);
  if (!Receiver.IsPermitted())
    return RefuseRead(MemberAccessFailure::RefusedReceiver, Receiver.Failure,
                      ReceiverRefusalText(Member, Receiver.Failure));

  if (!Member.PermitsRead())
    return RefuseRead(
        MemberAccessFailure::UnreadableMember, UserdataAccessFailure::None,
        DescribeMemberDirectionRefusal(Member.QualifiedName, true));

  if (Member.IsLazy() && Context.Lazy != nullptr) {
    if (const Value *Cached = Context.Lazy->Observe(
            Header, Member.Member, Context.DispatchGeneration)) {
      MemberReadResult Result;
      Result.Failure = MemberAccessFailure::None;
      Result.Produced = *Cached;
      Result.ServedFromCache = true;
      return Result;
    }
  }

  if (Member.IsInstance()) {
    MemberInstanceOutcome Produced;
    try {
      Produced = Member.InstanceRead(Receiver.Storage);
    } catch (const std::exception &Error) {
      return RefuseRead(
          MemberAccessFailure::ContainedException, UserdataAccessFailure::None,
          DescribeMemberException(Member.QualifiedName, true, Error.what()));
    } catch (...) {
      return RefuseRead(
          MemberAccessFailure::ContainedException, UserdataAccessFailure::None,
          DescribeMemberUnknownException(Member.QualifiedName, true));
    }

    if (!Produced.Succeeded || !Produced.Produced.has_value())
      return RefuseRead(MemberAccessFailure::RefusedTarget,
                        UserdataAccessFailure::None,
                        DescribeMemberTargetRefusal(Member.QualifiedName, true,
                                                    Produced.Refusal));

    MemberReadResult Result;
    Result.Failure = MemberAccessFailure::None;
    Result.Instance = std::move(*Produced.Produced);
    return Result;
  }

  if (Member.IsConverted()) {
    if (!Member.ConvertedRead)
      return RefuseRead(
          MemberAccessFailure::UnavailableRequest,
          UserdataAccessFailure::UnavailableRequest,
          DescribeMemberInternalRefusal(Member.QualifiedName,
                                        "declares no converted getter."));

    ConversionFrame Frame(Luna::ConversionDirection::Write,
                          Member.QualifiedName, 0);
    static_cast<void>(Frame.Open(OwnedValue::Nil()));
    ConversionContext Committing = Frame.CommitContext();

    MemberConvertedOutcome Produced;
    try {
      Produced = Member.ConvertedRead(Receiver.Storage, Committing);
    } catch (const std::exception &Error) {
      return RefuseRead(
          MemberAccessFailure::ContainedException, UserdataAccessFailure::None,
          DescribeMemberException(Member.QualifiedName, true, Error.what()));
    } catch (...) {
      return RefuseRead(
          MemberAccessFailure::ContainedException, UserdataAccessFailure::None,
          DescribeMemberUnknownException(Member.QualifiedName, true));
    }

    if (!Produced.Succeeded || !Frame.IsPublished() ||
        !Frame.PublishedResult().has_value())
      return RefuseRead(MemberAccessFailure::RefusedTarget,
                        UserdataAccessFailure::None,
                        DescribeMemberTargetRefusal(Member.QualifiedName, true,
                                                    Produced.Refusal));

    MemberReadResult Result;
    Result.Failure = MemberAccessFailure::None;
    Result.ConvertedValue = *Frame.PublishedResult();
    return Result;
  }

  MemberReadOutcome Produced;
  try {
    Produced = Member.Read(Receiver.Storage);
  } catch (const std::exception &Error) {
    return RefuseRead(
        MemberAccessFailure::ContainedException, UserdataAccessFailure::None,
        DescribeMemberException(Member.QualifiedName, true, Error.what()));
  } catch (...) {
    return RefuseRead(
        MemberAccessFailure::ContainedException, UserdataAccessFailure::None,
        DescribeMemberUnknownException(Member.QualifiedName, true));
  }

  if (!Produced.Succeeded)
    return RefuseRead(MemberAccessFailure::RefusedTarget,
                      UserdataAccessFailure::None,
                      DescribeMemberTargetRefusal(Member.QualifiedName, true,
                                                  Produced.Refusal));

  MemberReadResult Result;
  Result.Failure = MemberAccessFailure::None;
  Result.Produced = Produced.Produced;
  if (Member.IsLazy() && Context.Lazy != nullptr)
    Result.Recorded = Context.Lazy->Store(
        Header, Member.Member, Result.Produced, Context.DispatchGeneration);
  return Result;
}

MemberWriteResult WriteClassMember(MemberAccessContext &Context,
                                   UserdataHeader &Header,
                                   const RegisteredMember &Member,
                                   const MemberValueSource &Incoming) {
  if (!Context.IsUsable())
    return RefuseWrite(MemberAccessFailure::UnavailableRequest,
                       UserdataAccessFailure::UnavailableRequest,
                       DescribeMemberInternalRefusal(
                           Member.QualifiedName,
                           "was reached through an incomplete access "
                           "request."));
  if (!Member.IsComplete())
    return RefuseWrite(MemberAccessFailure::UnknownMember,
                       UserdataAccessFailure::None,
                       DescribeUnknownMember(Member.ClassName, Member.Segment));

  const UserdataAccessResult Receiver = ValidateReceiver(Context, Header, true);
  if (!Receiver.IsPermitted())
    return RefuseWrite(MemberAccessFailure::RefusedReceiver, Receiver.Failure,
                       ReceiverRefusalText(Member, Receiver.Failure));

  if (!Member.PermitsWrite())
    return RefuseWrite(
        MemberAccessFailure::UnwritableMember, UserdataAccessFailure::None,
        DescribeMemberDirectionRefusal(Member.QualifiedName, false));

  if (!Incoming)
    return RefuseWrite(
        MemberAccessFailure::UnavailableRequest,
        UserdataAccessFailure::UnavailableRequest,
        DescribeMemberInternalRefusal(Member.QualifiedName,
                                      "received no value to write."));

  const MemberValueOutcome Offered = Incoming();
  if (!Offered.Succeeded)
    return RefuseWrite(
        MemberAccessFailure::IncompatibleValue, UserdataAccessFailure::None,
        Offered.Refusal.empty()
            ? DescribeMemberValueMismatch(Member.QualifiedName,
                                          DeclaredValueText(Context, Member))
            : Offered.Refusal);

  if (Member.IsConverted()) {
    if (!Member.ConvertedWrite || !Offered.ConvertedValue.has_value())
      return RefuseWrite(
          MemberAccessFailure::UnavailableRequest,
          UserdataAccessFailure::UnavailableRequest,
          DescribeMemberInternalRefusal(Member.QualifiedName,
                                        "declares no converted setter."));

    ConversionFrame Frame(Luna::ConversionDirection::Read, Member.QualifiedName,
                          0);
    const ValueView Source = Frame.Open(*Offered.ConvertedValue);
    ConversionContext Committing = Frame.CommitContext();

    MemberConvertedOutcome Written;
    try {
      Written = Member.ConvertedWrite(Receiver.Storage, Source, Committing);
    } catch (const std::exception &Error) {
      return RefuseWrite(
          MemberAccessFailure::ContainedException, UserdataAccessFailure::None,
          DescribeMemberException(Member.QualifiedName, false, Error.what()));
    } catch (...) {
      return RefuseWrite(
          MemberAccessFailure::ContainedException, UserdataAccessFailure::None,
          DescribeMemberUnknownException(Member.QualifiedName, false));
    }

    if (!Written.Succeeded)
      return RefuseWrite(MemberAccessFailure::RefusedTarget,
                         UserdataAccessFailure::None,
                         DescribeMemberTargetRefusal(Member.QualifiedName,
                                                     false, Written.Refusal));

    MemberWriteResult Result;
    Result.Failure = MemberAccessFailure::None;
    if (Context.Lazy != nullptr)
      Result.Invalidated = Context.Lazy->InvalidateOwner(Header);
    return Result;
  }

  MemberWriteOutcome Written;
  try {
    Written = Member.Write(Receiver.Storage, Offered.Converted);
  } catch (const std::exception &Error) {
    return RefuseWrite(
        MemberAccessFailure::ContainedException, UserdataAccessFailure::None,
        DescribeMemberException(Member.QualifiedName, false, Error.what()));
  } catch (...) {
    return RefuseWrite(
        MemberAccessFailure::ContainedException, UserdataAccessFailure::None,
        DescribeMemberUnknownException(Member.QualifiedName, false));
  }

  if (!Written.Succeeded)
    return RefuseWrite(MemberAccessFailure::RefusedTarget,
                       UserdataAccessFailure::None,
                       DescribeMemberTargetRefusal(Member.QualifiedName, false,
                                                   Written.Refusal));

  if (Member.HasChangeHandler()) {
    try {
      Member.Change(Receiver.Storage, Offered.Converted);
    } catch (const std::exception &Error) {
      return RefuseWrite(
          MemberAccessFailure::ContainedException, UserdataAccessFailure::None,
          DescribeMemberException(Member.QualifiedName, false, Error.what()));
    } catch (...) {
      return RefuseWrite(
          MemberAccessFailure::ContainedException, UserdataAccessFailure::None,
          DescribeMemberUnknownException(Member.QualifiedName, false));
    }
  }

  MemberWriteResult Result;
  Result.Failure = MemberAccessFailure::None;

  if (Context.Lazy != nullptr)
    Result.Invalidated = Context.Lazy->InvalidateOwner(Header);
  return Result;
}

MemberWriteResult WriteClassMember(MemberAccessContext &Context,
                                   UserdataHeader &Header,
                                   const RegisteredMember &Member,
                                   const Value &Incoming) {
  const MemberValueSource Source = [&Context, &Member,
                                    &Incoming]() -> MemberValueOutcome {
    const TypeDescriptor Offered = CanonicalMemberValueType(Incoming);
    const auto OfferedIdentity = TypeIdentityRegistry::ComputeIdentity(Offered);
    if (!OfferedIdentity || *OfferedIdentity != Member.ValueType)
      return MemberValueOutcome::Refuse(DescribeMemberValueMismatch(
          Member.QualifiedName, DeclaredValueText(Context, Member)));
    return MemberValueOutcome::Accept(Incoming);
  };
  return WriteClassMember(Context, Header, Member, Source);
}

std::size_t InvalidateClassMemberCache(LazyPropertyCache &Cache,
                                       UserdataHeader &Header) {
  return Cache.InvalidateOwner(Header);
}

} // namespace Luna::Detail
