// clang-format off
#include "state/invocation/parameters/argument_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

// Every live argument frame of the current thread, keyed by its Luna-owned
// token. Tokens are issued monotonically and never reused, so an ended frame's
// token resolves to nothing instead of to some later frame.
struct ArgumentFrameTable final {
  std::unordered_map<std::uint64_t, ArgumentFrame *> Frames;
  std::uint64_t NextToken = 1;
  std::size_t ExpiredAccessCount = 0;
};

[[nodiscard]] ArgumentFrameTable &Table() noexcept {
  static thread_local ArgumentFrameTable Frames;
  return Frames;
}

} // namespace

ArgumentFrame::ArgumentFrame(ArgumentPack Arguments)
    : ArgumentsValue(std::move(Arguments)) {
  ArgumentFrameTable &Frames = Table();
  TokenValue = Frames.NextToken++;
  Frames.Frames.emplace(TokenValue, this);
}

ArgumentFrame::~ArgumentFrame() { Deactivate(); }

void ArgumentFrame::Deactivate() noexcept {
  if (!ActiveValue)
    return;
  ActiveValue = false;
  Table().Frames.erase(TokenValue);
}

ArgumentView ArgumentFrame::View() const noexcept {
  return ArgumentView(TokenValue);
}

ArgumentFrame *FindArgumentFrame(std::uint64_t Token) noexcept {
  if (Token == 0)
    return nullptr;
  const ArgumentFrameTable &Frames = Table();
  const auto Found = Frames.Frames.find(Token);
  if (Found == Frames.Frames.end())
    return nullptr;
  ArgumentFrame *Frame = Found->second;
  return Frame && Frame->IsActive() ? Frame : nullptr;
}

void RecordExpiredArgumentAccess() noexcept { ++Table().ExpiredAccessCount; }

std::size_t ExpiredArgumentAccessCount() noexcept {
  return Table().ExpiredAccessCount;
}

void ResetArgumentBoundaryDiagnostics() noexcept {
  Table().ExpiredAccessCount = 0;
}

} // namespace Luna::Detail
