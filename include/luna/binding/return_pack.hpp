#pragma once

// Ordered multiple return values at the public boundary.
//
// A native callable produces its returns in exactly one of three shapes, and
// every one of them is described here or by the return metadata built from it:
//
//   * `void` produces zero values.
//   * One supported scalar produces exactly one value.
//   * `std::pair`, `std::tuple`, and `ReturnPack` produce ordered multiple
//     values, in declaration order.
//
// `ReturnPack` is the dynamic form: the element count is decided by the
// invocation rather than by the signature, which is what a callable needs when
// the number of returns depends on its arguments. It owns every value it
// reports, holds no virtual-machine resource, and exposes no stack, index, or
// registry reference.
//
// A pack is only staging. Luna converts and validates every element of it
// before any return is exposed, so a refused element publishes zero values
// rather than a partial pack.

// clang-format off
#include <luna/binding/value.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>
// clang-format on

namespace Luna {

// One owning ordered pack of return values.
class ReturnPack final {
public:
  ReturnPack() = default;

  explicit ReturnPack(std::vector<Value> Values)
      : ValuesValue(std::move(Values)) {}

  [[nodiscard]] static ReturnPack Empty() { return ReturnPack(); }

  [[nodiscard]] std::size_t Size() const noexcept { return ValuesValue.size(); }

  [[nodiscard]] bool IsEmpty() const noexcept { return ValuesValue.empty(); }

  // Element `Index`, or null when the pack has no such position.
  [[nodiscard]] const Value *At(std::size_t Index) const noexcept {
    if (Index >= ValuesValue.size())
      return nullptr;
    return &ValuesValue[Index];
  }

  [[nodiscard]] std::span<const Value> Values() const noexcept {
    return ValuesValue;
  }

  // One-based return position of element `Index`, which is what a refused
  // element's diagnostic reports.
  [[nodiscard]] std::size_t Position(std::size_t Index) const noexcept {
    return Index + 1;
  }

  void Append(Value Element) { ValuesValue.push_back(std::move(Element)); }

  // Typed appends. They exist because the pinned foundation variant would
  // otherwise let a literal choose a surprising alternative, so every element a
  // callable publishes names its own canonical type.
  ReturnPack &AppendBoolean(bool Element) {
    ValuesValue.emplace_back(Element);
    return *this;
  }

  ReturnPack &AppendInteger(int Element) {
    ValuesValue.emplace_back(Element);
    return *this;
  }

  ReturnPack &AppendNumber(double Element) {
    ValuesValue.emplace_back(Element);
    return *this;
  }

  ReturnPack &AppendText(std::string Element) {
    ValuesValue.emplace_back(std::move(Element));
    return *this;
  }

  void Clear() noexcept { ValuesValue.clear(); }

  [[nodiscard]] friend bool operator==(const ReturnPack &Left,
                                       const ReturnPack &Right) {
    return Left.ValuesValue == Right.ValuesValue;
  }

  [[nodiscard]] friend bool operator!=(const ReturnPack &Left,
                                       const ReturnPack &Right) {
    return !(Left == Right);
  }

private:
  std::vector<Value> ValuesValue;
};

} // namespace Luna
