#pragma once

// The one place a member failure is worded.
//
// Requirement 13.9 asks one thing of every getter, setter, field-read, and
// field-write failure: it identifies the class and the member. That is not a
// per-call-site decision, so no caller composes its own sentence here. Every
// refusal a member access can produce - an unknown name, a refused receiver, a
// direction the member does not permit, a value the declared type does not
// accept, a target that refused, a target that threw, and a produced value the
// virtual machine could not publish - is worded through exactly the shared
// conversion vocabulary, with the class/member qualified name as its subject.
//
// The consequence is that one member reports one identical sentence shape
// whether it was reached through a script index, a script assignment, or a
// private hook, and an instance method's ordinary-argument diagnostic and its
// property sibling's diagnostic name their subject the same way.
//
// This header names no Luau type. It words failures; it decides none.

// clang-format off
#include "state/type/structured_conversion.hpp"
#include "state/userdata/access.hpp"

#include <cstddef>
#include <string>
#include <string_view>
// clang-format on

namespace Luna::Detail {

// The conversion failure one refused access reports. The access gate decides in
// its own deterministic order; this only names the reason it stopped at.
[[nodiscard]] StructuredFailure
StructuredFailureForUserdataAccess(UserdataAccessFailure Failure) noexcept;

// The subject of one member: `Member 'Class.Member'`.
[[nodiscard]] ConversionSubject
MemberConversionSubject(std::string_view QualifiedName);

// One refused receiver of one member, worded exactly as an instance method's
// receiver refusal is worded, because it is the same gate and the same failure.
[[nodiscard]] std::string
DescribeMemberReceiverRefusal(std::string_view QualifiedName,
                              std::string_view ClassName,
                              UserdataAccessFailure Failure);

// One direction the member does not permit at all.
[[nodiscard]] std::string
DescribeMemberDirectionRefusal(std::string_view QualifiedName, bool Reading);

// One written value the member's declared canonical type does not accept. The
// nested diagnostic keeps the exact foundation classification of the value that
// arrived.
[[nodiscard]] std::string
DescribeMemberValueRefusal(std::string_view QualifiedName,
                           const StructuredDiagnostic &Failure);

// The same refusal without a nested diagnostic: the value is a Luna-owned value
// of another canonical type than the member declared.
[[nodiscard]] std::string
DescribeMemberValueMismatch(std::string_view QualifiedName,
                            std::string_view ExpectedType);

// One declared getter or setter that ran and refused.
[[nodiscard]] std::string
DescribeMemberTargetRefusal(std::string_view QualifiedName, bool Reading,
                            std::string_view Reason);

// One declared getter or setter that ran and threw. It keeps the foundation's
// exception-translation shape, so a member target and a callable target are
// translated the same way.
[[nodiscard]] std::string
DescribeMemberException(std::string_view QualifiedName, bool Reading,
                        std::string_view Detail);
[[nodiscard]] std::string
DescribeMemberUnknownException(std::string_view QualifiedName, bool Reading);

// One name the class never declared as a member.
[[nodiscard]] std::string DescribeUnknownMember(std::string_view ClassName,
                                                std::string_view Member);

// One member name that is a declared method of the class rather than a typed
// accessor, so an assignment to it is refused instead of shadowing the method.
[[nodiscard]] std::string
DescribeMemberMethodAssignment(std::string_view QualifiedName);

// One produced value the virtual machine could not publish. The getter already
// ran, so this names the publication rather than the target.
[[nodiscard]] std::string
DescribeMemberPublicationRefusal(std::string_view QualifiedName,
                                 std::string_view ValueTypeName);

// One member access Luna could not even start: the request, the access context,
// or the captured type generation was unavailable.
[[nodiscard]] std::string
DescribeMemberInternalRefusal(std::string_view QualifiedName,
                              std::string_view Detail);

} // namespace Luna::Detail
