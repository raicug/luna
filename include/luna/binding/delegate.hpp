#pragma once

// clang-format off
#include <luna/binding/class_construction.hpp>
#include <luna/binding/conversion.hpp>
#include <luna/binding/registered_class.hpp>
#include <luna/binding/value.hpp>
#include <luna/type/stable_type_key.hpp>

#include <cstddef>
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

enum class DelegateValueForm { Scalar, Instance, Owned, Pack };

[[nodiscard]] constexpr std::string_view
DelegateValueFormText(DelegateValueForm Form) noexcept {
  switch (Form) {
  case DelegateValueForm::Scalar:
    return "scalar";
  case DelegateValueForm::Instance:
    return "instance";
  case DelegateValueForm::Owned:
    return "owned";
  case DelegateValueForm::Pack:
    return "pack";
  }
  return "scalar";
}

struct DelegateParameterShape final {
  DelegateValueForm Form = DelegateValueForm::Scalar;
  ValueKind Kind = ValueKind::Boolean;
  Detail::ClassKeyResolver Resolve = nullptr;

  [[nodiscard]] friend bool
  operator==(const DelegateParameterShape &Left,
             const DelegateParameterShape &Right) = default;
};

struct DelegateShape final {
  std::vector<DelegateParameterShape> Parameters;
  std::optional<ValueKind> Result;

  [[nodiscard]] bool CarriesObjects() const noexcept {
    for (const DelegateParameterShape &Declared : Parameters) {
      if (Declared.Form != DelegateValueForm::Scalar)
        return true;
    }
    return false;
  }

  [[nodiscard]] std::size_t FixedParameterCount() const noexcept {
    std::size_t Fixed = 0;
    for (const DelegateParameterShape &Declared : Parameters) {
      if (Declared.Form != DelegateValueForm::Pack)
        Fixed += 1;
    }
    return Fixed;
  }

  [[nodiscard]] bool CarriesPack() const noexcept {
    return FixedParameterCount() != Parameters.size();
  }

  [[nodiscard]] friend bool operator==(const DelegateShape &Left,
                                       const DelegateShape &Right) = default;
};

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

template <class Type>
inline constexpr bool IsDelegateOwnedType =
    std::is_same_v<std::remove_cvref_t<Type>, OwnedValue>;

template <class Type>
inline constexpr bool IsDelegatePackType =
    std::is_same_v<std::remove_cvref_t<Type>, ValuePack>;

template <class Type>
inline constexpr bool IsDelegateInstanceType = IsInstanceReturnType<Type>;

template <class Type>
inline constexpr bool IsDelegateParameterValue =
    IsDelegateValueType<Type> || IsDelegateOwnedType<Type> ||
    IsDelegatePackType<Type> || IsDelegateInstanceType<Type>;

template <class... Parameters> struct DelegatePackPosition;

template <> struct DelegatePackPosition<> {
  static constexpr bool IsValid = true;
};

template <class Final> struct DelegatePackPosition<Final> {
  static constexpr bool IsValid = true;
};

template <class First, class... Rest>
struct DelegatePackPosition<First, Rest...> {
  static constexpr bool IsValid =
      !IsDelegatePackType<First> && DelegatePackPosition<Rest...>::IsValid;
};

template <class Parameter>
[[nodiscard]] DelegateParameterShape DelegateParameterShapeOf() {
  DelegateParameterShape Declared;
  if constexpr (IsDelegateOwnedType<Parameter>) {
    Declared.Form = DelegateValueForm::Owned;
  } else if constexpr (IsDelegatePackType<Parameter>) {
    Declared.Form = DelegateValueForm::Pack;
  } else if constexpr (IsDelegateInstanceType<Parameter>) {
    Declared.Form = DelegateValueForm::Instance;
    Declared.Resolve =
        ClassKeyResolverFor<typename InstanceReturnTrait<Parameter>::Native>();
  } else {
    Declared.Form = DelegateValueForm::Scalar;
    Declared.Kind = DelegateValueKind<Parameter>();
  }
  return Declared;
}

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

  [[nodiscard]] virtual DelegateCallResult
  CallOwned(std::span<const OwnedValue> Arguments) {
    static_cast<void>(Arguments);
    return DelegateCallResult::Refused(
        DelegateStatus::HandlerFailed,
        "this subscribed handler does not accept arguments carrying objects.");
  }

  virtual void Release() noexcept = 0;
};

