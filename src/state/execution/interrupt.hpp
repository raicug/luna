#pragma once

// clang-format off
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
// clang-format on

struct lua_State;

namespace Luna::Detail {

inline constexpr std::string_view InterruptPrefix = "Execution interrupted:";

inline constexpr std::string_view DefaultInterruptReason =
    "the host requested a stop";

class InterruptRequest final {
public:
  static constexpr std::size_t MaximumComposedBytes = 512;

  InterruptRequest() = default;

  InterruptRequest(const InterruptRequest &) = delete;
  InterruptRequest &operator=(const InterruptRequest &) = delete;

  void Request(std::string Reason);
  void Clear() noexcept;

  [[nodiscard]] bool IsPending() const noexcept;

  [[nodiscard]] std::string Composed() const;

  [[nodiscard]] std::size_t CopyComposed(char *Target,
                                         std::size_t Capacity) const noexcept;

private:
  mutable std::mutex Guard;
  bool PendingValue = false;
  std::string ComposedValue;
};

void InstallInterruptCallback(lua_State *Root,
                              InterruptRequest *Pending) noexcept;

void ClearInterruptCallback(lua_State *Root) noexcept;

[[nodiscard]] InterruptRequest *
ObserveInterruptRequest(lua_State *State) noexcept;

} // namespace Luna::Detail
