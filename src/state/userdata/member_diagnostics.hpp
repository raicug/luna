#pragma once

// clang-format off
#include "state/type/structured_conversion.hpp"
#include "state/userdata/access.hpp"

#include <cstddef>
#include <string>
#include <string_view>
// clang-format on

namespace Luna::Detail {

[[nodiscard]] StructuredFailure
StructuredFailureForUserdataAccess(UserdataAccessFailure Failure) noexcept;

[[nodiscard]] ConversionSubject
MemberConversionSubject(std::string_view QualifiedName);

[[nodiscard]] std::string
DescribeMemberReceiverRefusal(std::string_view QualifiedName,
                              std::string_view ClassName,
                              UserdataAccessFailure Failure);

[[nodiscard]] std::string
DescribeMemberDirectionRefusal(std::string_view QualifiedName, bool Reading);

[[nodiscard]] std::string
DescribeMemberValueRefusal(std::string_view QualifiedName,
                           const StructuredDiagnostic &Failure);

[[nodiscard]] std::string
DescribeMemberValueMismatch(std::string_view QualifiedName,
                            std::string_view ExpectedType);

[[nodiscard]] std::string
DescribeMemberTargetRefusal(std::string_view QualifiedName, bool Reading,
                            std::string_view Reason);

[[nodiscard]] std::string
DescribeMemberException(std::string_view QualifiedName, bool Reading,
                        std::string_view Detail);
[[nodiscard]] std::string
DescribeMemberUnknownException(std::string_view QualifiedName, bool Reading);

[[nodiscard]] std::string DescribeUnknownMember(std::string_view ClassName,
                                                std::string_view Member);

[[nodiscard]] std::string
DescribeMemberMethodAssignment(std::string_view QualifiedName);

[[nodiscard]] std::string
DescribeMemberPublicationRefusal(std::string_view QualifiedName,
                                 std::string_view ValueTypeName);

[[nodiscard]] std::string
DescribeMemberInternalRefusal(std::string_view QualifiedName,
                              std::string_view Detail);

} // namespace Luna::Detail
