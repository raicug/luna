#pragma once

// clang-format off
#include <luna/binding/instance_receiver.hpp>
#include <luna/binding/parameter_descriptor.hpp>
#include <luna/binding/value.hpp>
#include <luna/type/stable_type_key.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>
// clang-format on

namespace Luna {

enum class ReturnDisposition {
  Value,
  Void,
  Suppress,
  Pack,
  Instance,
  Owned,
  OwnedPack
};

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

  [[nodiscard]] static ReturnMetadata ForPack(std::vector<ValueKind> Kinds) {
    ReturnMetadata Metadata(ReturnDisposition::Pack, std::nullopt);
    Metadata.PackKindsValue = std::move(Kinds);
    Metadata.DeclaredPackValue = true;
    return Metadata;
  }

  [[nodiscard]] static ReturnMetadata ForDynamicPack() {
    return ReturnMetadata(ReturnDisposition::Pack, std::nullopt);
  }

  [[nodiscard]] static ReturnMetadata ForOwnedValue() noexcept {
    return ReturnMetadata(ReturnDisposition::Owned, std::nullopt);
  }

  [[nodiscard]] static ReturnMetadata ForOwnedPack() noexcept {
    return ReturnMetadata(ReturnDisposition::OwnedPack, std::nullopt);
  }

  [[nodiscard]] static ReturnMetadata ForInstance(StableTypeKey Class) {
    ReturnMetadata Metadata(ReturnDisposition::Instance, std::nullopt);
    Metadata.InstanceKeyValue = std::move(Class);
    return Metadata;
  }

  [[nodiscard]] static ReturnMetadata ForAsync(ReturnMetadata Awaited) {
    Awaited.AsynchronousValue = true;
    return Awaited;
  }

  [[nodiscard]] ReturnDisposition Disposition() const noexcept {
    return DispositionValue;
  }

  [[nodiscard]] const StableTypeKey *InstanceKey() const noexcept {
    return InstanceKeyValue ? &*InstanceKeyValue : nullptr;
  }

  [[nodiscard]] const ValueKind *Kind() const noexcept {
    return KindValue ? &*KindValue : nullptr;
  }

  [[nodiscard]] std::span<const ValueKind> PackKinds() const noexcept {
    return PackKindsValue;
  }

  [[nodiscard]] bool HasDeclaredPackShape() const noexcept {
    return DeclaredPackValue;
  }

  [[nodiscard]] bool IsAsynchronous() const noexcept {
    return AsynchronousValue;
  }

private:
  ReturnMetadata(ReturnDisposition DispositionValue,
                 std::optional<ValueKind> KindValue) noexcept
      : DispositionValue(DispositionValue), KindValue(std::move(KindValue)) {}

  ReturnDisposition DispositionValue;
  std::optional<ValueKind> KindValue;
  std::vector<ValueKind> PackKindsValue;
  bool DeclaredPackValue = false;
  bool AsynchronousValue = false;
  std::optional<StableTypeKey> InstanceKeyValue;
};

class CallableMetadata {
public:
  CallableMetadata(std::vector<ValueKind> ParameterTypes,
                   ReturnMetadata ReturnType)
      : ParameterTypesValue(std::move(ParameterTypes)),
        ReturnTypeValue(std::move(ReturnType)) {
    ParametersValue.reserve(ParameterTypesValue.size());
    for (const ValueKind Kind : ParameterTypesValue)
      ParametersValue.push_back(ParameterDescriptor::ForRequired(Kind));
  }

  [[nodiscard]] static CallableMetadata
  ForDeclaredParameters(std::vector<ParameterDescriptor> Parameters,
                        ReturnMetadata ReturnType) {
    CallableMetadata Metadata(std::vector<ValueKind>(), std::move(ReturnType));
    Metadata.ParametersValue = std::move(Parameters);
    Metadata.RichParametersValue = true;
    return Metadata;
  }

  [[nodiscard]] static CallableMetadata
  ForInstanceMember(ReceiverMetadata Receiver, CallableMetadata Declared) {
    Declared.ReceiverValue = std::move(Receiver);
    return Declared;
  }

  [[nodiscard]] std::span<const ValueKind> ParameterTypes() const noexcept {
    return ParameterTypesValue;
  }

  [[nodiscard]] std::span<const ParameterDescriptor>
  Parameters() const noexcept {
    return ParametersValue;
  }

  [[nodiscard]] bool HasRichParameters() const noexcept {
    return RichParametersValue;
  }

  [[nodiscard]] const ReturnMetadata &ReturnType() const noexcept {
    return ReturnTypeValue;
  }

  [[nodiscard]] const ReceiverMetadata *Receiver() const noexcept {
    return ReceiverValue ? &*ReceiverValue : nullptr;
  }

  [[nodiscard]] bool HasReceiver() const noexcept {
    return ReceiverValue.has_value();
  }

private:
  std::vector<ValueKind> ParameterTypesValue;
  std::vector<ParameterDescriptor> ParametersValue;
  ReturnMetadata ReturnTypeValue;
  bool RichParametersValue = false;
  std::optional<ReceiverMetadata> ReceiverValue;
};

} // namespace Luna
