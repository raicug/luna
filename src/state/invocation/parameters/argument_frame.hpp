#pragma once

// clang-format off
#include <luna/binding/argument_pack.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>
// clang-format on

namespace Luna::Detail {

class ArgumentFrame final {
public:
  explicit ArgumentFrame(ArgumentPack Arguments);
  ~ArgumentFrame();

  ArgumentFrame(const ArgumentFrame &) = delete;
  ArgumentFrame &operator=(const ArgumentFrame &) = delete;
  ArgumentFrame(ArgumentFrame &&) = delete;
  ArgumentFrame &operator=(ArgumentFrame &&) = delete;

  [[nodiscard]] std::uint64_t Token() const noexcept { return TokenValue; }
  [[nodiscard]] bool IsActive() const noexcept { return ActiveValue; }

  [[nodiscard]] ArgumentView View() const noexcept;

  [[nodiscard]] const ArgumentPack &Arguments() const noexcept {
    return ArgumentsValue;
  }

  void Deactivate() noexcept;

private:
  ArgumentPack ArgumentsValue;
  std::uint64_t TokenValue = 0;
  bool ActiveValue = true;
};

[[nodiscard]] ArgumentFrame *FindArgumentFrame(std::uint64_t Token) noexcept;

void RecordExpiredArgumentAccess() noexcept;
[[nodiscard]] std::size_t ExpiredArgumentAccessCount() noexcept;
void ResetArgumentBoundaryDiagnostics() noexcept;

} // namespace Luna::Detail
