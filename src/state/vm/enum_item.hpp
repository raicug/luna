#pragma once

// clang-format off
#include <luna/reflection/ids.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
// clang-format on

struct lua_State;

namespace Luna::Detail {

inline constexpr std::uint32_t EnumItemMagic = 0x4C554E45U;

inline constexpr std::uint16_t EnumItemLayoutVersion = 1;

inline constexpr std::string_view EnumItemTypeName = "EnumItem";

struct EnumItemPayload final {
  std::uint32_t Magic = EnumItemMagic;
  std::uint16_t LayoutVersion = EnumItemLayoutVersion;

  TypeId Enumeration;
  std::int64_t Numeric = 0;

  [[nodiscard]] constexpr bool HasCanonicalLayout() const noexcept {
    return Magic == EnumItemMagic && LayoutVersion == EnumItemLayoutVersion;
  }
};

static_assert(std::is_trivially_copyable_v<EnumItemPayload>,
              "An enumerator object lives in virtual-machine owned memory and "
              "must stay trivially copyable.");

[[nodiscard]] const EnumItemPayload *
InspectEnumItem(const void *Block, std::size_t ByteCount) noexcept;

class EnumItemRegistry final {
public:
  EnumItemRegistry() = default;
  ~EnumItemRegistry();

  EnumItemRegistry(const EnumItemRegistry &) = delete;
  EnumItemRegistry &operator=(const EnumItemRegistry &) = delete;

  void Bind(lua_State *Root) noexcept;

  [[nodiscard]] bool Publish(lua_State *State, const TypeId &Enumeration,
                             std::int64_t Numeric,
                             std::string_view EnumerationName,
                             std::string_view Name);

  void Retire() noexcept;

  [[nodiscard]] std::size_t InternedCount() const noexcept {
    return Items.size();
  }

private:
  struct Interned final {
    TypeId Enumeration;
    std::int64_t Numeric = 0;
    int Reference = 0;
  };

  [[nodiscard]] const Interned *Find(const TypeId &Enumeration,
                                     std::int64_t Numeric) const noexcept;

  lua_State *Thread = nullptr;
  std::vector<Interned> Items;
};

[[nodiscard]] bool PublishEnumItemRegistry(lua_State *State,
                                           EnumItemRegistry *Registry) noexcept;

[[nodiscard]] EnumItemRegistry *
ObserveEnumItemRegistry(lua_State *State) noexcept;

} // namespace Luna::Detail
