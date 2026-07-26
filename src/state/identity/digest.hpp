#pragma once

// Version-pinned 256-bit digest used by every Luna canonical identity. The
// algorithm and its version are part of Luna's identity contract and are
// covered by fixed test vectors, so an identity never depends on an
// implementation-defined hash, an address, or a process-random seed.

// clang-format off
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
// clang-format on

namespace Luna::Detail {

class CanonicalDigest final {
public:
  // Pinned SHA-256. Changing the algorithm requires changing this version and
  // the canonical schema version together.
  static constexpr std::uint32_t AlgorithmVersion = 1;
  static constexpr std::size_t ByteCount = 32;

  using Storage = std::array<std::uint8_t, ByteCount>;

  [[nodiscard]] static Storage
  Compute(std::span<const std::uint8_t> Bytes) noexcept;
};

} // namespace Luna::Detail
