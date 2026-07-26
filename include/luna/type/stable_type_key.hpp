#pragma once

// Explicit stable identity for user-defined leaf types. A stable key is the
// only persistent identity Luna accepts for a user class or enum leaf: it never
// derives from an RTTI name, an address, a registration order, a locale, or a
// process-random value.

// clang-format off
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna {

// Deterministic reason a stable-key text is accepted or rejected.
enum class StableTypeKeyStatus {
  Valid,
  Empty,
  TooLong,
  EmptySegment,
  InvalidLeadingCharacter,
  InvalidCharacter,
  ReservedPrefix
};

[[nodiscard]] constexpr std::string_view
StableTypeKeyStatusText(StableTypeKeyStatus Status) noexcept {
  switch (Status) {
  case StableTypeKeyStatus::Valid:
    return "valid";
  case StableTypeKeyStatus::Empty:
    return "empty";
  case StableTypeKeyStatus::TooLong:
    return "too-long";
  case StableTypeKeyStatus::EmptySegment:
    return "empty-segment";
  case StableTypeKeyStatus::InvalidLeadingCharacter:
    return "invalid-leading-character";
  case StableTypeKeyStatus::InvalidCharacter:
    return "invalid-character";
  case StableTypeKeyStatus::ReservedPrefix:
    return "reserved-prefix";
  }
  return "invalid";
}

class StableTypeKey {
public:
  // Explicit Luna-owned policy: keys are ASCII, dot-separated identifier
  // segments and never exceed this many bytes.
  static constexpr std::size_t MaximumLength = 256;

  // Every fixed Luna-owned key lives under this prefix, so a user-defined leaf
  // can never claim a reserved identity.
  static constexpr std::string_view ReservedKeyPrefix = "luna.";

  static constexpr char SegmentSeparator = '.';

  StableTypeKey() = default;

  explicit StableTypeKey(std::string_view Text) : TextValue(Text) {}

  [[nodiscard]] static StableTypeKeyStatus
  Classify(std::string_view Text) noexcept {
    if (Text.empty())
      return StableTypeKeyStatus::Empty;
    if (Text.size() > MaximumLength)
      return StableTypeKeyStatus::TooLong;
    if (Text.size() >= ReservedKeyPrefix.size() &&
        Text.substr(0, ReservedKeyPrefix.size()) == ReservedKeyPrefix)
      return StableTypeKeyStatus::ReservedPrefix;

    bool AtSegmentStart = true;
    for (const char Character : Text) {
      if (Character == SegmentSeparator) {
        if (AtSegmentStart)
          return StableTypeKeyStatus::EmptySegment;
        AtSegmentStart = true;
        continue;
      }
      if (AtSegmentStart) {
        if (!IsLeadingCharacter(Character))
          return IsTrailingCharacter(Character)
                     ? StableTypeKeyStatus::InvalidLeadingCharacter
                     : StableTypeKeyStatus::InvalidCharacter;
        AtSegmentStart = false;
        continue;
      }
      if (!IsTrailingCharacter(Character))
        return StableTypeKeyStatus::InvalidCharacter;
    }
    if (AtSegmentStart)
      return StableTypeKeyStatus::EmptySegment;
    return StableTypeKeyStatus::Valid;
  }

  [[nodiscard]] static bool IsValidText(std::string_view Text) noexcept {
    return Classify(Text) == StableTypeKeyStatus::Valid;
  }

  [[nodiscard]] const std::string &Text() const noexcept { return TextValue; }

  [[nodiscard]] StableTypeKeyStatus Status() const noexcept {
    return Classify(TextValue);
  }

  [[nodiscard]] bool IsValid() const noexcept {
    return Status() == StableTypeKeyStatus::Valid;
  }

  [[nodiscard]] bool IsEmpty() const noexcept { return TextValue.empty(); }

  // Locale-independent FNV-1a over the exact key bytes.
  [[nodiscard]] std::size_t Hash() const noexcept {
    std::uint64_t Accumulator = 0xcbf29ce484222325ULL;
    for (const char Character : TextValue) {
      Accumulator ^=
          static_cast<std::uint64_t>(static_cast<unsigned char>(Character));
      Accumulator *= 0x100000001b3ULL;
    }
    return static_cast<std::size_t>(Accumulator);
  }

  [[nodiscard]] friend bool operator==(const StableTypeKey &Left,
                                       const StableTypeKey &Right) noexcept {
    return Left.TextValue == Right.TextValue;
  }

  [[nodiscard]] friend std::strong_ordering
  operator<=>(const StableTypeKey &Left, const StableTypeKey &Right) noexcept {
    const int Comparison = Left.TextValue.compare(Right.TextValue);
    if (Comparison < 0)
      return std::strong_ordering::less;
    if (Comparison > 0)
      return std::strong_ordering::greater;
    return std::strong_ordering::equal;
  }

private:
  [[nodiscard]] static constexpr bool
  IsLeadingCharacter(char Character) noexcept {
    return (Character >= 'A' && Character <= 'Z') ||
           (Character >= 'a' && Character <= 'z') || Character == '_';
  }

  [[nodiscard]] static constexpr bool
  IsTrailingCharacter(char Character) noexcept {
    return IsLeadingCharacter(Character) ||
           (Character >= '0' && Character <= '9');
  }

  std::string TextValue;
};

} // namespace Luna
