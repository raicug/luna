// clang-format off
#include "state/binding/registration_checks.hpp"

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

[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateGlobalName(std::string_view GlobalName) {
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

[[nodiscard]] std::string GlobalContext(std::string_view GlobalName) {
  return "global '" + std::string(GlobalName) + "'";
}

} // namespace

std::optional<ErrorDiagnostic>
CheckRegistrationPreconditions(std::string_view GlobalName, bool StateReady,
                               bool HasCallableTarget, bool IsDuplicate) {
  if (auto Diagnostic = ValidateGlobalName(GlobalName))
    return Diagnostic;

  if (!StateReady)
    return ErrorDiagnostic::Create(ErrorCategory::StateNotReady,
                                   "Cannot register " +
                                       GlobalContext(GlobalName) +
                                       ": State is not ready.");

  if (!HasCallableTarget)
    return ErrorDiagnostic::Create(ErrorCategory::NullCallable,
                                   "Cannot register " +
                                       GlobalContext(GlobalName) +
                                       ": callable target is null.");

  if (IsDuplicate)
    return ErrorDiagnostic::Create(ErrorCategory::DuplicateGlobalName,
                                   "Cannot register " +
                                       GlobalContext(GlobalName) +
                                       ": name is already registered.");

  return std::nullopt;
}

} // namespace Luna::Detail
