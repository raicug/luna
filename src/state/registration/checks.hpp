#pragma once

// Foundation precedence gate and the one place every registration diagnostic is
// worded. `CheckRegistrationPreconditions` keeps the exact wording and order
// the foundation established; the deterministic precedence gate of the unified
// transaction reuses the same pieces so no failure family gets a second,
// slightly different message.

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

// Grammar of one root-scope global identifier: 1 to 255 ASCII bytes, an initial
// letter or underscore, then letters, digits, or underscores.
[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateGlobalIdentifier(std::string_view GlobalName);

// Grammar of one canonical qualified name: validated identifier segments
// separated by the canonical separator, within the Luna-owned length policy.
[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateCanonicalQualifiedName(std::string_view QualifiedName);

// Grammar of one namespace segment. A scope accepts exactly one identifier
// segment per registration, so a name that carries the canonical separator is
// rejected instead of being split into several scopes.
[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateNamespaceSegment(std::string_view Segment);

// Subject of one diagnostic: the attempted symbol kind and its name, for
// example `global 'Add'` or `scope 'Studio.Physics'`.
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

// The exact canonical path already holds a value Luna does not own as the
// requested symbol, so the request collides instead of adopting or replacing
// it.
[[nodiscard]] ErrorDiagnostic
UnownedPathDiagnostic(std::string_view Subject,
                      std::string_view ExistingKindText);

// One builder can no longer be used: its State moved to another owner object,
// its owner was destroyed, the State froze, its scope was removed, or the
// registered model was replaced by an incompatible generation.
[[nodiscard]] ErrorDiagnostic StaleBuilderDiagnostic(std::string_view Subject,
                                                     std::string_view Reason);
[[nodiscard]] ErrorDiagnostic
UnavailableTypeDiagnostic(std::string_view Subject, std::string_view Position,
                          std::string_view TypeText);

// One canonical type is declared twice with different converters.
[[nodiscard]] ErrorDiagnostic
ConflictingConverterDiagnostic(std::string_view Subject,
                               std::string_view TypeText);

// One canonical type is declared twice with incompatible metadata.
[[nodiscard]] ErrorDiagnostic
IncompatibleTypeDeclarationDiagnostic(std::string_view Subject,
                                      std::string_view TypeText);

// Two unequal canonical descriptors share one type identity.
[[nodiscard]] ErrorDiagnostic
TypeDescriptorCollisionDiagnostic(std::string_view Subject,
                                  std::string_view TypeText);
[[nodiscard]] ErrorDiagnostic
MalformedMetadataDiagnostic(std::string_view Subject, std::string_view Reason);

// Two enumerators of one enumeration declare the same numeric value without
// declaring the second name as an explicit alias of the first.
[[nodiscard]] ErrorDiagnostic DuplicateEnumeratorValueDiagnostic(
    std::string_view Subject, std::string_view ExistingName, long long Numeric);

// One alias names something the enumeration does not declare as a canonical
// enumerator.
[[nodiscard]] ErrorDiagnostic
UnknownAliasTargetDiagnostic(std::string_view Subject,
                             std::string_view CanonicalName);

// One value does not fit a declared range: the enumeration's C++ underlying
// type, or the exact-integer domain Luna converts through. The received value
// and the permitted range are always reported, and the value is never narrowed,
// wrapped, or rounded to fit.
[[nodiscard]] ErrorDiagnostic
ValueOutOfRangeDiagnostic(std::string_view Subject, std::string_view Domain,
                          long long Numeric, long long Lowest,
                          long long Highest);

// One declared bitflag value carries a bit the declared supported mask does not
// contain. The value is rejected whole rather than truncated to the mask.
[[nodiscard]] ErrorDiagnostic
UnsupportedFlagBitsDiagnostic(std::string_view Subject, long long Numeric,
                              long long SupportedBits);

// Unscoped enumerations and bitflag behavior are never inferred: each one needs
// its own explicit opt-in.
[[nodiscard]] ErrorDiagnostic MissingOptInDiagnostic(std::string_view Subject,
                                                     std::string_view Reason);

// One declared member name belongs to Luna's own metamethod and system
// namespace. Luna never lets a declaration replace its own behavior, so the
// name collides instead of overriding.
[[nodiscard]] ErrorDiagnostic
ReservedMemberNameDiagnostic(std::string_view Subject,
                             std::string_view Segment);

// One declared member name is a metamethod Luna itself installs and depends on.
// The refusal names the Luna-owned behavior behind that metamethod, so a
// consumer learns which promise the declaration would have replaced.
[[nodiscard]] ErrorDiagnostic
ReservedMetamethodDiagnostic(std::string_view Subject, std::string_view Segment,
                             std::string_view RoleText);

// One member name is reachable from more than one base of a class, so no single
// declaration owns it. Luna reports the ambiguity instead of cloning a record
// or silently selecting one of the two.
[[nodiscard]] ErrorDiagnostic
AmbiguousInheritedMemberDiagnostic(std::string_view Subject,
                                   std::string_view Segment);

} // namespace Luna::Detail
