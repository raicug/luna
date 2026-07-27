// clang-format off
#include "state/registration/checks.hpp"

#include "state/identity/symbol_descriptor.hpp"

#include <cstddef>
#include <string>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] constexpr bool IsAsciiLetter(unsigned char Byte) noexcept {
  return (Byte >= static_cast<unsigned char>('A') &&
          Byte <= static_cast<unsigned char>('Z')) ||
         (Byte >= static_cast<unsigned char>('a') &&
          Byte <= static_cast<unsigned char>('z'));
}

[[nodiscard]] constexpr bool IsAsciiDigit(unsigned char Byte) noexcept {
  return Byte >= static_cast<unsigned char>('0') &&
         Byte <= static_cast<unsigned char>('9');
}

[[nodiscard]] constexpr bool IsInitialByte(unsigned char Byte) noexcept {
  return IsAsciiLetter(Byte) || Byte == static_cast<unsigned char>('_');
}

[[nodiscard]] constexpr bool IsSubsequentByte(unsigned char Byte) noexcept {
  return IsInitialByte(Byte) || IsAsciiDigit(Byte);
}

[[nodiscard]] std::string HexByte(unsigned char Byte) {
  constexpr char Digits[] = "0123456789ABCDEF";
  std::string Result = "0x00";
  Result[2] = Digits[(Byte >> 4U) & 0x0FU];
  Result[3] = Digits[Byte & 0x0FU];
  return Result;
}

[[nodiscard]] ErrorDiagnostic InvalidName(std::string Message) {
  return ErrorDiagnostic::Create(ErrorCategory::InvalidGlobalName,
                                 std::move(Message));
}

[[nodiscard]] ErrorDiagnostic Refusal(ErrorCategory Category,
                                      std::string_view Subject,
                                      std::string_view Reason) {
  return ErrorDiagnostic::Create(Category, "Cannot register " +
                                               std::string(Subject) + ": " +
                                               std::string(Reason));
}

} // namespace

std::optional<ErrorDiagnostic>
ValidateGlobalIdentifier(std::string_view GlobalName) {
  if (GlobalName.empty())
    return InvalidName(
        "Invalid global name: name is empty; expected 1 to 255 ASCII bytes.");

  if (GlobalName.size() > 255)
    return InvalidName("Invalid global name: name has " +
                       std::to_string(GlobalName.size()) +
                       " bytes; maximum is 255.");

  for (std::size_t Index = 0; Index < GlobalName.size(); ++Index) {
    const auto Byte = static_cast<unsigned char>(GlobalName[Index]);
    if (Byte > 0x7FU)
      return InvalidName("Invalid global name: byte " +
                         std::to_string(Index + 1) + " is non-ASCII (" +
                         HexByte(Byte) + ").");

    const bool Valid =
        Index == 0 ? IsInitialByte(Byte) : IsSubsequentByte(Byte);
    if (!Valid) {
      const std::string Position =
          Index == 0 ? "first byte" : "byte " + std::to_string(Index + 1);
      const std::string Expected =
          Index == 0 ? "an ASCII letter or underscore"
                     : "an ASCII letter, digit, or underscore";
      return InvalidName("Invalid global name: " + Position + " is " +
                         HexByte(Byte) + "; expected " + Expected + ".");
    }
  }

  return std::nullopt;
}

std::optional<ErrorDiagnostic>
ValidateCanonicalQualifiedName(std::string_view QualifiedName) {
  switch (ClassifyQualifiedName(QualifiedName)) {
  case QualifiedNameStatus::Valid:
    return std::nullopt;
  case QualifiedNameStatus::Empty:
    return InvalidName("Invalid qualified name: name is empty; expected at "
                       "least one identifier segment.");
  case QualifiedNameStatus::TooLong:
    return InvalidName("Invalid qualified name: name has " +
                       std::to_string(QualifiedName.size()) +
                       " bytes; maximum is " +
                       std::to_string(MaximumQualifiedNameLength) + ".");
  case QualifiedNameStatus::EmptySegment:
    return InvalidName("Invalid qualified name '" + std::string(QualifiedName) +
                       "': a segment is empty.");
  case QualifiedNameStatus::InvalidLeadingCharacter:
    return InvalidName("Invalid qualified name '" + std::string(QualifiedName) +
                       "': a segment starts with a byte that is not an ASCII "
                       "letter or underscore.");
  case QualifiedNameStatus::InvalidCharacter:
    return InvalidName("Invalid qualified name '" + std::string(QualifiedName) +
                       "': a segment contains a byte that is not an ASCII "
                       "letter, digit, or underscore.");
  }
  return InvalidName("Invalid qualified name '" + std::string(QualifiedName) +
                     "'.");
}

