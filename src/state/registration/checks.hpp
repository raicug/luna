#pragma once

// clang-format off
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include <optional>
#include <string>
#include <string_view>
// clang-format on

namespace Luna::Detail {

[[nodiscard]] std::optional<ErrorDiagnostic>
CheckRegistrationPreconditions(std::string_view GlobalName, bool StateReady,
                               bool HasCallableTarget, bool IsDuplicate);

[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateGlobalIdentifier(std::string_view GlobalName);

[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateCanonicalQualifiedName(std::string_view QualifiedName);

[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateNamespaceSegment(std::string_view Segment);

[[nodiscard]] std::string GlobalSubject(std::string_view GlobalName);
[[nodiscard]] std::string SubjectText(std::string_view KindText,
                                      std::string_view Name);

[[nodiscard]] ErrorDiagnostic StateNotReadyDiagnostic(std::string_view Subject);
[[nodiscard]] ErrorDiagnostic FrozenStateDiagnostic(std::string_view Subject);
[[nodiscard]] ErrorDiagnostic ForeignThreadDiagnostic(std::string_view Subject);
[[nodiscard]] ErrorDiagnostic NullCallableDiagnostic(std::string_view Subject);
[[nodiscard]] ErrorDiagnostic DuplicateNameDiagnostic(std::string_view Subject);
[[nodiscard]] ErrorDiagnostic
IncompatibleCategoryDiagnostic(std::string_view Subject,
                               std::string_view ExistingKindText,
                               bool ExistingIsPending);
[[nodiscard]] ErrorDiagnostic
ForeignScopeDiagnostic(std::string_view Subject,
                       std::string_view ParentQualifiedName);
[[nodiscard]] ErrorDiagnostic StaleScopeDiagnostic(std::string_view Subject);

[[nodiscard]] ErrorDiagnostic
UnownedPathDiagnostic(std::string_view Subject,
                      std::string_view ExistingKindText);

[[nodiscard]] ErrorDiagnostic StaleBuilderDiagnostic(std::string_view Subject,
                                                     std::string_view Reason);
[[nodiscard]] ErrorDiagnostic
UnavailableTypeDiagnostic(std::string_view Subject, std::string_view Position,
                          std::string_view TypeText);

[[nodiscard]] ErrorDiagnostic
ConflictingConverterDiagnostic(std::string_view Subject,
                               std::string_view TypeText);

[[nodiscard]] ErrorDiagnostic
IncompatibleTypeDeclarationDiagnostic(std::string_view Subject,
                                      std::string_view TypeText);

[[nodiscard]] ErrorDiagnostic
TypeDescriptorCollisionDiagnostic(std::string_view Subject,
                                  std::string_view TypeText);
[[nodiscard]] ErrorDiagnostic
MalformedMetadataDiagnostic(std::string_view Subject, std::string_view Reason);

[[nodiscard]] ErrorDiagnostic DuplicateEnumeratorValueDiagnostic(
    std::string_view Subject, std::string_view ExistingName, long long Numeric);

[[nodiscard]] ErrorDiagnostic
UnknownAliasTargetDiagnostic(std::string_view Subject,
                             std::string_view CanonicalName);

[[nodiscard]] ErrorDiagnostic
ValueOutOfRangeDiagnostic(std::string_view Subject, std::string_view Domain,
                          long long Numeric, long long Lowest,
                          long long Highest);

[[nodiscard]] ErrorDiagnostic
UnsupportedFlagBitsDiagnostic(std::string_view Subject, long long Numeric,
                              long long SupportedBits);

[[nodiscard]] ErrorDiagnostic MissingOptInDiagnostic(std::string_view Subject,
                                                     std::string_view Reason);

[[nodiscard]] ErrorDiagnostic
ReservedMemberNameDiagnostic(std::string_view Subject,
                             std::string_view Segment);

[[nodiscard]] ErrorDiagnostic
ReservedMetamethodDiagnostic(std::string_view Subject, std::string_view Segment,
                             std::string_view RoleText);

[[nodiscard]] ErrorDiagnostic
AmbiguousInheritedMemberDiagnostic(std::string_view Subject,
                                   std::string_view Segment);

} // namespace Luna::Detail
