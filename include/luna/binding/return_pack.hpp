#pragma once

// clang-format off
#include <luna/binding/class_construction.hpp>
#include <luna/binding/conversion.hpp>
#include <luna/binding/registered_class.hpp>
#include <luna/binding/value.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <memory>
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
      : HeapValues(std::move(Values)), UsesHeapValues(true) {}

  [[nodiscard]] static ReturnPack Empty() { return ReturnPack(); }

  [[nodiscard]] std::size_t Size() const noexcept {
    if (CarriesOwnedValue)
      return OwnedValuesValue.size();
    return UsesHeapValues ? HeapValues.size() : InlineSize;
  }

  [[nodiscard]] bool IsEmpty() const noexcept { return Size() == 0; }

  [[nodiscard]] const Value *At(std::size_t Index) const noexcept {
    const std::span<const Value> Present = Values();
    return Index < Present.size() ? &Present[Index] : nullptr;
  }

  [[nodiscard]] std::span<const Value> Values() const noexcept {
    if (UsesHeapValues)
      return HeapValues;
    return std::span<const Value>(InlineValues.data(), InlineSize);
  }

  [[nodiscard]] std::size_t Position(std::size_t Index) const noexcept {
    return Index + 1;
  }

  void Append(Value Element) {
    if (CarriesOwnedValue) {
      OwnedValuesValue.push_back(OwnedValue::FromValue(Element));
      return;
    }
    if (!UsesHeapValues && InlineSize < InlineCapacity) {
      InlineValues[InlineSize++] = std::move(Element);
      return;
    }
    PromoteInlineValues();
    HeapValues.push_back(std::move(Element));
  }

  ReturnPack &AppendBoolean(bool Element) {
    return AppendDirect<bool>(Element);
  }

  ReturnPack &AppendInteger(int Element) { return AppendDirect<int>(Element); }

  ReturnPack &AppendNumber(double Element) {
    return AppendDirect<double>(Element);
  }

  ReturnPack &AppendText(std::string Element) {
    return AppendDirect<std::string>(std::move(Element));
  }

  template <class Type> ReturnPack &AppendInstance(Type Element) {
    AppendOwned(OwnedValue::Instance<Type>(std::move(Element)));
    return *this;
  }

  template <class Type>
  ReturnPack &AppendInstance(std::shared_ptr<Type> Element) {
    AppendOwned(OwnedValue::Instance<Type>(std::move(Element)));
    return *this;
  }

  template <class Type>
  ReturnPack &AppendInstance(Type *Element, OwnershipPolicy Declared) {
    AppendOwned(OwnedValue::Instance<Type>(Element, std::move(Declared)));
    return *this;
  }

  void AppendOwned(OwnedValue Element) {
    if (!CarriesOwnedValue) {
      const std::size_t ValueCount = Size();
      CarriesOwnedValue = true;
      OwnedValuesValue.reserve(ValueCount + 1);
      for (const Value &Existing : Values())
        OwnedValuesValue.push_back(OwnedValue::FromValue(Existing));
      InlineSize = 0;
      HeapValues.clear();
      UsesHeapValues = false;
    }
    OwnedValuesValue.push_back(std::move(Element));
  }

  [[nodiscard]] bool CarriesOwnedValues() const noexcept {
    return CarriesOwnedValue;
  }

  [[nodiscard]] ValuePack ToOwnedValues() const {
    ValuePack Produced;
    for (const OwnedValue &Element : OwnedValuesValue)
      Produced.Append(Element);
    return Produced;
  }

  [[nodiscard]] std::vector<Value> TakeValues() && {
    CarriesOwnedValue = false;
    if (UsesHeapValues)
      return std::move(HeapValues);
    return std::vector<Value>(
        std::make_move_iterator(InlineValues.begin()),
        std::make_move_iterator(InlineValues.begin() + InlineSize));
  }

  [[nodiscard]] ValuePack TakeOwnedValues() && noexcept {
    CarriesOwnedValue = false;
    return ValuePack(std::move(OwnedValuesValue));
  }

  void Clear() noexcept {
    InlineSize = 0;
    HeapValues.clear();
    UsesHeapValues = false;
    OwnedValuesValue.clear();
    CarriesOwnedValue = false;
  }

  [[nodiscard]] friend bool operator==(const ReturnPack &Left,
                                       const ReturnPack &Right) {
    const std::span<const Value> LeftValues = Left.Values();
    const std::span<const Value> RightValues = Right.Values();
    return Left.CarriesOwnedValue == Right.CarriesOwnedValue &&
           LeftValues.size() == RightValues.size() &&
           std::equal(LeftValues.begin(), LeftValues.end(),
                      RightValues.begin()) &&
           Left.OwnedValuesValue == Right.OwnedValuesValue;
  }

  [[nodiscard]] friend bool operator!=(const ReturnPack &Left,
                                       const ReturnPack &Right) {
    return !(Left == Right);
  }

private:
  static constexpr std::size_t InlineCapacity = 3;

  template <class Type, class... Arguments>
  ReturnPack &AppendDirect(Arguments &&...Source) {
    if (CarriesOwnedValue) {
      Append(
          Value(std::in_place_type<Type>, std::forward<Arguments>(Source)...));
      return *this;
    }
    if (!UsesHeapValues && InlineSize < InlineCapacity) {
      InlineValues[InlineSize++].template emplace<Type>(
          std::forward<Arguments>(Source)...);
      return *this;
    }
    PromoteInlineValues();
    HeapValues.emplace_back(std::in_place_type<Type>,
                            std::forward<Arguments>(Source)...);
    return *this;
  }

  void PromoteInlineValues() {
    if (UsesHeapValues)
      return;
    HeapValues.reserve(InlineSize + 1);
    for (std::size_t Index = 0; Index < InlineSize; ++Index)
      HeapValues.push_back(std::move(InlineValues[Index]));
    InlineSize = 0;
    UsesHeapValues = true;
  }

  std::array<Value, InlineCapacity> InlineValues;
  std::size_t InlineSize = 0;
  std::vector<Value> HeapValues;
  std::vector<OwnedValue> OwnedValuesValue;
  bool UsesHeapValues = false;
  bool CarriesOwnedValue = false;
};

} // namespace Luna