std::optional<ErrorDiagnostic>
ValidateNamespaceSegment(std::string_view Segment) {
  if (Segment.find(QualifiedNameSeparator) != std::string_view::npos)
    return InvalidName(
        "Invalid namespace segment '" + std::string(Segment) +
        "': a namespace accepts exactly one identifier segment per "
        "registration.");

  if (auto Diagnostic = ValidateGlobalIdentifier(Segment)) {
    return InvalidName("Invalid namespace segment '" + std::string(Segment) +
                       "': " + std::string(Diagnostic->Message()));
  }
  return std::nullopt;
}

std::string GlobalSubject(std::string_view GlobalName) {
  return "global '" + std::string(GlobalName) + "'";
}

std::string SubjectText(std::string_view KindText, std::string_view Name) {
  return std::string(KindText) + " '" + std::string(Name) + "'";
}

ErrorDiagnostic StateNotReadyDiagnostic(std::string_view Subject) {
  return Refusal(ErrorCategory::StateNotReady, Subject, "State is not ready.");
}

ErrorDiagnostic FrozenStateDiagnostic(std::string_view Subject) {
  return Refusal(ErrorCategory::StateNotReady, Subject,
                 "State is frozen and rejects registration.");
}

ErrorDiagnostic ForeignThreadDiagnostic(std::string_view Subject) {
  return Refusal(ErrorCategory::StateNotReady, Subject,
                 "registration is only allowed on the State's owner thread.");
}

ErrorDiagnostic NullCallableDiagnostic(std::string_view Subject) {
  return Refusal(ErrorCategory::NullCallable, Subject,
                 "callable target is null.");
}

ErrorDiagnostic DuplicateNameDiagnostic(std::string_view Subject) {
  return Refusal(ErrorCategory::DuplicateGlobalName, Subject,
                 "name is already registered.");
}

ErrorDiagnostic
IncompatibleCategoryDiagnostic(std::string_view Subject,
                               std::string_view ExistingKindText,
                               bool ExistingIsPending) {
  const std::string Existing = std::string(ExistingKindText);
  const std::string Where = ExistingIsPending
                                ? " in this registration transaction"
                                : " in this State";
  return Refusal(ErrorCategory::DuplicateGlobalName, Subject,
                 "name is already declared as a " + Existing + Where + ".");
}

ErrorDiagnostic ForeignScopeDiagnostic(std::string_view Subject,
                                       std::string_view ParentQualifiedName) {
  return Refusal(ErrorCategory::InvalidGlobalName, Subject,
                 "parent scope '" + std::string(ParentQualifiedName) +
                     "' is not a Luna-owned scope of this State.");
}

ErrorDiagnostic StaleScopeDiagnostic(std::string_view Subject) {
  return Refusal(ErrorCategory::StateNotReady, Subject,
                 "the captured scope belongs to an earlier lifecycle "
                 "generation and is stale.");
}

ErrorDiagnostic UnownedPathDiagnostic(std::string_view Subject,
                                      std::string_view ExistingKindText) {
  return Refusal(ErrorCategory::DuplicateGlobalName, Subject,
                 "the requested path already holds a " +
                     std::string(ExistingKindText) +
                     " Luna does not own as that symbol; Luna never adopts or "
                     "replaces it.");
}

ErrorDiagnostic StaleBuilderDiagnostic(std::string_view Subject,
                                       std::string_view Reason) {
  return Refusal(ErrorCategory::StateNotReady, Subject,
                 "the builder is stale because " + std::string(Reason) + ".");
}

ErrorDiagnostic UnavailableTypeDiagnostic(std::string_view Subject,
                                          std::string_view Position,
                                          std::string_view TypeText) {
  return Refusal(ErrorCategory::Internal, Subject,
                 std::string(Position) + " uses type '" +
                     std::string(TypeText) +
                     "', which has no registered canonical type or converter.");
}

ErrorDiagnostic ConflictingConverterDiagnostic(std::string_view Subject,
                                               std::string_view TypeText) {
  return Refusal(ErrorCategory::Internal, Subject,
                 "type '" + std::string(TypeText) +
                     "' is already declared with a different converter.");
}

