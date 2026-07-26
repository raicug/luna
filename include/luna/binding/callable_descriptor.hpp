#pragma once

// clang-format off
#include <luna/binding/argument_pack.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/class_construction.hpp>
#include <luna/binding/instance_receiver.hpp>

#include <concepts>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
// clang-format on

namespace Luna {

// What one invocation produced. `Instance` is one staged native object of a
// registered class, produced by a constructor, a factory, or a singleton
// accessor; nothing about it is published yet.
enum class InvocationOutcomeKind {
  Value,
  Void,
  Values,
  Instance,
  InternalFailure
};

class InvocationOutcome {
public:
  [[nodiscard]] static InvocationOutcome WithValue(Value ReturnedValue) {
    return InvocationOutcome(InvocationOutcomeKind::Value,
                             std::move(ReturnedValue), {});
  }

  // One ordered pack of returned values, in return order. It is staging only:
  // nothing is published until every element has been validated.
  [[nodiscard]] static InvocationOutcome
  WithValues(std::vector<Value> ReturnedValues) {
    InvocationOutcome Outcome(InvocationOutcomeKind::Values, std::nullopt, {});
    Outcome.ValuesStorage = std::move(ReturnedValues);
    return Outcome;
  }

  [[nodiscard]] static InvocationOutcome Void() {
    return InvocationOutcome(InvocationOutcomeKind::Void, std::nullopt, {});
  }

  // One staged native object plus the ownership statement it will be owned
  // under. It is staging only: no value exists, no ownership record exists, and
  // nothing reached the virtual machine.
  [[nodiscard]] static InvocationOutcome
  WithInstance(Detail::ConstructedInstance Produced) {
    InvocationOutcome Outcome(InvocationOutcomeKind::Instance, std::nullopt,
                              {});
    Outcome.InstanceStorage = std::move(Produced);
    return Outcome;
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

  // The ordered returned values of one pack outcome, in return order.
  [[nodiscard]] std::span<const Value> ReturnedValues() const noexcept {
    return ValuesStorage;
  }

  // The staged native object of one instance outcome, or null otherwise.
  [[nodiscard]] const Detail::ConstructedInstance *
  ProducedInstance() const noexcept {
    return InstanceStorage ? &*InstanceStorage : nullptr;
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
  std::vector<Value> ValuesStorage;
  std::optional<Detail::ConstructedInstance> InstanceStorage;
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

    // The richer call shape: one slot per fixed parameter, omitted or supplied,
    // plus the variadic tail when the signature declares one. It is spelled
    // separately from `Invoke` so neither call is ever ambiguous.
    [[nodiscard]] virtual InvocationOutcome
    InvokeDeclared(const InvocationArguments &Arguments) = 0;

    // The two member shapes: the same two calls, plus the one validated
    // receiver the member operates on. They are spelled separately from the
    // receiverless calls, so an adapter that declares no receiver can never be
    // handed one and a member can never be invoked without one.
    [[nodiscard]] virtual InvocationOutcome
    InvokeWithReceiver(const InstanceReceiver &Receiver,
                       std::span<const Value> Arguments) = 0;
    [[nodiscard]] virtual InvocationOutcome
    InvokeDeclaredWithReceiver(const InstanceReceiver &Receiver,
                               const InvocationArguments &Arguments) = 0;
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

    // An adapter that describes only the foundation shape keeps working: its
    // fixed slots are unwrapped into the ordinary value span, and a shape it
    // cannot describe is refused as an internal inconsistency instead of being
    // guessed.
    [[nodiscard]] InvocationOutcome
    InvokeDeclared(const InvocationArguments &Arguments) override {
      if constexpr (requires(Adapter &Target) {
                      Target.InvokeDeclared(Arguments);
                    }) {
        return AdapterValue.InvokeDeclared(Arguments);
      } else {
        if (Arguments.HasVariadic())
          return InvocationOutcome::InternalFailure(
              "Callable argument metadata is inconsistent.");
        std::vector<Value> Supplied;
        Supplied.reserve(Arguments.Size());
        for (const ArgumentSlot &Slot : Arguments.Fixed()) {
          const Value *Present = Slot.Get();
          if (!Present)
            return InvocationOutcome::InternalFailure(
                "Callable argument metadata is inconsistent.");
          Supplied.push_back(*Present);
        }
        return AdapterValue.Invoke(Supplied);
      }
    }

    [[nodiscard]] InvocationOutcome
    InvokeWithReceiver(const InstanceReceiver &Receiver,
                       std::span<const Value> Arguments) override {
      if constexpr (requires(Adapter &Target) {
                      Target.InvokeWithReceiver(Receiver, Arguments);
                    }) {
        return AdapterValue.InvokeWithReceiver(Receiver, Arguments);
      } else {
        static_cast<void>(Receiver);
        static_cast<void>(Arguments);
        return InvocationOutcome::InternalFailure(
            "Callable declares no instance receiver.");
      }
    }

    [[nodiscard]] InvocationOutcome
    InvokeDeclaredWithReceiver(const InstanceReceiver &Receiver,
                               const InvocationArguments &Arguments) override {
      if constexpr (requires(Adapter &Target) {
                      Target.InvokeDeclaredWithReceiver(Receiver, Arguments);
                    }) {
        return AdapterValue.InvokeDeclaredWithReceiver(Receiver, Arguments);
      } else {
        static_cast<void>(Receiver);
        static_cast<void>(Arguments);
        return InvocationOutcome::InternalFailure(
            "Callable declares no instance receiver.");
      }
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

  [[nodiscard]] InvocationOutcome
  InvokeDeclared(const InvocationArguments &Arguments) {
    if (!Implementation)
      return InvocationOutcome::InternalFailure(
          "Callable descriptor has no implementation.");
    return Implementation->InvokeDeclared(Arguments);
  }

  // One instance member, invoked on the receiver its own validated access
  // produced. The receiver is never converted here: it arrives already
  // validated, which is what keeps every access check ahead of native code.
  [[nodiscard]] InvocationOutcome
  InvokeWithReceiver(const InstanceReceiver &Receiver,
                     std::span<const Value> Arguments) {
    if (!Implementation)
      return InvocationOutcome::InternalFailure(
          "Callable descriptor has no implementation.");
    return Implementation->InvokeWithReceiver(Receiver, Arguments);
  }

  [[nodiscard]] InvocationOutcome
  InvokeDeclaredWithReceiver(const InstanceReceiver &Receiver,
                             const InvocationArguments &Arguments) {
    if (!Implementation)
      return InvocationOutcome::InternalFailure(
          "Callable descriptor has no implementation.");
    return Implementation->InvokeDeclaredWithReceiver(Receiver, Arguments);
  }

private:
  CallableMetadata MetadataValue;
  std::unique_ptr<Interface> Implementation;
};

} // namespace Luna