template <class Signature> struct DelegateSignatureShape;

template <class Return, class... Parameters>
struct DelegateSignatureShape<Return(Parameters...)> {
  static constexpr bool IsSupported =
      (std::is_void_v<Return> || IsDelegateValueType<Return>) &&
      (IsDelegateParameterValue<Parameters> && ...) &&
      DelegatePackPosition<Parameters...>::IsValid;

  static constexpr bool CarriesObjects =
      (false || ... || !IsDelegateValueType<Parameters>);

  [[nodiscard]] static DelegateShape Shape() {
    DelegateShape Declared;
    Declared.Parameters = std::vector<DelegateParameterShape>{
        DelegateParameterShapeOf<Parameters>()...};
    if constexpr (!std::is_void_v<Return>)
      Declared.Result = DelegateValueKind<Return>();
    return Declared;
  }
};

} // namespace Detail

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

  void DeclareOwnership(OwnershipPolicy Declared) {
    OwnershipValue = std::move(Declared);
  }

  [[nodiscard]] const OwnershipPolicy &Ownership() const noexcept {
    return OwnershipValue;
  }

  [[nodiscard]] DelegateCallResult Invoke(Parameters... Arguments) const {
    if (!BoundValue)
      return DelegateCallResult::Refused(
          DelegateStatus::Released,
          "the delegate names no subscribed handler.");

    if constexpr (Detail::DelegateSignatureShape<
                      SignatureType>::CarriesObjects) {
      std::vector<OwnedValue> Staged;
      Staged.reserve(sizeof...(Parameters));
      (StageOwned(Staged, std::forward<Parameters>(Arguments)), ...);
      return BoundValue->CallOwned(Staged);
    } else {
      std::vector<Value> Staged;
      Staged.reserve(sizeof...(Parameters));
      (Staged.push_back(Value(std::in_place_type<Parameters>,
                              std::forward<Parameters>(Arguments))),
       ...);
      return BoundValue->Call(Staged);
    }
  }

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
  template <class Parameter>
  void StageOwned(std::vector<OwnedValue> &Staged, Parameter &&Argument) const {
    using Declared = std::remove_cvref_t<Parameter>;
    if constexpr (Detail::IsDelegatePackType<Declared>) {
      for (std::size_t Index = 0; Index < Argument.Size(); ++Index)
        Staged.push_back(Argument.At(Index));
    } else if constexpr (Detail::IsDelegateOwnedType<Declared>) {
      Staged.push_back(std::forward<Parameter>(Argument));
    } else if constexpr (Detail::IsDelegateValueType<Declared>) {
      Staged.push_back(OwnedValue::FromValue(Value(
          std::in_place_type<Declared>, std::forward<Parameter>(Argument))));
    } else if constexpr (std::is_pointer_v<Declared>) {
      using Native = typename Detail::InstanceReturnTrait<Declared>::Native;
      Staged.push_back(OwnedValue::Instance<Native>(Argument, OwnershipValue));
    } else {
      using Native = typename Detail::InstanceReturnTrait<Declared>::Native;
      Staged.push_back(
          OwnedValue::Instance<Native>(std::forward<Parameter>(Argument)));
    }
  }

  std::shared_ptr<Detail::DelegateTarget> BoundValue;
  OwnershipPolicy OwnershipValue = OwnershipPolicy::LuaOwned();
};

} // namespace Luna