ErrorDiagnostic
IncompatibleTypeDeclarationDiagnostic(std::string_view Subject,
                                      std::string_view TypeText) {
  return Refusal(ErrorCategory::Internal, Subject,
                 "type '" + std::string(TypeText) +
                     "' is already declared with incompatible metadata.");
}

ErrorDiagnostic TypeDescriptorCollisionDiagnostic(std::string_view Subject,
                                                  std::string_view TypeText) {
  return Refusal(ErrorCategory::Internal, Subject,
                 "type '" + std::string(TypeText) +
                     "' collides with a different canonical descriptor that "
                     "shares its type identity.");
}

ErrorDiagnostic MalformedMetadataDiagnostic(std::string_view Subject,
                                            std::string_view Reason) {
  return Refusal(ErrorCategory::Internal, Subject, Reason);
}

ErrorDiagnostic
DuplicateEnumeratorValueDiagnostic(std::string_view Subject,
                                   std::string_view ExistingName,
                                   long long Numeric) {
  return Refusal(ErrorCategory::DuplicateGlobalName, Subject,
                 "value " + std::to_string(Numeric) +
                     " is already declared as enumerator '" +
                     std::string(ExistingName) +
                     "'; declare the additional name as an explicit alias of "
                     "that canonical enumerator.");
}

ErrorDiagnostic UnknownAliasTargetDiagnostic(std::string_view Subject,
                                             std::string_view CanonicalName) {
  return Refusal(ErrorCategory::InvalidGlobalName, Subject,
                 "alias target '" + std::string(CanonicalName) +
                     "' is not a declared canonical enumerator of this "
                     "enumeration.");
}

ErrorDiagnostic ValueOutOfRangeDiagnostic(std::string_view Subject,
                                          std::string_view Domain,
                                          long long Numeric, long long Lowest,
                                          long long Highest) {
  return Refusal(ErrorCategory::Internal, Subject,
                 "value " + std::to_string(Numeric) + " does not fit " +
                     std::string(Domain) + " range [" + std::to_string(Lowest) +
                     ", " + std::to_string(Highest) +
                     "]; Luna never narrows, wraps, or rounds a declared "
                     "value to fit.");
}

ErrorDiagnostic UnsupportedFlagBitsDiagnostic(std::string_view Subject,
                                              long long Numeric,
                                              long long SupportedBits) {
  return Refusal(ErrorCategory::Internal, Subject,
                 "value " + std::to_string(Numeric) +
                     " carries a bit outside the declared supported mask " +
                     std::to_string(SupportedBits) +
                     "; Luna never truncates unsupported bits.");
}

ErrorDiagnostic MissingOptInDiagnostic(std::string_view Subject,
                                       std::string_view Reason) {
  return Refusal(ErrorCategory::Internal, Subject, Reason);
}

ErrorDiagnostic ReservedMemberNameDiagnostic(std::string_view Subject,
                                             std::string_view Segment) {
  return Refusal(ErrorCategory::DuplicateGlobalName, Subject,
                 "the member name '" + std::string(Segment) +
                     "' belongs to Luna's own metamethod and system namespace; "
                     "Luna never lets a declaration replace it.");
}

ErrorDiagnostic ReservedMetamethodDiagnostic(std::string_view Subject,
                                             std::string_view Segment,
                                             std::string_view RoleText) {
  return Refusal(ErrorCategory::DuplicateGlobalName, Subject,
                 "the member name '" + std::string(Segment) +
                     "' is a metamethod Luna owns for " +
                     std::string(RoleText) +
                     "; Luna never lets a declaration replace it.");
}

ErrorDiagnostic AmbiguousInheritedMemberDiagnostic(std::string_view Subject,
                                                   std::string_view Segment) {
  return Refusal(ErrorCategory::DuplicateGlobalName, Subject,
                 "the member name '" + std::string(Segment) +
                     "' is reachable from more than one base of this class, so "
                     "no single declaration owns it.");
}

std::optional<ErrorDiagnostic>
CheckRegistrationPreconditions(std::string_view GlobalName, bool StateReady,
                               bool HasCallableTarget, bool IsDuplicate) {
  if (auto Diagnostic = ValidateGlobalIdentifier(GlobalName))
    return Diagnostic;

  const std::string Context = GlobalSubject(GlobalName);

  if (!StateReady)
    return StateNotReadyDiagnostic(Context);

  if (!HasCallableTarget)
    return NullCallableDiagnostic(Context);

  if (IsDuplicate)
    return DuplicateNameDiagnostic(Context);

  return std::nullopt;
}

} // namespace Luna::Detail
