// clang-format off
#include <luna/binding/argument_pack.hpp>

#include "state/invocation/parameters/argument_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
// clang-format on

namespace Luna {
namespace {

[[nodiscard]] const Detail::ArgumentFrame *
ResolveFrame(std::uint64_t Token) noexcept {
  const Detail::ArgumentFrame *Frame = Detail::FindArgumentFrame(Token);
  if (!Frame)
    Detail::RecordExpiredArgumentAccess();
  return Frame;
}

} // namespace

bool ArgumentView::IsActive() const noexcept {
  return Detail::FindArgumentFrame(FrameTokenValue) != nullptr;
}

std::size_t ArgumentView::Size() const noexcept {
  const Detail::ArgumentFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return 0;
  const ArgumentPack &Arguments = Frame->Arguments();
  return Arguments.Size();
}

bool ArgumentView::IsEmpty() const noexcept { return Size() == 0; }

std::size_t ArgumentView::Position(std::size_t Index) const noexcept {
  const Detail::ArgumentFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return 0;
  const ArgumentPack &Arguments = Frame->Arguments();
  return Arguments.Position(Index);
}

std::size_t ArgumentView::FirstPosition() const noexcept {
  const Detail::ArgumentFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return 0;
  const ArgumentPack &Arguments = Frame->Arguments();
  return Arguments.FirstPosition();
}

ValueCategory ArgumentView::Kind(std::size_t Index) const noexcept {
  const OwnedValue Element = At(Index);
  return Element.Kind();
}

bool ArgumentView::IsNil(std::size_t Index) const noexcept {
  return Kind(Index) == ValueCategory::Nil;
}

std::optional<bool> ArgumentView::ToBoolean(std::size_t Index) const noexcept {
  const OwnedValue Element = At(Index);
  return Element.ToBoolean();
}

std::optional<double> ArgumentView::ToNumber(std::size_t Index) const noexcept {
  const OwnedValue Element = At(Index);
  return Element.ToNumber();
}

std::optional<std::string> ArgumentView::ToText(std::size_t Index) const {
  const OwnedValue Element = At(Index);
  return Element.ToText();
}

OwnedValue ArgumentView::At(std::size_t Index) const {
  const Detail::ArgumentFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return OwnedValue();
  const ArgumentPack &Arguments = Frame->Arguments();
  return Arguments.At(Index);
}

std::string ArgumentView::Path(std::size_t Index) const {
  const Detail::ArgumentFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return std::string();
  const ArgumentPack &Arguments = Frame->Arguments();
  if (Index >= Arguments.Size())
    return std::string();
  return "argument " + std::to_string(Arguments.Position(Index));
}

ArgumentPack ArgumentView::ToOwned() const {
  const Detail::ArgumentFrame *Frame = ResolveFrame(FrameTokenValue);
  if (!Frame)
    return ArgumentPack();
  return Frame->Arguments();
}

} // namespace Luna
