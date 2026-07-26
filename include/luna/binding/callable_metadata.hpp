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

// How one callable produces its returns. `Void` publishes zero values, `Value`
// exactly one, `Pack` the ordered elements of a `std::pair`, `std::tuple`, or
// `Luna::ReturnPack`, and `Instance` exactly one value of one registered class.
enum class ReturnDisposition { Value, Void, Suppress, Pack, Instance };

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

  // One ordered pack whose element types the signature fixes: a returned
  // `std::pair` or `std::tuple`. The declared element count is the published
  // return count.
  [[nodiscard]] static ReturnMetadata ForPack(std::vector<ValueKind> Kinds) {
    ReturnMetadata Metadata(ReturnDisposition::Pack, std::nullopt);
    Metadata.PackKindsValue = std::move(Kinds);
    Metadata.DeclaredPackValue = true;
    return Metadata;
  }

  // One ordered pack whose element count and element types the invocation
  // decides: a returned `Luna::ReturnPack`.
  [[nodiscard]] static ReturnMetadata ForDynamicPack() {
    return ReturnMetadata(ReturnDisposition::Pack, std::nullopt);
  }

  // Exactly one value of the registered class `Class` names: the result of a
  // constructor, a factory, or a singleton accessor. The class is named by its
  // validated stable key, so publication resolves it through the canonical type
  // registry of the generation the invocation captured.
  [[nodiscard]] static ReturnMetadata ForInstance(StableTypeKey Class) {
    ReturnMetadata Metadata(ReturnDisposition::Instance, std::nullopt);
    Metadata.InstanceKeyValue = std::move(Class);
    return Metadata;
  }

  [[nodiscard]] ReturnDisposition Disposition() const noexcept {
    return DispositionValue;
  }

  // The registered class one instance return publishes, or none for every other
  // return shape.
  [[nodiscard]] const StableTypeKey *InstanceKey() const noexcept {
    return InstanceKeyValue ? &*InstanceKeyValue : nullptr;
  }

  [[nodiscard]] const ValueKind *Kind() const noexcept {
    return KindValue ? &*KindValue : nullptr;
  }

  // The declared element types of one ordered pack, in return order. A dynamic
  // pack declares none.
  [[nodiscard]] std::span<const ValueKind> PackKinds() const noexcept {
    return PackKindsValue;
  }

  // The signature fixes the element count and element types of the pack, so
  // every element can be validated against its declared type.
  [[nodiscard]] bool HasDeclaredPackShape() const noexcept {
    return DeclaredPackValue;
  }

private:
  ReturnMetadata(ReturnDisposition DispositionValue,
                 std::optional<ValueKind> KindValue) noexcept
      : DispositionValue(DispositionValue), KindValue(std::move(KindValue)) {}

  ReturnDisposition DispositionValue;
  std::optional<ValueKind> KindValue;
  std::vector<ValueKind> PackKindsValue;
  bool DeclaredPackValue = false;
  std::optional<StableTypeKey> InstanceKeyValue;
};

class CallableMetadata {
public:
  // The foundation shape: every parameter is required and names one value kind.
  CallableMetadata(std::vector<ValueKind> ParameterTypes,
                   ReturnMetadata ReturnType)
      : ParameterTypesValue(std::move(ParameterTypes)),
        ReturnTypeValue(std::move(ReturnType)) {
    ParametersValue.reserve(ParameterTypesValue.size());
    for (const ValueKind Kind : ParameterTypesValue)
      ParametersValue.push_back(ParameterDescriptor::ForRequired(Kind));
  }

  // The richer shape: optional, defaulted, and variadic parameters described by
  // their own immutable descriptors. `ParameterTypes` stays empty here, because
  // one value kind cannot describe an omittable or variadic parameter; every
  // caller that needs the shape reads `Parameters`.
  [[nodiscard]] static CallableMetadata
  ForDeclaredParameters(std::vector<ParameterDescriptor> Parameters,
                        ReturnMetadata ReturnType) {
    CallableMetadata Metadata(std::vector<ValueKind>(), std::move(ReturnType));
    Metadata.ParametersValue = std::move(Parameters);
    Metadata.RichParametersValue = true;
    return Metadata;
  }

  // The same metadata, for one instance member: its ordinary parameters are
  // described exactly as any other callable's, and the object it operates on is
  // described separately because it is rank position zero of the call rather
  // than one of those parameters.
  [[nodiscard]] static CallableMetadata
  ForInstanceMember(ReceiverMetadata Receiver, CallableMetadata Declared) {
    Declared.ReceiverValue = std::move(Receiver);
    return Declared;
  }

  [[nodiscard]] std::span<const ValueKind> ParameterTypes() const noexcept {
    return ParameterTypesValue;
  }

  // Every declared parameter, in declared order, for both shapes.
  [[nodiscard]] std::span<const ParameterDescriptor>
  Parameters() const noexcept {
    return ParametersValue;
  }

  // The callable declares at least one optional, defaulted, or variadic
  // parameter, so its call shape is not the foundation's fixed arity.
  [[nodiscard]] bool HasRichParameters() const noexcept {
    return RichParametersValue;
  }

  [[nodiscard]] const ReturnMetadata &ReturnType() const noexcept {
    return ReturnTypeValue;
  }

  // The object one instance member operates on, or none for every callable that
  // takes no receiver at all - a free function, a static method, a constructor,
  // and a factory alike.
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
