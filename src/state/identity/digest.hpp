#pragma once

// clang-format off
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
// clang-format on

namespace Luna::Detail {

class CanonicalDigest final {
public:
  static constexpr std::uint32_t AlgorithmVersion = 1;
  static constexpr std::size_t ByteCount = 32;

  using Storage = std::array<std::uint8_t, ByteCount>;

  [[nodiscard]] static Storage
  Compute(std::span<const std::uint8_t> Bytes) noexcept;
};

} // namespace Luna::Detail
