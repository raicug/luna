#pragma once

// clang-format off
#include <luna/binding/value.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace Luna {

// Why one call of a subscribed handler ended the way it did. Every failure
// stage is reported, never thrown, by the non-throwing invocation path.
enum class DelegateStatus {
  Ready,
  Released,
  ForeignThread,
  HandlerFailed,
  ResultMismatch
};

[[nodiscard]] constexpr std::string_view
DelegateStatusText(DelegateStatus Status) noexcept {
  switch (Status) {
  case DelegateStatus::Ready:
    return "ready";
  case DelegateStatus::Released:
    return "released";
  case DelegateStatus::ForeignThread:
    return "foreign_thread";
  case DelegateStatus::HandlerFailed:
    return "handler_failed";
  case DelegateStatus::ResultMismatch:
    return "result_mismatch";
  }
  return "released";
}

// The declared call shape of one delegate parameter. It names only canonical
// Luna value kinds, so a delegate descriptor stays reflectable.
struct DelegateShape final {
  std::vector<ValueKind> Parameters;
  std::optional<ValueKind> Result;

  [[nodiscard]] friend bool operator==(const DelegateShape &Left,
                                       const DelegateShape &Right) = default;
};

// One completed handler call. A produced value is present only when the
// delegate declares a result and the handler published a matching value.
class DelegateCallResult final {
public:
  DelegateCallResult() = default;

  [[nodiscard]] static DelegateCallResult
  Delivered(std::optional<Value> Produced) {
    DelegateCallResult Result;
    Result.StatusValue = DelegateStatus::Ready;
    Result.ProducedValue = std::move(Produced);
    return Result;
  }

  [[nodiscard]] static DelegateCallResult Refused(DelegateStatus Status,
                                                  std::string Diagnostic) {
    DelegateCallResult Result;
    Result.StatusValue =
        Status == DelegateStatus::Ready ? DelegateStatus::Released : Status;
    Result.DiagnosticValue = std::move(Diagnostic);
    return Result;
  }

  [[nodiscard]] DelegateStatus Status() const noexcept { return StatusValue; }

  [[nodiscard]] bool IsSuccess() const noexcept {
    return StatusValue == DelegateStatus::Ready;
  }

  [[nodiscard]] const Value *Produced() const noexcept {
    return ProducedValue ? &*ProducedValue : nullptr;
  }

  [[nodiscard]] const std::string &Diagnostic() const noexcept {
    return DiagnosticValue;
  }

private:
  DelegateStatus StatusValue = DelegateStatus::Released;
  std::optional<Value> ProducedValue;
  std::string DiagnosticValue;
};

// What the throwing call operator reports when a handler cannot deliver.
class DelegateFailure final : public std::runtime_error {
public:
  DelegateFailure(DelegateStatus Status, const std::string &Diagnostic)
      : std::runtime_error(Diagnostic.empty()
                               ? std::string("the subscribed handler is ") +
                                     std::string(DelegateStatusText(Status))
                               : Diagnostic),
        StatusValue(Status) {}

  [[nodiscard]] DelegateStatus Status() const noexcept { return StatusValue; }

private:
  DelegateStatus StatusValue = DelegateStatus::Released;
};

namespace Detail {

template <class Type>
inline constexpr bool IsDelegateValueType =
    std::is_same_v<Type, bool> || std::is_same_v<Type, int> ||
    std::is_same_v<Type, double> || std::is_same_v<Type, std::string>;

template <class Type> [[nodiscard]] constexpr ValueKind DelegateValueKind() {
  if constexpr (std::is_same_v<Type, bool>)
    return ValueKind::Boolean;
  else if constexpr (std::is_same_v<Type, int>)
    return ValueKind::Integer;
  else if constexpr (std::is_same_v<Type, double>)
    return ValueKind::Number;
  else
    return ValueKind::String;
}

// One subscribed handler that the virtual machine owns. Luna holds it through
// its own reference mechanism, so no public declaration ever names a virtual
// machine state or a stack index.
class DelegateTarget {
public:
  DelegateTarget() = default;
  virtual ~DelegateTarget() = default;

