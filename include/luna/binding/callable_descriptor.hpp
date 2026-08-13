#pragma once

// clang-format off
#include <luna/binding/argument_pack.hpp>
#include <luna/binding/async_task.hpp>
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

class ReturnPack;

enum class InvocationOutcomeKind {
  Value,
  Void,
  Values,
  OwnedValues,
  Instance,
  Chunk,
  Suspended,
  InternalFailure
};

namespace Detail {

struct PrimitiveCallValue final {
  bool Boolean = false;
  int Integer = 0;
  double Number = 0.0;
};

struct PrimitiveInvocationPlan final {
  using InvokeFunction = bool (*)(void *, std::span<const PrimitiveCallValue>,
                                  PrimitiveCallValue &);
  using InvokePackFunction = bool (*)(void *,
                                      std::span<const PrimitiveCallValue>,
                                      ReturnPack &);

  void *Context = nullptr;
  InvokeFunction Invoke = nullptr;
  InvokePackFunction InvokePack = nullptr;

  [[nodiscard]] bool IsAvailable() const noexcept {
    return Context != nullptr && (Invoke != nullptr || InvokePack != nullptr);
  }

  [[nodiscard]] bool HasScalarInvoke() const noexcept {
    return Context != nullptr && Invoke != nullptr;
  }

  [[nodiscard]] bool HasPackInvoke() const noexcept {
    return Context != nullptr && InvokePack != nullptr;
  }
};

} // namespace Detail

class InvocationOutcome {
public:
  [[nodiscard]] static InvocationOutcome WithValue(Value ReturnedValue) {
    return InvocationOutcome(InvocationOutcomeKind::Value,
                             std::move(ReturnedValue), {});
  }

  [[nodiscard]] static InvocationOutcome
  WithValues(std::vector<Value> ReturnedValues) {
    InvocationOutcome Outcome(InvocationOutcomeKind::Values, std::nullopt, {});
    Outcome.ValuesStorage = std::move(ReturnedValues);
    return Outcome;
  }

  [[nodiscard]] static InvocationOutcome Void() {
    return InvocationOutcome(InvocationOutcomeKind::Void, std::nullopt, {});
  }

  [[nodiscard]] static InvocationOutcome WithOwnedValues(ValuePack Produced) {
    InvocationOutcome Outcome(InvocationOutcomeKind::OwnedValues, std::nullopt,
                              {});
    Outcome.OwnedStorage = std::move(Produced);
    return Outcome;
  }

  [[nodiscard]] static InvocationOutcome
  WithInstance(Detail::ConstructedInstance Produced) {
    InvocationOutcome Outcome(InvocationOutcomeKind::Instance, std::nullopt,
                              {});
    Outcome.InstanceStorage = std::move(Produced);
    return Outcome;
  }

  [[nodiscard]] static InvocationOutcome WithChunk(std::string Bytecode,
                                                   std::string Name) {
    if (Bytecode.empty())
      return InternalFailure("Callable produced no loadable chunk.");
    InvocationOutcome Outcome(InvocationOutcomeKind::Chunk, std::nullopt, {});
    Outcome.ChunkBytecodeValue = std::move(Bytecode);
    Outcome.ChunkNameValue = std::move(Name);
    return Outcome;
  }

  [[nodiscard]] static InvocationOutcome
  Suspended(std::unique_ptr<Detail::PendingAsyncWork> Started) {
    if (!Started)
      return InternalFailure("Callable produced no asynchronous work.");
    InvocationOutcome Outcome(InvocationOutcomeKind::Suspended, std::nullopt,
                              {});
    Outcome.SuspendedStorage = std::move(Started);
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

  [[nodiscard]] std::span<const Value> ReturnedValues() const noexcept {
    return ValuesStorage;
  }

  [[nodiscard]] const ValuePack &ReturnedOwnedValues() const noexcept {
    return OwnedStorage;
  }

  [[nodiscard]] const Detail::ConstructedInstance *
  ProducedInstance() const noexcept {
    return InstanceStorage ? &*InstanceStorage : nullptr;
  }

  [[nodiscard]] const std::string &ChunkBytecode() const noexcept {
    return ChunkBytecodeValue;
  }

  [[nodiscard]] const std::string &ChunkName() const noexcept {
    return ChunkNameValue;
  }

  [[nodiscard]] const std::string &FailureMessage() const noexcept {
    return FailureMessageValue;
  }

  [[nodiscard]] bool HasSuspendedWork() const noexcept {
    return SuspendedStorage != nullptr;
  }

  [[nodiscard]] std::unique_ptr<Detail::PendingAsyncWork> TakeSuspendedWork() {
    return std::move(SuspendedStorage);
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
  ValuePack OwnedStorage;
  std::optional<Detail::ConstructedInstance> InstanceStorage;
  std::unique_ptr<Detail::PendingAsyncWork> SuspendedStorage;
  std::string ChunkBytecodeValue;
  std::string ChunkNameValue;
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

    [[nodiscard]] virtual InvocationOutcome
    InvokeDeclared(const InvocationArguments &Arguments) = 0;

    [[nodiscard]] virtual InvocationOutcome
    InvokeWithReceiver(const InstanceReceiver &Receiver,
                       std::span<const Value> Arguments) = 0;
    [[nodiscard]] virtual InvocationOutcome
    InvokeDeclaredWithReceiver(const InstanceReceiver &Receiver,
                               const InvocationArguments &Arguments) = 0;
    [[nodiscard]] virtual const Detail::PrimitiveInvocationPlan *
    PrimitiveInvocation() const noexcept = 0;
  };

  template <class Adapter> class Model final : public Interface {
  public:
    template <class Source>
    explicit Model(Source &&AdapterValue)
        : AdapterValue(std::forward<Source>(AdapterValue)) {
      if constexpr (requires(Adapter &Target) { Target.PrimitiveInvocation(); })
        PrimitivePlan = this->AdapterValue.PrimitiveInvocation();
    }

    [[nodiscard]] bool HasTarget() const noexcept override {
      return AdapterValue.HasTarget();
    }

    [[nodiscard]] InvocationOutcome
    Invoke(std::span<const Value> Arguments) override {
      return AdapterValue.Invoke(Arguments);
    }

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

    [[nodiscard]] const Detail::PrimitiveInvocationPlan *
    PrimitiveInvocation() const noexcept override {
      return PrimitivePlan.IsAvailable() ? &PrimitivePlan : nullptr;
    }

  private:
    Adapter AdapterValue;
    Detail::PrimitiveInvocationPlan PrimitivePlan;
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
            std::forward<Adapter>(AdapterValue))),
        PrimitivePlan(Implementation->PrimitiveInvocation()) {}

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

  [[nodiscard]] const Detail::PrimitiveInvocationPlan *
  PrimitiveInvocation() const noexcept {
    return PrimitivePlan;
  }

  [[nodiscard]] InvocationOutcome
  InvokeDeclared(const InvocationArguments &Arguments) {
    if (!Implementation)
      return InvocationOutcome::InternalFailure(
          "Callable descriptor has no implementation.");
    return Implementation->InvokeDeclared(Arguments);
  }

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
  const Detail::PrimitiveInvocationPlan *PrimitivePlan = nullptr;
};

} // namespace Luna
