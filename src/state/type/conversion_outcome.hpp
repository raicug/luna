#pragma once

// Outcome of one committing conversion from a Luau value into a Luna value.
// The foundation's inherited size policy lives here as well, so the migrated
// converters, the invocation validator, and the return writer all read one
// definition of the accepted byte count instead of repeating a literal.
//
// This header is deliberately free of every Luau declaration: it names the
// virtual machine only through an opaque forward declaration, which is what
// lets the canonical type records describe converters without leaking Luau.

// clang-format off
#include <luna/binding/value.hpp>

#include <cstddef>
#include <optional>
#include <string>
// clang-format on

namespace Luna::Detail {

// Inherited foundation policy: the largest string, in bytes, one conversion
// accepts in either direction.
inline constexpr std::size_t MaximumInvocationStringBytes = 1'048'576;

enum class ArgumentReadStatus {
  Success,
  TypeMismatch,
  IntegerNonFinite,
  IntegerOutOfRange,
  IntegerFractional,
  StringTooLong,
  InternalFailure
};

struct ArgumentReadResult final {
  ArgumentReadStatus Status = ArgumentReadStatus::InternalFailure;
  std::optional<Value> ConvertedValue;
  std::string ReceivedType;
  double ReceivedNumber = 0.0;
  std::size_t ReceivedByteCount = 0;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Status == ArgumentReadStatus::Success && ConvertedValue.has_value();
  }
};

} // namespace Luna::Detail
