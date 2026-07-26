#pragma once

// Variadic arguments and one call's argument slots at the public boundary.
//
// A variadic callable receives its trailing arguments in one of exactly two
// Luna-owned forms, and neither one exposes a virtual-machine stack, pointer,
// index, or registry reference:
//
//   * `ArgumentView` is callback-lifetime only. It is an opaque Luna-owned
//     token naming the argument frame Luna opened for the current native
//     invocation; every accessor answers from that frame. Once the invocation
//     returns, every copy of the view answers as an inert empty view and the
//     attempt is recorded, so retaining one can never reach released
//     virtual-machine storage. `ToOwned()` is the documented way to keep the
//     arguments.
//   * `ArgumentPack` is owning. It holds every value it reports, outlives the
//     invocation, and is what a callable retains, stores, or publishes.
//
// Both forms report the one-based call argument position of each element, which
// is what lets a variadic rejection name the first failing call position.
//
// `ArgumentSlot` and `InvocationArguments` describe what one selected native
// target receives: one slot per fixed parameter, present or omitted, plus the
// variadic tail when the signature declares one. An omitted slot is how an
// unsupplied optional parameter reaches native code, and a materialized default
// arrives as an ordinary present slot.

// clang-format off
#include <luna/binding/conversion.hpp>
#include <luna/binding/value.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
// clang-format on

namespace Luna {

namespace Detail {
class ArgumentFrame;
} // namespace Detail

// One owning ordered pack of variadic arguments.
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

  // One-based call argument position of element `Index`, which is what a
  // variadic diagnostic reports.
  [[nodiscard]] std::size_t Position(std::size_t Index) const noexcept {
    return FirstPositionValue + Index;
  }

  // One-based call argument position the variadic tail starts at.
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

// A transient, non-owning view of the variadic arguments of one native
// invocation. The token is a Luna-owned opaque number: it is not a pointer, not
// a stack index, and not a registry reference, and no accessor can turn it into
// one.
class ArgumentView final {
public:
  ArgumentView() noexcept = default;

  // The view still names the live argument frame of a live invocation.
  [[nodiscard]] bool IsActive() const noexcept;

  [[nodiscard]] std::size_t Size() const noexcept;
  [[nodiscard]] bool IsEmpty() const noexcept;

  // One-based call argument position of element `Index`, and of the first
  // variadic element.
  [[nodiscard]] std::size_t Position(std::size_t Index) const noexcept;
  [[nodiscard]] std::size_t FirstPosition() const noexcept;

  [[nodiscard]] ValueCategory Kind(std::size_t Index) const noexcept;
  [[nodiscard]] bool IsNil(std::size_t Index) const noexcept;

  [[nodiscard]] std::optional<bool> ToBoolean(std::size_t Index) const noexcept;
  [[nodiscard]] std::optional<double>
  ToNumber(std::size_t Index) const noexcept;
  [[nodiscard]] std::optional<std::string> ToText(std::size_t Index) const;

  // Copy one element out of the frame.
  [[nodiscard]] OwnedValue At(std::size_t Index) const;

  // Complete path of element `Index`, such as `argument 3`.
  [[nodiscard]] std::string Path(std::size_t Index) const;

  // Copy every element out of the frame. This is the only supported retention.
  [[nodiscard]] ArgumentPack ToOwned() const;

private:
  friend class Detail::ArgumentFrame;

  explicit ArgumentView(std::uint64_t FrameToken) noexcept
      : FrameTokenValue(FrameToken) {}

  std::uint64_t FrameTokenValue = 0;
};

// One fixed parameter of one call: either a supplied or materialized value, or
// an omitted slot.
class ArgumentSlot final {
public:
  ArgumentSlot() = default;

  [[nodiscard]] static ArgumentSlot Omitted() { return ArgumentSlot(); }

  [[nodiscard]] static ArgumentSlot Supplied(Value Supplied) {
    ArgumentSlot Slot;
    Slot.ValueStorage = std::move(Supplied);
    return Slot;
  }

  [[nodiscard]] bool HasValue() const noexcept {
    return ValueStorage.has_value();
  }

  [[nodiscard]] const Value *Get() const noexcept {
    return ValueStorage ? &*ValueStorage : nullptr;
  }

private:
  std::optional<Value> ValueStorage;
};

// Everything one selected native target receives: one slot per fixed parameter
// in declared order, plus the variadic tail when the signature declares one.
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

  // The callback-lifetime view of the variadic tail.
  [[nodiscard]] ArgumentView Variadic() const noexcept { return ViewValue; }

  // The owning variadic tail, for a parameter that retains its arguments.
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
