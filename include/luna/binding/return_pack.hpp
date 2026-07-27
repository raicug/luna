#pragma once

// clang-format off
#include <luna/binding/value.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>
// clang-format on

namespace Luna {

class ReturnPack final {
public:
  ReturnPack() = default;

  explicit ReturnPack(std::vector<Value> Values)
      : ValuesValue(std::move(Values)) {}

  [[nodiscard]] static ReturnPack Empty() { return ReturnPack(); }

  [[nodiscard]] std::size_t Size() const noexcept { return ValuesValue.size(); }

  [[nodiscard]] bool IsEmpty() const noexcept { return ValuesValue.empty(); }

  [[nodiscard]] const Value *At(std::size_t Index) const noexcept {
    if (Index >= ValuesValue.size())
      return nullptr;
    return &ValuesValue[Index];
  }

  [[nodiscard]] std::span<const Value> Values() const noexcept {
    return ValuesValue;
  }

  [[nodiscard]] std::size_t Position(std::size_t Index) const noexcept {
    return Index + 1;
  }

  void Append(Value Element) { ValuesValue.push_back(std::move(Element)); }

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
