#pragma once

// clang-format off
#include <luna/binding/class_construction.hpp>
#include <luna/binding/conversion.hpp>
#include <luna/binding/registered_class.hpp>
#include <luna/binding/value.hpp>

#include <cstddef>
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
      : ValuesValue(std::move(Values)) {}

  [[nodiscard]] static ReturnPack Empty() { return ReturnPack(); }

  [[nodiscard]] std::size_t Size() const noexcept {
    return CarriesOwnedValue ? OwnedValuesValue.size() : ValuesValue.size();
  }

  [[nodiscard]] bool IsEmpty() const noexcept { return Size() == 0; }

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

  void Append(Value Element) {
    if (CarriesOwnedValue) {
      OwnedValuesValue.push_back(OwnedValue::FromValue(Element));
      return;
    }
    ValuesValue.push_back(std::move(Element));
  }

  ReturnPack &AppendBoolean(bool Element) {
    Append(Value(std::in_place_type<bool>, Element));
    return *this;
  }

  ReturnPack &AppendInteger(int Element) {
    Append(Value(std::in_place_type<int>, Element));
    return *this;
  }

  ReturnPack &AppendNumber(double Element) {
    Append(Value(std::in_place_type<double>, Element));
    return *this;
  }

  ReturnPack &AppendText(std::string Element) {
    Append(Value(std::in_place_type<std::string>, std::move(Element)));
    return *this;
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
      CarriesOwnedValue = true;
      OwnedValuesValue.reserve(ValuesValue.size() + 1);
      for (const Value &Existing : ValuesValue)
        OwnedValuesValue.push_back(OwnedValue::FromValue(Existing));
      ValuesValue.clear();
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

  void Clear() noexcept {
    ValuesValue.clear();
    OwnedValuesValue.clear();
    CarriesOwnedValue = false;
  }

  [[nodiscard]] friend bool operator==(const ReturnPack &Left,
                                       const ReturnPack &Right) {
    return Left.CarriesOwnedValue == Right.CarriesOwnedValue &&
           Left.ValuesValue == Right.ValuesValue &&
           Left.OwnedValuesValue == Right.OwnedValuesValue;
  }

  [[nodiscard]] friend bool operator!=(const ReturnPack &Left,
                                       const ReturnPack &Right) {
    return !(Left == Right);
  }

private:
  std::vector<Value> ValuesValue;
  std::vector<OwnedValue> OwnedValuesValue;
  bool CarriesOwnedValue = false;
};

} // namespace Luna
