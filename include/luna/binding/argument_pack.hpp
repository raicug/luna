#pragma once

// clang-format off
#include <luna/binding/conversion.hpp>
#include <luna/binding/delegate.hpp>
#include <luna/binding/instance_receiver.hpp>
#include <luna/binding/value.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
// clang-format on

namespace Luna {

namespace Detail {
class ArgumentFrame;
}

class ArgumentPack final {
public:
  ArgumentPack() = default;

  ArgumentPack(ValuePack Values, std::size_t FirstPosition)
      : ValuesValue(std::move(Values)), FirstPositionValue(FirstPosition) {}

  [[nodiscard]] std::size_t Size() const noexcept { return ValuesValue.Size(); }

  [[nodiscard]] bool IsEmpty() const noexcept {
    return ValuesValue.Size() == 0;
  }

  [[nodiscard]] OwnedValue At(std::size_t Index) const {
    return ValuesValue.At(Index);
  }

  [[nodiscard]] std::size_t Position(std::size_t Index) const noexcept {
    return FirstPositionValue + Index;
  }

  [[nodiscard]] std::size_t FirstPosition() const noexcept {
    return FirstPositionValue;
  }

  [[nodiscard]] const ValuePack &Values() const noexcept { return ValuesValue; }

  void Append(OwnedValue Element) { ValuesValue.Append(std::move(Element)); }

  [[nodiscard]] friend bool operator==(const ArgumentPack &Left,
                                       const ArgumentPack &Right) {
    return Left.FirstPositionValue == Right.FirstPositionValue &&
           Left.ValuesValue == Right.ValuesValue;
  }

  [[nodiscard]] friend bool operator!=(const ArgumentPack &Left,
                                       const ArgumentPack &Right) {
    return !(Left == Right);
  }

private:
  ValuePack ValuesValue;
  std::size_t FirstPositionValue = 1;
};

class ArgumentView final {
public:
  ArgumentView() noexcept = default;

  [[nodiscard]] bool IsActive() const noexcept;

  [[nodiscard]] std::size_t Size() const noexcept;
  [[nodiscard]] bool IsEmpty() const noexcept;

  [[nodiscard]] std::size_t Position(std::size_t Index) const noexcept;
  [[nodiscard]] std::size_t FirstPosition() const noexcept;

  [[nodiscard]] ValueCategory Kind(std::size_t Index) const noexcept;
  [[nodiscard]] bool IsNil(std::size_t Index) const noexcept;

  [[nodiscard]] std::optional<bool> ToBoolean(std::size_t Index) const noexcept;
  [[nodiscard]] std::optional<double>
  ToNumber(std::size_t Index) const noexcept;
  [[nodiscard]] std::optional<std::string> ToText(std::size_t Index) const;

  [[nodiscard]] OwnedValue At(std::size_t Index) const;

  [[nodiscard]] std::string Path(std::size_t Index) const;

  [[nodiscard]] ArgumentPack ToOwned() const;

private:
  friend class Detail::ArgumentFrame;

  explicit ArgumentView(std::uint64_t FrameToken) noexcept
      : FrameTokenValue(FrameToken) {}

  std::uint64_t FrameTokenValue = 0;
};

class ArgumentSlot final {
public:
  ArgumentSlot() = default;

  [[nodiscard]] static ArgumentSlot Omitted() { return ArgumentSlot(); }

  [[nodiscard]] static ArgumentSlot Supplied(Value Supplied) {
    ArgumentSlot Slot;
    Slot.ValueStorage = std::move(Supplied);
    return Slot;
  }

  // One subscribed handler the caller supplied for a delegate parameter.
  [[nodiscard]] static ArgumentSlot
  SuppliedHandler(std::shared_ptr<Detail::DelegateTarget> Handler) {
    ArgumentSlot Slot;
    Slot.HandlerStorage = std::move(Handler);
    return Slot;
  }

  // The raw value the caller supplied for a parameter whose declared type
  // converts through its own `Luna::TypeConverter<T>` specialization. The
  // native value is only produced later, once the templated call site knows
  // the concrete C++ type to read it as.
  [[nodiscard]] static ArgumentSlot SuppliedConverted(OwnedValue Source) {
    ArgumentSlot Slot;
    Slot.ConvertedStorage = std::move(Source);
    return Slot;
  }

  // One registered class instance the caller supplied for an instance
  // operand, already through the same access gate a receiver passes, so the
  // templated call site receives storage it may use without re-checking.
  [[nodiscard]] static ArgumentSlot SuppliedInstance(InstanceReceiver Bound) {
    ArgumentSlot Slot;
    Slot.InstanceStorage = Bound;
    return Slot;
  }

  [[nodiscard]] bool HasValue() const noexcept {
    return ValueStorage.has_value();
  }

  [[nodiscard]] bool HasHandler() const noexcept {
    return HandlerStorage != nullptr;
  }

  [[nodiscard]] bool HasConvertedValue() const noexcept {
    return ConvertedStorage.has_value();
  }

  [[nodiscard]] bool HasInstance() const noexcept {
    return InstanceStorage.IsBound();
  }

  [[nodiscard]] const InstanceReceiver &Instance() const noexcept {
    return InstanceStorage;
  }

  [[nodiscard]] const Value *Get() const noexcept {
    return ValueStorage ? &*ValueStorage : nullptr;
  }

  [[nodiscard]] const std::shared_ptr<Detail::DelegateTarget> &
  Handler() const noexcept {
    return HandlerStorage;
  }

  [[nodiscard]] const OwnedValue *ConvertedValue() const noexcept {
    return ConvertedStorage ? &*ConvertedStorage : nullptr;
  }

private:
  std::optional<Value> ValueStorage;
  std::shared_ptr<Detail::DelegateTarget> HandlerStorage;
  std::optional<OwnedValue> ConvertedStorage;
  InstanceReceiver InstanceStorage;
};

class InvocationArguments final {
public:
  InvocationArguments() = default;

  explicit InvocationArguments(std::span<const ArgumentSlot> Fixed) noexcept
      : FixedValue(Fixed) {}

  InvocationArguments(std::span<const ArgumentSlot> Fixed, ArgumentView View,
                      const ArgumentPack *Retained) noexcept
      : FixedValue(Fixed), ViewValue(View), RetainedValue(Retained) {}

  [[nodiscard]] std::size_t Size() const noexcept { return FixedValue.size(); }

  [[nodiscard]] std::span<const ArgumentSlot> Fixed() const noexcept {
    return FixedValue;
  }

  [[nodiscard]] const ArgumentSlot *At(std::size_t Index) const noexcept {
    if (Index >= FixedValue.size())
      return nullptr;
    return &FixedValue[Index];
  }

  [[nodiscard]] bool HasVariadic() const noexcept {
    return RetainedValue != nullptr;
  }

  [[nodiscard]] ArgumentView Variadic() const noexcept { return ViewValue; }

  [[nodiscard]] const ArgumentPack &Retained() const noexcept {
    static const ArgumentPack Empty;
    return RetainedValue ? *RetainedValue : Empty;
  }

private:
  std::span<const ArgumentSlot> FixedValue;
  ArgumentView ViewValue;
  const ArgumentPack *RetainedValue = nullptr;
};

} // namespace Luna
