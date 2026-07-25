#pragma once

// clang-format off
#include <luna/binding/value.hpp>

#include <optional>
#include <span>
#include <utility>
#include <vector>
// clang-format on

namespace Luna {

enum class ReturnDisposition { Value, Void, Suppress };

class ReturnMetadata {
public:
  [[nodiscard]] static ReturnMetadata ForValue(ValueKind Kind) noexcept {
    return ReturnMetadata(ReturnDisposition::Value, Kind);
  }

  [[nodiscard]] static ReturnMetadata ForVoid() noexcept {
    return ReturnMetadata(ReturnDisposition::Void, std::nullopt);
  }

  [[nodiscard]] static ReturnMetadata Suppressed() noexcept {
    return ReturnMetadata(ReturnDisposition::Suppress, std::nullopt);
  }

  [[nodiscard]] ReturnDisposition Disposition() const noexcept {
    return DispositionValue;
  }

  [[nodiscard]] const ValueKind *Kind() const noexcept {
    return KindValue ? &*KindValue : nullptr;
  }

private:
  ReturnMetadata(ReturnDisposition DispositionValue,
                 std::optional<ValueKind> KindValue) noexcept
      : DispositionValue(DispositionValue), KindValue(std::move(KindValue)) {}

  ReturnDisposition DispositionValue;
  std::optional<ValueKind> KindValue;
};

class CallableMetadata {
public:
  CallableMetadata(std::vector<ValueKind> ParameterTypes,
                   ReturnMetadata ReturnType)
      : ParameterTypesValue(std::move(ParameterTypes)),
        ReturnTypeValue(std::move(ReturnType)) {}

  [[nodiscard]] std::span<const ValueKind> ParameterTypes() const noexcept {
    return ParameterTypesValue;
  }

  [[nodiscard]] const ReturnMetadata &ReturnType() const noexcept {
    return ReturnTypeValue;
  }

private:
  std::vector<ValueKind> ParameterTypesValue;
  ReturnMetadata ReturnTypeValue;
};

} // namespace Luna
