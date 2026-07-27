// clang-format off
#include "state/identity/digest.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr std::array<std::uint32_t, 64> RoundConstants{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

[[nodiscard]] constexpr std::uint32_t
RotateRight(std::uint32_t Value, std::uint32_t Count) noexcept {
  return static_cast<std::uint32_t>((Value >> Count) | (Value << (32 - Count)));
}

void CompressBlock(std::array<std::uint32_t, 8> &Accumulator,
                   const std::uint8_t *Data) noexcept {
  std::array<std::uint32_t, 64> Schedule{};
  for (std::size_t Index = 0; Index < 16; ++Index) {
    const std::size_t Base = Index * 4;
    Schedule[Index] = (static_cast<std::uint32_t>(Data[Base]) << 24) |
                      (static_cast<std::uint32_t>(Data[Base + 1]) << 16) |
                      (static_cast<std::uint32_t>(Data[Base + 2]) << 8) |
                      static_cast<std::uint32_t>(Data[Base + 3]);
  }
  for (std::size_t Index = 16; Index < 64; ++Index) {
    const std::uint32_t Left = Schedule[Index - 15];
    const std::uint32_t Right = Schedule[Index - 2];
    const std::uint32_t Sigma0 =
        RotateRight(Left, 7) ^ RotateRight(Left, 18) ^ (Left >> 3);
    const std::uint32_t Sigma1 =
        RotateRight(Right, 17) ^ RotateRight(Right, 19) ^ (Right >> 10);
    Schedule[Index] = static_cast<std::uint32_t>(Schedule[Index - 16] + Sigma0 +
                                                 Schedule[Index - 7] + Sigma1);
  }

  std::uint32_t A = Accumulator[0];
  std::uint32_t B = Accumulator[1];
  std::uint32_t C = Accumulator[2];
  std::uint32_t D = Accumulator[3];
  std::uint32_t E = Accumulator[4];
  std::uint32_t F = Accumulator[5];
  std::uint32_t G = Accumulator[6];
  std::uint32_t H = Accumulator[7];

  for (std::size_t Index = 0; Index < 64; ++Index) {
    const std::uint32_t Sigma1 =
        RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
    const std::uint32_t Choice = (E & F) ^ (~E & G);
    const std::uint32_t First = static_cast<std::uint32_t>(
        H + Sigma1 + Choice + RoundConstants[Index] + Schedule[Index]);
    const std::uint32_t Sigma0 =
        RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
    const std::uint32_t Majority = (A & B) ^ (A & C) ^ (B & C);
    const std::uint32_t Second = static_cast<std::uint32_t>(Sigma0 + Majority);

    H = G;
    G = F;
    F = E;
    E = static_cast<std::uint32_t>(D + First);
    D = C;
    C = B;
    B = A;
    A = static_cast<std::uint32_t>(First + Second);
  }

  Accumulator[0] += A;
  Accumulator[1] += B;
  Accumulator[2] += C;
  Accumulator[3] += D;
  Accumulator[4] += E;
  Accumulator[5] += F;
  Accumulator[6] += G;
  Accumulator[7] += H;
}

} // namespace

CanonicalDigest::Storage
CanonicalDigest::Compute(std::span<const std::uint8_t> Bytes) noexcept {
  std::array<std::uint32_t, 8> Accumulator{
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

  const std::size_t FullBlockCount = Bytes.size() / 64;
  for (std::size_t Block = 0; Block < FullBlockCount; ++Block)
    CompressBlock(Accumulator, Bytes.data() + Block * 64);

  const std::size_t RemainingCount = Bytes.size() % 64;
  std::array<std::uint8_t, 128> Tail{};
  for (std::size_t Index = 0; Index < RemainingCount; ++Index)
    Tail[Index] = Bytes[FullBlockCount * 64 + Index];
  Tail[RemainingCount] = 0x80;

  const std::size_t TailLength = RemainingCount < 56 ? 64 : 128;
  const std::uint64_t BitLength = static_cast<std::uint64_t>(Bytes.size()) * 8;
  for (std::size_t Index = 0; Index < 8; ++Index)
    Tail[TailLength - 1 - Index] =
        static_cast<std::uint8_t>((BitLength >> (Index * 8)) & 0xffULL);

  CompressBlock(Accumulator, Tail.data());
  if (TailLength == 128)
    CompressBlock(Accumulator, Tail.data() + 64);

  Storage Digest{};
  for (std::size_t Index = 0; Index < Accumulator.size(); ++Index) {
    Digest[Index * 4] =
        static_cast<std::uint8_t>((Accumulator[Index] >> 24) & 0xffu);
    Digest[Index * 4 + 1] =
        static_cast<std::uint8_t>((Accumulator[Index] >> 16) & 0xffu);
    Digest[Index * 4 + 2] =
        static_cast<std::uint8_t>((Accumulator[Index] >> 8) & 0xffu);
    Digest[Index * 4 + 3] =
        static_cast<std::uint8_t>(Accumulator[Index] & 0xffu);
  }
  return Digest;
}

} // namespace Luna::Detail
