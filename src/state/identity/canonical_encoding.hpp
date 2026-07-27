#pragma once

// clang-format off
#include <luna/type/type_descriptor.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

struct SymbolDescriptor;

inline constexpr std::uint32_t CanonicalSchemaVersion = 1;

enum class CanonicalDomain : std::uint8_t { Type = 1, Symbol = 2 };

class CanonicalEncoder final {
public:
  CanonicalEncoder() = default;

  void WriteRootTag(CanonicalDomain Domain);

  void WriteTag(std::uint8_t Tag);
  void WriteUnsigned(std::uint64_t Value);
  void WriteFlag(bool Value);

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

[[nodiscard]] std::vector<std::uint8_t>
EncodeCanonicalType(const TypeDescriptor &Descriptor);

[[nodiscard]] std::vector<std::uint8_t>
EncodeCanonicalSymbol(const SymbolDescriptor &Descriptor);

} // namespace Luna::Detail
