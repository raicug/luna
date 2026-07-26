#pragma once

// The private bridge behind the public variadic boundary.
//
// An `ArgumentFrame` is the transient scope Luna opens around one native
// invocation that declares a variadic parameter. It owns the complete, already
// converted argument pack, hands out `Luna::ArgumentView` tokens naming that
// owned copy, and answers every accessor the public view exposes.
//
// Because the pack is owned before any native code sees it, a view can never
// reach virtual-machine storage, and because the token is a plain Luna-owned
// number there is nothing in it to turn back into a pointer, a stack index, or
// a registry reference. A view that outlives its frame answers as an inert
// empty view and the attempt is counted, so retaining one is detectable instead
// of dangerous.
//
// This header names no virtual-machine type at all.

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

  // The callback-lifetime view of this frame's arguments.
  [[nodiscard]] ArgumentView View() const noexcept;

  // The owning pack, for a parameter that retains its arguments.
  [[nodiscard]] const ArgumentPack &Arguments() const noexcept {
    return ArgumentsValue;
  }

  // End the frame. Every outstanding view becomes inert.
  void Deactivate() noexcept;

private:
  ArgumentPack ArgumentsValue;
  std::uint64_t TokenValue = 0;
  bool ActiveValue = true;
};

// Resolve one frame token. An unknown or ended token resolves to nothing, which
// is what makes a retained view inert instead of dangerous.
[[nodiscard]] ArgumentFrame *FindArgumentFrame(std::uint64_t Token) noexcept;

// Luna-owned boundary guard: an access through a view whose frame already
// ended.
void RecordExpiredArgumentAccess() noexcept;
[[nodiscard]] std::size_t ExpiredArgumentAccessCount() noexcept;
void ResetArgumentBoundaryDiagnostics() noexcept;

} // namespace Luna::Detail
