// clang-format off
#include "state/invocation/members/receiver.hpp"

#include <luna/binding/instance_receiver.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/type/structured_conversion.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"

#include <lua.h>

#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

// The one call form hint every missing receiver carries. It is the same
// sentence for every member, because the two spellings are the same call.
constexpr std::string_view CallFormHint =
    " An instance member is called as object:Member(...) or as "
    "Class.Member(object, ...).";

[[nodiscard]] std::string ClassText(const TypeGeneration &Types,
                                    const TypeDescriptor &Class) {
  const std::string_view Public = Types.PublicNameOf(Class);
  if (!Public.empty())
    return std::string(Public);
  return CanonicalTypeText(Class);
}

[[nodiscard]] ConversionSubject MemberSubject(std::string_view MemberName) {
  ConversionSubject Subject;
  Subject.Kind = ConversionSubjectKind::Member;
  Subject.Name = std::string(MemberName);
  return Subject;
}

// One receiver refusal, worded through exactly the shared conversion
// vocabulary.
[[nodiscard]] std::string Describe(std::string_view MemberName,
                                   const StructuredDiagnostic &Failure) {
  const ConversionSubject Subject = MemberSubject(MemberName);
  return DescribeConversionFailure(Subject, ConversionDirection::Receiver, 0,
                                   Failure);
}

[[nodiscard]] StructuredDiagnostic Reject(StructuredFailure Failure,
                                          std::string ExpectedType) {
  StructuredDiagnostic Diagnostic;
  Diagnostic.Failure = Failure;
  Diagnostic.ExpectedType = std::move(ExpectedType);
  return Diagnostic;
}

[[nodiscard]] ValidatedReceiver Refuse(ReceiverStatus Status,
                                       std::string Diagnostic) {
  ValidatedReceiver Result;
  Result.Status = Status;
  Result.Diagnostic = std::move(Diagnostic);
  return Result;
}

} // namespace

std::string_view ReceiverStatusText(ReceiverStatus Status) noexcept {
  switch (Status) {
  case ReceiverStatus::Bound:
    return "bound";
  case ReceiverStatus::MissingReceiver:
    return "missing_receiver";
  case ReceiverStatus::RefusedAccess:
    return "refused_access";
  case ReceiverStatus::UnavailableClass:
    return "unavailable_class";
  }
  return "unavailable_class";
}

std::string ReceiverConstRejectionText(const TypeGeneration &Types,
                                       const TypeDescriptor &Class) {
  return "receiver expected a mutable " + ClassText(Types, Class) +
         " but received a const view";
}

std::string ReceiverAbsenceRejectionText(const TypeGeneration &Types,
                                         const TypeDescriptor &Class) {
  return "receiver expected " + ClassText(Types, Class) +
         " but received no value";
}

ValidatedReceiver ValidateInstanceReceiver(
    lua_State *State, std::string_view MemberName, const TypeGeneration &Types,
    const TypeDescriptor &Class, bool RequiresMutation, int StackIndex) {
  const std::string ClassName = ClassText(Types, Class);
  if (!State)
    return Refuse(ReceiverStatus::UnavailableClass,
                  "Internal error: the argument stack is unavailable for "
                  "member '" +
                      std::string(MemberName) + "'.");

  // Presence first: a dot call that supplied no receiver at all fails here,
  // ahead of every ordinary-argument decision.
  if (StackIndex < 1 || lua_gettop(State) < StackIndex) {
    const StructuredDiagnostic Missing =
        Reject(StructuredFailure::MissingElement, ClassName);
    return Refuse(ReceiverStatus::MissingReceiver,
                  Describe(MemberName, Missing) + std::string(CallFormHint));
  }

  // The class is read through its own registered converter, which is the one
  // gate every access passes: layout, origin State, metatable identity,
  // payload, lifetime, publication, and dynamic type are all decided there, in
  // that fixed order.
  const TypeRecord *Record = Types.Find(Class);
  if (!Record || !Record->IsReadable || !Record->StructuredRead)
    return Refuse(ReceiverStatus::UnavailableClass,
                  "Internal error: the canonical type of member '" +
                      std::string(MemberName) +
                      "' receiver is unavailable for reading.");

  ConversionScope Scope(Types, State);
  const StructuredReadResult Read =
      Record->StructuredRead(Scope, *Record, StackIndex);
  if (!Read.IsSuccess()) {
    const bool Internal = IsInternalStructuredFailure(Read.Diagnostic.Failure);
    return Refuse(Internal ? ReceiverStatus::UnavailableClass
                           : ReceiverStatus::RefusedAccess,
                  Describe(MemberName, Read.Diagnostic));
  }

  void *const Storage = Read.ConvertedValue.HandleStorage();
  if (!Storage)
    return Refuse(ReceiverStatus::UnavailableClass,
                  "Internal error: member '" + std::string(MemberName) +
                      "' received no validated receiver.");

  // Const access is the last receiver check, exactly where the access gate puts
  // it: a const value of the class accepts a const member and refuses every
  // member that would mutate it.
  const bool PermitsMutation = Read.ConvertedValue.HandlePermitsMutation();
  if (RequiresMutation && !PermitsMutation) {
    const StructuredDiagnostic Violation =
        Reject(StructuredFailure::ConstViolation, ClassName);
    return Refuse(ReceiverStatus::RefusedAccess,
                  Describe(MemberName, Violation));
  }

  ValidatedReceiver Result;
  Result.Status = ReceiverStatus::Bound;
  Result.Bound = InstanceReceiver::Validated(Storage, PermitsMutation);
  return Result;
}

} // namespace Luna::Detail
