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
#include <new>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
// clang-format on

namespace Luna {

class ReturnPack final {
public:
  ReturnPack() = default;

  explicit ReturnPack(std::vector<Value> Values)
      : HeapValues(std::move(Values)), UsesHeapValues(true) {}

  ReturnPack(const ReturnPack &Source)
      : HeapValues(Source.HeapValues),
        OwnedValuesValue(Source.OwnedValuesValue),
        UsesHeapValues(Source.UsesHeapValues),
        CarriesOwnedValue(Source.CarriesOwnedValue) {
    try {
      CopyInlineValues(Source);
    } catch (...) {
      DestroyInlineValues();
      throw;
    }
  }

  ReturnPack(ReturnPack &&Source) noexcept(
      std::is_nothrow_move_constructible_v<Value> &&
      std::is_nothrow_move_constructible_v<std::vector<Value>> &&
      std::is_nothrow_move_constructible_v<std::vector<OwnedValue>>)
      : HeapValues(std::move(Source.HeapValues)),
        OwnedValuesValue(std::move(Source.OwnedValuesValue)),
        UsesHeapValues(Source.UsesHeapValues),
        CarriesOwnedValue(Source.CarriesOwnedValue) {
    MoveInlineValues(Source);
    Source.ResetMovedFrom();
  }

  ReturnPack &operator=(const ReturnPack &Source) {
    if (this == &Source)
      return *this;
    ReturnPack Copied(Source);
    return *this = std::move(Copied);
  }

  ReturnPack &operator=(ReturnPack &&Source) noexcept(
      std::is_nothrow_move_assignable_v<Value> &&
      std::is_nothrow_move_assignable_v<std::vector<Value>> &&
      std::is_nothrow_move_assignable_v<std::vector<OwnedValue>>) {
    if (this == &Source)
      return *this;
    Clear();
    HeapValues = std::move(Source.HeapValues);
    OwnedValuesValue = std::move(Source.OwnedValuesValue);
    UsesHeapValues = Source.UsesHeapValues;
    CarriesOwnedValue = Source.CarriesOwnedValue;
    MoveInlineValues(Source);
    Source.ResetMovedFrom();
    return *this;
  }

  ~ReturnPack() { DestroyInlineValues(); }

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
    return std::span<const Value>(InlineData(), InlineSize);
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
      ConstructInline(std::move(Element));
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
    if (CarriesOwnedValue) {
      OwnedValuesValue.push_back(std::move(Element));
      return;
    }

    std::vector<OwnedValue> Staged;
    Staged.reserve(Size() + 1);
    for (const Value &Existing : Values())
      Staged.push_back(OwnedValue::FromValue(Existing));
    Staged.push_back(std::move(Element));

    DestroyInlineValues();
    HeapValues.clear();
    UsesHeapValues = false;
    OwnedValuesValue = std::move(Staged);
    CarriesOwnedValue = true;
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

    std::vector<Value> Produced;
    Produced.reserve(InlineSize);
    for (std::size_t Index = 0; Index < InlineSize; ++Index)
      Produced.push_back(std::move(InlineAt(Index)));
    DestroyInlineValues();
    return Produced;
  }

  [[nodiscard]] ValuePack TakeOwnedValues() && noexcept {
    CarriesOwnedValue = false;
    return ValuePack(std::move(OwnedValuesValue));
  }

  void Clear() noexcept {
    DestroyInlineValues();
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

  [[nodiscard]] Value *InlineData() noexcept {
    return reinterpret_cast<Value *>(InlineStorage.data());
  }

  [[nodiscard]] const Value *InlineData() const noexcept {
    return reinterpret_cast<const Value *>(InlineStorage.data());
  }

  [[nodiscard]] Value &InlineAt(std::size_t Index) noexcept {
    return *std::launder(InlineData() + Index);
  }

  [[nodiscard]] const Value &InlineAt(std::size_t Index) const noexcept {
    return *std::launder(InlineData() + Index);
  }

  template <class... Arguments> void ConstructInline(Arguments &&...Source) {
    std::construct_at(InlineData() + InlineSize,
                      std::forward<Arguments>(Source)...);
    ++InlineSize;
  }

  void DestroyInlineValues() noexcept {
    while (InlineSize != 0)
      std::destroy_at(InlineData() + --InlineSize);
  }

  void CopyInlineValues(const ReturnPack &Source) {
    for (std::size_t Index = 0; Index < Source.InlineSize; ++Index)
      ConstructInline(Source.InlineAt(Index));
  }

  void MoveInlineValues(ReturnPack &Source) noexcept(
      std::is_nothrow_move_constructible_v<Value>) {
    for (std::size_t Index = 0; Index < Source.InlineSize; ++Index)
      ConstructInline(std::move(Source.InlineAt(Index)));
    Source.DestroyInlineValues();
  }

  void ResetMovedFrom() noexcept {
    InlineSize = 0;
    UsesHeapValues = false;
    CarriesOwnedValue = false;
  }

  template <class Type, class... Arguments>
  ReturnPack &AppendDirect(Arguments &&...Source) {
    if (CarriesOwnedValue) {
      Append(
          Value(std::in_place_type<Type>, std::forward<Arguments>(Source)...));
      return *this;
    }
    if (!UsesHeapValues && InlineSize < InlineCapacity) {
      ConstructInline(std::in_place_type<Type>,
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
      HeapValues.push_back(std::move(InlineAt(Index)));
    DestroyInlineValues();
    UsesHeapValues = true;
  }

  alignas(Value)
      std::array<std::byte, InlineCapacity * sizeof(Value)> InlineStorage;
  std::size_t InlineSize = 0;
  std::vector<Value> HeapValues;
  std::vector<OwnedValue> OwnedValuesValue;
  bool UsesHeapValues = false;
  bool CarriesOwnedValue = false;
};

} // namespace Luna
