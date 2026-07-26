#pragma once

// Versioned, length-delimited canonical encoder. Every canonical byte sequence
// starts with the schema version and a domain tag, and every variable-length
// component is written with an explicit length, so two different descriptors
// can never produce the same bytes by concatenation. The encoder uses no
// locale-sensitive text formatting, no RTTI name, no address, and no
// registration order.

// clang-format off
#include <luna/type/type_descriptor.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

struct SymbolDescriptor;

// Canonical descriptor schema version. It is pinned into every encoding, so a
// future schema change never silently reuses a previous identity.
inline constexpr std::uint32_t CanonicalSchemaVersion = 1;

// Identity domains keep a type encoding and a symbol encoding disjoint even
// when their remaining bytes would otherwise agree.
enum class CanonicalDomain : std::uint8_t { Type = 1, Symbol = 2 };

class CanonicalEncoder final {
public:
  CanonicalEncoder() = default;

  // Writes the schema version and domain tag that open every canonical root
  // sequence.
  void WriteRootTag(CanonicalDomain Domain);

  void WriteTag(std::uint8_t Tag);
  void WriteUnsigned(std::uint64_t Value);
  void WriteFlag(bool Value);

  // Length-delimited components.
  void WriteText(std::string_view Text);
  void WriteBlock(std::span<const std::uint8_t> Block);

  [[nodiscard]] const std::vector<std::uint8_t> &Bytes() const noexcept {
    return BytesValue;
  }

  [[nodiscard]] std::vector<std::uint8_t> Release() noexcept {
    return std::move(BytesValue);
  }

private:
  std::vector<std::uint8_t> BytesValue;
};

// Canonical bytes of one normalized type descriptor. An incomplete descriptor
// produces an empty sequence and therefore never receives an identity.
[[nodiscard]] std::vector<std::uint8_t>
EncodeCanonicalType(const TypeDescriptor &Descriptor);

// Canonical bytes of one complete symbol identity. An incomplete descriptor
// produces an empty sequence and therefore never receives an identity.
[[nodiscard]] std::vector<std::uint8_t>
EncodeCanonicalSymbol(const SymbolDescriptor &Descriptor);

} // namespace Luna::Detail
