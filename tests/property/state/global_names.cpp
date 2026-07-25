// clang-format off
#include "state/binding/registration_checks.hpp"

#include <rapidcheck.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

constexpr std::string_view InitialCharacters =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_";
constexpr std::string_view LaterCharacters =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_0123456789";
constexpr std::array<unsigned char, 16> IllegalInitialCharacters = {
    0x00, ' ', '!', '-', '.', '/', '0', '5',
    '9',  ':', '@', '[', '`', '{', '~', 0x7F};
constexpr std::array<unsigned char, 16> IllegalLaterCharacters = {
    0x00, ' ', '!',  '-', '.', '/', ':', ';',
    '@',  '[', '\\', ']', '`', '{', '~', 0x7F};

[[nodiscard]] unsigned char GeneratedByte(const std::vector<int> &Values,
                                          std::size_t Index) noexcept {
  if (Values.empty())
    return static_cast<unsigned char>(Index * 37U + 11U);
  return static_cast<unsigned char>(
      static_cast<std::uint32_t>(Values[Index % Values.size()]) & 0xFFU);
}

[[nodiscard]] char GeneratedCharacter(std::string_view Alphabet,
                                      const std::vector<int> &Values,
                                      std::size_t Index) noexcept {
  return Alphabet[GeneratedByte(Values, Index) % Alphabet.size()];
}

[[nodiscard]] std::string ValidName(const std::vector<int> &Values,
                                    std::size_t Length) {
  std::string Name(Length, '_');
  Name.front() = GeneratedCharacter(InitialCharacters, Values, 0);
  for (std::size_t Index = 1; Index < Length; ++Index)
    Name[Index] = GeneratedCharacter(LaterCharacters, Values, Index);
  return Name;
}

[[nodiscard]] bool ReferenceAcceptsGlobalName(std::string_view Name) {
  if (Name.empty() || Name.size() > 255)
    return false;

  const auto IsAscii = [](char Character) {
    return static_cast<unsigned char>(Character) <= 0x7F;
  };
  const auto IsLetter = [](char Character) {
    return (Character >= 'A' && Character <= 'Z') ||
           (Character >= 'a' && Character <= 'z');
  };
  const auto IsInitial = [&](char Character) {
    return IsLetter(Character) || Character == '_';
  };
  const auto IsLater = [&](char Character) {
    return IsInitial(Character) || (Character >= '0' && Character <= '9');
  };

  return std::all_of(Name.begin(), Name.end(), IsAscii) &&
         IsInitial(Name.front()) &&
         std::all_of(Name.begin() + 1, Name.end(), IsLater);
}

[[nodiscard]] std::array<std::string, 8>
GeneratePartitionedNames(const std::vector<int> &Values) {
  std::string ArbitraryBytes;
  ArbitraryBytes.reserve(Values.size());
  for (std::size_t Index = 0; Index < Values.size(); ++Index)
    ArbitraryBytes.push_back(static_cast<char>(GeneratedByte(Values, Index)));

  const std::size_t OverlongLength = 256U + GeneratedByte(Values, 1);
  std::string Overlong = ValidName(Values, OverlongLength);

  const std::size_t NonAsciiLength = 1U + (GeneratedByte(Values, 2) % 255U);
  std::string NonAscii = ValidName(Values, NonAsciiLength);
  const std::size_t NonAsciiPosition =
      GeneratedByte(Values, 3) % NonAsciiLength;
  NonAscii[NonAsciiPosition] =
      static_cast<char>(0x80U | (GeneratedByte(Values, 4) & 0x7FU));

  const std::size_t IllegalFirstLength = 1U + (GeneratedByte(Values, 5) % 255U);
  std::string IllegalFirst = ValidName(Values, IllegalFirstLength);
  IllegalFirst.front() = static_cast<char>(
      IllegalInitialCharacters[GeneratedByte(Values, 6) %
                               IllegalInitialCharacters.size()]);

  const std::size_t IllegalLaterLength = 2U + (GeneratedByte(Values, 7) % 254U);
  std::string IllegalLater = ValidName(Values, IllegalLaterLength);
  const std::size_t IllegalLaterPosition =
      1U + (GeneratedByte(Values, 8) % (IllegalLaterLength - 1U));
  IllegalLater[IllegalLaterPosition] =
      static_cast<char>(IllegalLaterCharacters[GeneratedByte(Values, 9) %
                                               IllegalLaterCharacters.size()]);

  return {std::move(ArbitraryBytes), std::string{},
          std::move(Overlong),       std::move(NonAscii),
          std::move(IllegalFirst),   std::move(IllegalLater),
          ValidName(Values, 1),      ValidName(Values, 255)};
}

} // namespace

int RunGlobalNameGrammarProperties() {
  // **Validates: Requirements 3.3**
  // Feature: luau-binding-foundation, Property 3: Global name validation
  // matches the specified grammar
  const bool Passed = rc::check(
      "Global name validation matches the specified grammar",
      [](const std::vector<int> &GeneratedValues) {
        const auto Names = GeneratePartitionedNames(GeneratedValues);
        for (const auto &Name : Names) {
          const bool ExpectedAccepted = ReferenceAcceptsGlobalName(Name);
          const auto Diagnostic = Luna::Detail::CheckRegistrationPreconditions(
              Name, true, true, false);
          const bool ActualAccepted = !Diagnostic.has_value();

          RC_ASSERT(ActualAccepted == ExpectedAccepted);
          if (!ExpectedAccepted) {
            RC_ASSERT(Diagnostic.has_value());
            RC_ASSERT(Diagnostic->Category() ==
                      Luna::ErrorCategory::InvalidGlobalName);
          }
        }
      });

  return Passed ? 0 : 1;
}
