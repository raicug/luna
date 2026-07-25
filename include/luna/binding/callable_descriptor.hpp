#pragma once

// clang-format off
#include <luna/binding/callable_metadata.hpp>

#include <concepts>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
// clang-format on

namespace Luna {

enum class InvocationOutcomeKind { Value, Void, InternalFailure };

class InvocationOutcome {
public:
  [[nodiscard]] static InvocationOutcome WithValue(Value ReturnedValue) {
    return InvocationOutcome(InvocationOutcomeKind::Value,
                             std::move(ReturnedValue), {});
  }

  [[nodiscard]] static InvocationOutcome Void() {
    return InvocationOutcome(InvocationOutcomeKind::Void, std::nullopt, {});
  }

  [[nodiscard]] static InvocationOutcome InternalFailure(std::string Message) {
    if (Message.empty())
      Message = "Internal callable invocation failure.";
    return InvocationOutcome(InvocationOutcomeKind::InternalFailure,
                             std::nullopt, std::move(Message));
  }

  [[nodiscard]] InvocationOutcomeKind Kind() const noexcept {
    return KindValue;
  }

  [[nodiscard]] const Value *ReturnedValue() const noexcept {
    return ValueValue ? &*ValueValue : nullptr;
  }

  [[nodiscard]] const std::string &FailureMessage() const noexcept {
    return FailureMessageValue;
  }

private:
  InvocationOutcome(InvocationOutcomeKind KindValue,
                    std::optional<Value> ValueValue,
                    std::string FailureMessageValue)
      : KindValue(KindValue), ValueValue(std::move(ValueValue)),
        FailureMessageValue(std::move(FailureMessageValue)) {}

  InvocationOutcomeKind KindValue;
  std::optional<Value> ValueValue;
  std::string FailureMessageValue;
};

class ErasedCallableDescriptor {
private:
  class Interface {
  public:
    virtual ~Interface() = default;
    [[nodiscard]] virtual bool HasTarget() const noexcept = 0;
    [[nodiscard]] virtual InvocationOutcome
    Invoke(std::span<const Value> Arguments) = 0;
  };

  template <class Adapter> class Model final : public Interface {
  public:
    template <class Source>
    explicit Model(Source &&AdapterValue)
        : AdapterValue(std::forward<Source>(AdapterValue)) {}

    [[nodiscard]] bool HasTarget() const noexcept override {
      return AdapterValue.HasTarget();
    }

    [[nodiscard]] InvocationOutcome
    Invoke(std::span<const Value> Arguments) override {
      return AdapterValue.Invoke(Arguments);
    }

  private:
    Adapter AdapterValue;
  };

public:
  template <class Adapter>
    requires(!std::same_as<std::remove_cvref_t<Adapter>,
                           ErasedCallableDescriptor> &&
             std::constructible_from<std::remove_cvref_t<Adapter>,
                                     Adapter &&> &&
             requires(std::remove_cvref_t<Adapter> &AdapterValue,
                      std::span<const Value> Arguments) {
               { AdapterValue.HasTarget() } -> std::convertible_to<bool>;
               {
                 AdapterValue.Invoke(Arguments)
               } -> std::same_as<InvocationOutcome>;
             })
  ErasedCallableDescriptor(CallableMetadata MetadataValue,
                           Adapter &&AdapterValue)
      : MetadataValue(std::move(MetadataValue)),
        Implementation(std::make_unique<Model<std::remove_cvref_t<Adapter>>>(
            std::forward<Adapter>(AdapterValue))) {}

  ErasedCallableDescriptor(const ErasedCallableDescriptor &) = delete;
  ErasedCallableDescriptor &
  operator=(const ErasedCallableDescriptor &) = delete;
  ErasedCallableDescriptor(ErasedCallableDescriptor &&) noexcept = default;
  ErasedCallableDescriptor &
  operator=(ErasedCallableDescriptor &&) noexcept = default;

  [[nodiscard]] const CallableMetadata &Metadata() const noexcept {
    return MetadataValue;
  }

  [[nodiscard]] bool HasTarget() const noexcept {
    return Implementation && Implementation->HasTarget();
  }

  [[nodiscard]] InvocationOutcome Invoke(std::span<const Value> Arguments) {
    if (!Implementation)
      return InvocationOutcome::InternalFailure(
          "Callable descriptor has no implementation.");
    return Implementation->Invoke(Arguments);
  }

private:
  CallableMetadata MetadataValue;
  std::unique_ptr<Interface> Implementation;
};

} // namespace Luna
