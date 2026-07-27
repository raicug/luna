#pragma once

// clang-format off
#include <compare>
#include <cstdint>
#include <string_view>
// clang-format on

namespace Luna::Detail {

enum class OwnershipModel : std::uint8_t { Borrowed, LuaOwned, Shared };

[[nodiscard]] std::string_view
OwnershipModelText(OwnershipModel Model) noexcept;

enum class LifetimeState : std::uint8_t {
  Allocated,
  Constructed,
  Published,
  Invalid,
  Destroyed,
  SharedReleased,
  Released
};

[[nodiscard]] std::string_view LifetimeStateText(LifetimeState State) noexcept;

enum class ConstAccess : std::uint8_t { Mutable, Const };

[[nodiscard]] std::string_view ConstAccessText(ConstAccess Access) noexcept;

class MetatableId final {
public:
  constexpr MetatableId() noexcept = default;

  [[nodiscard]] static constexpr MetatableId
  FromValue(std::uint64_t Value) noexcept {
    MetatableId Identity;
    Identity.ValueStorage = Value;
    return Identity;
  }

  [[nodiscard]] constexpr std::uint64_t Value() const noexcept {
    return ValueStorage;
  }

  [[nodiscard]] constexpr bool IsValid() const noexcept {
    return ValueStorage != 0;
  }

  [[nodiscard]] friend constexpr bool
  operator==(const MetatableId &Left, const MetatableId &Right) noexcept {
    return Left.ValueStorage == Right.ValueStorage;
  }

  [[nodiscard]] friend constexpr std::strong_ordering
  operator<=>(const MetatableId &Left, const MetatableId &Right) noexcept {
    return Left.ValueStorage <=> Right.ValueStorage;
  }

private:
  std::uint64_t ValueStorage = 0;
};

struct NativeIdentity final {
  const void *Address = nullptr;
  std::uint64_t Nonce = 0;

  [[nodiscard]] constexpr bool IsValid() const noexcept {
    return Address != nullptr && Nonce != 0;
  }

  [[nodiscard]] friend constexpr bool
  operator==(const NativeIdentity &Left,
             const NativeIdentity &Right) noexcept = default;
};

class NativeIdentitySource final {
public:
  [[nodiscard]] std::uint64_t Next() noexcept { return ++Counter; }

  [[nodiscard]] std::uint64_t Issued() const noexcept { return Counter; }

private:
  std::uint64_t Counter = 0;
};

} // namespace Luna::Detail
