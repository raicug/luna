#pragma once

// clang-format off
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
// clang-format on

namespace Luna {

enum class SymbolKind {
  Namespace,
  Module,
  OverloadSet,
  FunctionCandidate,
  Constant,
  Enumeration,
  Enumerator,
  EnumeratorAlias,
  Class,
  Constructor,
  Factory,
  Method,
  StaticMethod,
  Property,
  Field,
  Operator,
  Type
};

[[nodiscard]] constexpr std::string_view
SymbolKindText(SymbolKind Kind) noexcept {
  switch (Kind) {
  case SymbolKind::Namespace:
    return "namespace";
  case SymbolKind::Module:
    return "module";
  case SymbolKind::OverloadSet:
    return "overload_set";
  case SymbolKind::FunctionCandidate:
    return "function_candidate";
  case SymbolKind::Constant:
    return "constant";
  case SymbolKind::Enumeration:
    return "enum";
  case SymbolKind::Enumerator:
    return "enumerator";
  case SymbolKind::EnumeratorAlias:
    return "enumerator_alias";
  case SymbolKind::Class:
    return "class";
  case SymbolKind::Constructor:
    return "constructor";
  case SymbolKind::Factory:
    return "factory";
  case SymbolKind::Method:
    return "method";
  case SymbolKind::StaticMethod:
    return "static_method";
  case SymbolKind::Property:
    return "property";
  case SymbolKind::Field:
    return "field";
  case SymbolKind::Operator:
    return "operator";
  case SymbolKind::Type:
    return "type";
  }
  return "unknown";
}

namespace Detail {

struct TypeIdentityTag {};
struct SymbolIdentityTag {};

template <class Tag> class StableIdentity {
public:
  static constexpr std::size_t BitCount = 256;
  static constexpr std::size_t ByteCount = BitCount / 8;
  static constexpr std::size_t TextLength = ByteCount * 2;

  using Storage = std::array<std::uint8_t, ByteCount>;

  constexpr StableIdentity() noexcept = default;

  [[nodiscard]] static constexpr StableIdentity
  FromBytes(const Storage &Bytes) noexcept {
    StableIdentity Identity;
    Identity.BytesValue = Bytes;
    return Identity;
  }

  [[nodiscard]] static std::optional<StableIdentity>
  Parse(std::string_view Text) noexcept {
    if (Text.size() != TextLength)
      return std::nullopt;
    Storage Bytes{};
    for (std::size_t Index = 0; Index < ByteCount; ++Index) {
      const std::optional<std::uint8_t> High = ParseDigit(Text[Index * 2]);
      const std::optional<std::uint8_t> Low = ParseDigit(Text[Index * 2 + 1]);
      if (!High || !Low)
        return std::nullopt;
      Bytes[Index] = static_cast<std::uint8_t>((*High << 4) | *Low);
    }
    return FromBytes(Bytes);
  }

  [[nodiscard]] constexpr const Storage &Bytes() const noexcept {
    return BytesValue;
  }

  [[nodiscard]] std::string ToString() const {
    std::string Text;
    Text.reserve(TextLength);
    for (const std::uint8_t Byte : BytesValue) {
      Text.push_back(FormatDigit(static_cast<std::uint8_t>(Byte >> 4)));
      Text.push_back(FormatDigit(static_cast<std::uint8_t>(Byte & 0x0f)));
    }
    return Text;
  }

  [[nodiscard]] constexpr bool IsValid() const noexcept {
    for (const std::uint8_t Byte : BytesValue) {
      if (Byte != 0)
        return true;
    }
    return false;
  }

  [[nodiscard]] constexpr std::size_t Hash() const noexcept {
    std::uint64_t Accumulator = 0xcbf29ce484222325ULL;
    for (const std::uint8_t Byte : BytesValue) {
      Accumulator ^= static_cast<std::uint64_t>(Byte);
      Accumulator *= 0x100000001b3ULL;
    }
    return static_cast<std::size_t>(Accumulator);
  }

  [[nodiscard]] friend constexpr bool
  operator==(const StableIdentity &Left, const StableIdentity &Right) noexcept {
    return Left.BytesValue == Right.BytesValue;
  }

  [[nodiscard]] friend constexpr std::strong_ordering
  operator<=>(const StableIdentity &Left,
              const StableIdentity &Right) noexcept {
    for (std::size_t Index = 0; Index < ByteCount; ++Index) {
      if (Left.BytesValue[Index] != Right.BytesValue[Index])
        return Left.BytesValue[Index] < Right.BytesValue[Index]
                   ? std::strong_ordering::less
                   : std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
  }

private:
  [[nodiscard]] static constexpr char
  FormatDigit(std::uint8_t Nibble) noexcept {
    return Nibble < 10 ? static_cast<char>('0' + Nibble)
                       : static_cast<char>('a' + (Nibble - 10));
  }

  [[nodiscard]] static constexpr std::optional<std::uint8_t>
  ParseDigit(char Character) noexcept {
    if (Character >= '0' && Character <= '9')
      return static_cast<std::uint8_t>(Character - '0');
    if (Character >= 'a' && Character <= 'f')
      return static_cast<std::uint8_t>(Character - 'a' + 10);
    return std::nullopt;
  }

  Storage BytesValue{};
};

} // namespace Detail

using TypeId = Detail::StableIdentity<Detail::TypeIdentityTag>;

using SymbolId = Detail::StableIdentity<Detail::SymbolIdentityTag>;

struct CanonicalHash {
  template <class Canonical>
  [[nodiscard]] std::size_t operator()(const Canonical &Value) const {
    return Value.Hash();
  }
};

} // namespace Luna