  DelegateTarget(const DelegateTarget &) = delete;
  DelegateTarget &operator=(const DelegateTarget &) = delete;

  [[nodiscard]] virtual bool IsLive() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t Identity() const noexcept = 0;
  [[nodiscard]] virtual DelegateCallResult
  Call(std::span<const Value> Arguments) = 0;
  virtual void Release() noexcept = 0;
};

template <class Signature> struct DelegateSignatureShape;

template <class Return, class... Parameters>
struct DelegateSignatureShape<Return(Parameters...)> {
  static constexpr bool IsSupported =
      (std::is_void_v<Return> || IsDelegateValueType<Return>) &&
      (IsDelegateValueType<Parameters> && ...);

  [[nodiscard]] static DelegateShape Shape() {
    DelegateShape Declared;
    Declared.Parameters =
        std::vector<ValueKind>{DelegateValueKind<Parameters>()...};
    if constexpr (!std::is_void_v<Return>)
      Declared.Result = DelegateValueKind<Return>();
    return Declared;
  }
};

} // namespace Detail

// A handler a script subscribed, held by owning native code. Copies share one
// virtual-machine reference, so releasing one copy releases the handler for
// every copy and every later call reports the release deterministically.
template <class Signature> class Delegate;

template <class Return, class... Parameters>
class Delegate<Return(Parameters...)> final {
public:
  using ResultType = Return;
  using SignatureType = Return(Parameters...);

  static_assert(Detail::DelegateSignatureShape<SignatureType>::IsSupported,
                "a Luna delegate publishes canonical Luna values only");

  Delegate() = default;

  explicit Delegate(std::shared_ptr<Detail::DelegateTarget> Bound) noexcept
      : BoundValue(std::move(Bound)) {}

  [[nodiscard]] static DelegateShape Shape() {
    return Detail::DelegateSignatureShape<SignatureType>::Shape();
  }

  [[nodiscard]] bool IsValid() const noexcept {
    return BoundValue != nullptr && BoundValue->IsLive();
  }

  [[nodiscard]] std::uint64_t Identity() const noexcept {
    return BoundValue ? BoundValue->Identity() : 0;
  }

  void Release() noexcept {
    if (BoundValue)
      BoundValue->Release();
  }

  [[nodiscard]] const std::shared_ptr<Detail::DelegateTarget> &
  Target() const noexcept {
    return BoundValue;
  }

  // Calls the handler and reports every stage without throwing.
  [[nodiscard]] DelegateCallResult Invoke(Parameters... Arguments) const {
    if (!BoundValue)
      return DelegateCallResult::Refused(
          DelegateStatus::Released,
          "the delegate names no subscribed handler.");

    std::vector<Value> Staged;
    Staged.reserve(sizeof...(Parameters));
    (Staged.push_back(Value(std::in_place_type<Parameters>,
                            std::forward<Parameters>(Arguments))),
     ...);
    return BoundValue->Call(Staged);
  }

  // Calls the handler and translates every refusal into one exception, so an
  // ordinary std::function subscriber reports failure the C++ way.
  Return operator()(Parameters... Arguments) const {
    DelegateCallResult Result = Invoke(std::forward<Parameters>(Arguments)...);
    if (!Result.IsSuccess())
      throw DelegateFailure(Result.Status(), Result.Diagnostic());
    if constexpr (!std::is_void_v<Return>) {
      const Value *Produced = Result.Produced();
      if (!Produced || !std::holds_alternative<Return>(*Produced))
        throw DelegateFailure(DelegateStatus::ResultMismatch,
                              "the subscribed handler published no value of "
                              "the declared result type.");
      return std::get<Return>(*Produced);
    }
  }

private:
  std::shared_ptr<Detail::DelegateTarget> BoundValue;
};

} // namespace Luna
