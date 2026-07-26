// clang-format off
#include "state/userdata/member_diagnostics.hpp"

#include "state/type/structured_conversion.hpp"
#include "state/userdata/access.hpp"

#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

// The half of the member boundary one failure belongs to, spelled the way the
// declaration itself spells it: a property or field reads through a getter and
// writes through a setter.
[[nodiscard]] std::string_view TargetText(bool Reading) noexcept {
  return Reading ? "getter" : "setter";
}

[[nodiscard]] std::string Prefix(std::string_view QualifiedName) {
  return DescribeConversionSubject(MemberConversionSubject(QualifiedName));
}

[[nodiscard]] std::string ContextPrefix(std::string_view QualifiedName) {
  return DescribeConversionSubjectContext(
      MemberConversionSubject(QualifiedName));
}

} // namespace

StructuredFailure
StructuredFailureForUserdataAccess(UserdataAccessFailure Failure) noexcept {
  switch (Failure) {
  case UserdataAccessFailure::MissingValue:
    return StructuredFailure::MissingElement;
  case UserdataAccessFailure::ForeignLayout:
    return StructuredFailure::ForeignUserdata;
  case UserdataAccessFailure::ForeignState:
    return StructuredFailure::ForeignOriginState;
  case UserdataAccessFailure::MetatableMismatch:
    return StructuredFailure::MetatableMismatch;
  case UserdataAccessFailure::NullPayload:
  case UserdataAccessFailure::MissingLifetimeHandle:
  case UserdataAccessFailure::ExpiredLifetimeHandle:
  case UserdataAccessFailure::Unpublished:
  case UserdataAccessFailure::Invalidated:
  case UserdataAccessFailure::Destroyed:
  case UserdataAccessFailure::Released:
    return StructuredFailure::ExpiredUserdata;
  case UserdataAccessFailure::TypeMismatch:
    return StructuredFailure::UserdataTypeMismatch;
  case UserdataAccessFailure::IncompatibleObject:
    return StructuredFailure::IncompatibleUserdataObject;
  case UserdataAccessFailure::ConstViolation:
    return StructuredFailure::ConstViolation;
  case UserdataAccessFailure::None:
  case UserdataAccessFailure::UnavailableRequest:
    break;
  }
  return StructuredFailure::InternalFailure;
}

ConversionSubject MemberConversionSubject(std::string_view QualifiedName) {
  ConversionSubject Subject;
  Subject.Kind = ConversionSubjectKind::Member;
  Subject.Name = std::string(QualifiedName);
  return Subject;
}

std::string DescribeMemberReceiverRefusal(std::string_view QualifiedName,
                                          std::string_view ClassName,
                                          UserdataAccessFailure Failure) {
  StructuredDiagnostic Diagnostic;
  Diagnostic.Failure = StructuredFailureForUserdataAccess(Failure);
  Diagnostic.ExpectedType = std::string(ClassName);
  Diagnostic.ReceivedType = std::string(UserdataAccessFailureText(Failure));
  const ConversionSubject Subject = MemberConversionSubject(QualifiedName);
  return DescribeConversionFailure(Subject, ConversionDirection::Receiver, 0,
                                   Diagnostic);
}

std::string DescribeMemberDirectionRefusal(std::string_view QualifiedName,
                                           bool Reading) {
  return Prefix(QualifiedName) +
         (Reading ? " permits no read." : " permits no write.");
}

std::string DescribeMemberValueRefusal(std::string_view QualifiedName,
                                       const StructuredDiagnostic &Failure) {
  const ConversionSubject Subject = MemberConversionSubject(QualifiedName);
  return DescribeConversionFailure(Subject, ConversionDirection::MemberValue, 0,
                                   Failure);
}

std::string DescribeMemberValueMismatch(std::string_view QualifiedName,
                                        std::string_view ExpectedType) {
  StructuredDiagnostic Diagnostic;
  Diagnostic.Failure = StructuredFailure::TypeMismatch;
  Diagnostic.ExpectedType = std::string(ExpectedType);
  Diagnostic.ReceivedType = "a value of another canonical type";
  return DescribeMemberValueRefusal(QualifiedName, Diagnostic);
}

std::string DescribeMemberTargetRefusal(std::string_view QualifiedName,
                                        bool Reading, std::string_view Reason) {
  std::string Message = Prefix(QualifiedName) + " " +
                        std::string(TargetText(Reading)) + " refused the " +
                        (Reading ? "read" : "write");
  if (Reason.empty())
    return Message + ".";
  return Message + ": " + std::string(Reason);
}

std::string DescribeMemberException(std::string_view QualifiedName,
                                    bool Reading, std::string_view Detail) {
  return "Runtime error: " + ContextPrefix(QualifiedName) + " " +
         std::string(TargetText(Reading)) + " threw: " + std::string(Detail);
}

std::string DescribeMemberUnknownException(std::string_view QualifiedName,
                                           bool Reading) {
  return "Internal error: " + ContextPrefix(QualifiedName) + " " +
         std::string(TargetText(Reading)) + " threw an unknown C++ exception.";
}

std::string DescribeUnknownMember(std::string_view ClassName,
                                  std::string_view Member) {
  return "Class '" + std::string(ClassName) + "' declares no member '" +
         std::string(Member) + "'.";
}

std::string DescribeMemberMethodAssignment(std::string_view QualifiedName) {
  return Prefix(QualifiedName) +
         " is a declared method, so it permits no write.";
}

std::string DescribeMemberPublicationRefusal(std::string_view QualifiedName,
                                             std::string_view ValueTypeName) {
  return "Internal error: " + ContextPrefix(QualifiedName) +
         " produced a value of type " + std::string(ValueTypeName) +
         " that could not be published.";
}

std::string DescribeMemberInternalRefusal(std::string_view QualifiedName,
                                          std::string_view Detail) {
  return "Internal error: " + ContextPrefix(QualifiedName) + " " +
         std::string(Detail);
}

} // namespace Luna::Detail
