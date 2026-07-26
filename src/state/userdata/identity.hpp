#pragma once

// Private identity vocabulary of one typed userdata value.
//
// None of these values is reachable from a consumer. A metatable identity is
// state-local and process-monotonic, so it identifies exactly one registered
// class of exactly one logical State without ever being an address. A native
// identity is Luna's own cache key: it may contain an address, but the address
// is always paired with a state-local nonce so a recycled allocation can never
// be mistaken for the object that previously lived there, and it never
// contributes to a `TypeId`, a `SymbolId`, reflection, a persisted diagnostic,
// or generated output.
//
// The ownership model, the lifetime state, and the const-access state are the
// three axes the release state machine moves along. They are declared here
// because both the userdata header and the per-State class registry describe
// them, and neither one may invent its own spelling.

// clang-format off
#include <compare>
#include <cstdint>
#include <string_view>
// clang-format on

namespace Luna::Detail {

// How one exposed object is owned. Borrowed storage is never deleted by Luna,
// a Lua-owned object is destroyed exactly once, and a shared object holds
// exactly one corresponding shared ownership reference.
enum class OwnershipModel : std::uint8_t { Borrowed, LuaOwned, Shared };

[[nodiscard]] std::string_view
OwnershipModelText(OwnershipModel Model) noexcept;

// Where one userdata is in the release state machine. Every transition runs
// through the one idempotent release gate, so a state is never skipped and
// never revisited.
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

// Whether the exposed view permits mutation. A const view accepts const methods
// and reads and rejects every mutating access.
enum class ConstAccess : std::uint8_t { Mutable, Const };

[[nodiscard]] std::string_view ConstAccessText(ConstAccess Access) noexcept;

// Identity of the one metatable a registered class owns in one logical State.
// It is state-local, monotonic, and never an address, so it is safe to store in
// a userdata header and to compare without touching the virtual machine.
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

// Luna's own cache key for one exposed native object. The address alone is
// never enough: the state-local nonce is what keeps a recycled allocation from
// impersonating a released object.
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

// The state-local nonce source of one State. It never restarts, so no two
// exposures of one State share a nonce even when the allocator reuses storage.
class NativeIdentitySource final {
public:
  [[nodiscard]] std::uint64_t Next() noexcept { return ++Counter; }

  [[nodiscard]] std::uint64_t Issued() const noexcept { return Counter; }

private:
  std::uint64_t Counter = 0;
};

} // namespace Luna::Detail
