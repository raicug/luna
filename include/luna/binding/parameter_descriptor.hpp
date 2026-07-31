#pragma once

// clang-format off
#include <luna/binding/conversion.hpp>
#include <luna/binding/supported_callable.hpp>
#include <luna/binding/value.hpp>
#include <luna/type/stable_type_key.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace Luna {

enum class ParameterForm {
  Required,
  Optional,
  Defaulted,
  Variadic,
  Delegate,
  Converted,
  Instance
};

[[nodiscard]] constexpr std::string_view
ParameterFormText(ParameterForm Form) noexcept {
  switch (Form) {
  case ParameterForm::Required:
    return "required";
  case ParameterForm::Optional:
    return "optional";
  case ParameterForm::Defaulted:
    return "defaulted";
  case ParameterForm::Variadic:
    return "variadic";
  case ParameterForm::Delegate:
    return "delegate";
  case ParameterForm::Converted:
    return "converted";
  case ParameterForm::Instance:
    return "instance";
  }
  return "required";
}

struct ConvertedParameterShape final {
  using ProbeFunction = ConversionProbe (*)(ValueView Source,
                                            const ConversionContext &Context);

  ProbeFunction Probe = nullptr;

  [[nodiscard]] friend bool operator==(const ConvertedParameterShape &Left,
                                       const ConvertedParameterShape &Right) {
    return Left.Probe == Right.Probe;
  }

  [[nodiscard]] friend bool operator!=(const ConvertedParameterShape &Left,
                                       const ConvertedParameterShape &Right) {
    return !(Left == Right);
  }
};

struct InstanceParameterShape final {
  Detail::ClassKeyResolver Resolve = nullptr;
  bool RequiresMutation = false;

  [[nodiscard]] const StableTypeKey &Class() const {
    static const StableTypeKey Undeclared;
    return Resolve ? Resolve() : Undeclared;
  }

  [[nodiscard]] friend bool operator==(const InstanceParameterShape &Left,
                                       const InstanceParameterShape &Right) {
    return Left.Resolve == Right.Resolve &&
           Left.RequiresMutation == Right.RequiresMutation;
  }

  [[nodiscard]] friend bool operator!=(const InstanceParameterShape &Left,
                                       const InstanceParameterShape &Right) {
    return !(Left == Right);
  }
};

[[nodiscard]] constexpr ValueKind ValueKindOf(const Value &Source) noexcept {
  if (std::holds_alternative<bool>(Source))
    return ValueKind::Boolean;
  if (std::holds_alternative<int>(Source))
    return ValueKind::Integer;
  if (std::holds_alternative<double>(Source))
    return ValueKind::Number;
  return ValueKind::String;
}

class ParameterDescriptor final {
public:
  ParameterDescriptor() = default;

  [[nodiscard]] static ParameterDescriptor ForRequired(ValueKind Kind) {
    ParameterDescriptor Descriptor;
    Descriptor.FormValue = ParameterForm::Required;
    Descriptor.KindValue = Kind;
    return Descriptor;
  }

  [[nodiscard]] static ParameterDescriptor ForOptional(ValueKind Kind) {
    ParameterDescriptor Descriptor;
    Descriptor.FormValue = ParameterForm::Optional;
    Descriptor.KindValue = Kind;
    Descriptor.AcceptsNilValue = true;
    return Descriptor;
  }

  [[nodiscard]] static ParameterDescriptor
  ForDefaulted(ValueKind Kind, Value Default, bool AcceptsNil = false) {
    ParameterDescriptor Descriptor;
    Descriptor.FormValue = ParameterForm::Defaulted;
    Descriptor.KindValue = Kind;
    Descriptor.DefaultValue = std::move(Default);
    Descriptor.AcceptsNilValue = AcceptsNil;
    return Descriptor;
  }

  [[nodiscard]] static ParameterDescriptor ForVariadic(bool Retains) {
    ParameterDescriptor Descriptor;
    Descriptor.FormValue = ParameterForm::Variadic;
    Descriptor.RetainsValue = Retains;
    return Descriptor;
  }

  [[nodiscard]] static ParameterDescriptor ForDelegate(DelegateShape Declared) {
    ParameterDescriptor Descriptor;
    Descriptor.FormValue = ParameterForm::Delegate;
    Descriptor.DelegateValue = std::move(Declared);
    return Descriptor;
  }

  [[nodiscard]] static ParameterDescriptor
  ForConverted(StableTypeKey Key, ConvertedParameterShape Declared) {
    ParameterDescriptor Descriptor;
    Descriptor.FormValue = ParameterForm::Converted;
    Descriptor.ConvertedKeyValue = std::move(Key);
    Descriptor.ConvertedValue = std::move(Declared);
    return Descriptor;
  }

  [[nodiscard]] static ParameterDescriptor
  ForInstance(InstanceParameterShape Declared) {
    ParameterDescriptor Descriptor;
    Descriptor.FormValue = ParameterForm::Instance;
    Descriptor.InstanceValue = std::move(Declared);
    return Descriptor;
  }

  [[nodiscard]] ParameterForm Form() const noexcept { return FormValue; }

  [[nodiscard]] const ValueKind *Kind() const noexcept {
    return KindValue ? &*KindValue : nullptr;
  }

  [[nodiscard]] bool IsVariadic() const noexcept {
    return FormValue == ParameterForm::Variadic;
  }

  [[nodiscard]] bool IsDelegate() const noexcept {
    return FormValue == ParameterForm::Delegate;
  }

  [[nodiscard]] bool IsConverted() const noexcept {
    return FormValue == ParameterForm::Converted;
  }

  [[nodiscard]] bool IsInstance() const noexcept {
    return FormValue == ParameterForm::Instance;
  }

  [[nodiscard]] const InstanceParameterShape *
  InstanceSignature() const noexcept {
    return InstanceValue ? &*InstanceValue : nullptr;
  }

  [[nodiscard]] const StableTypeKey *InstanceKey() const {
    if (!InstanceValue)
      return nullptr;
    return &InstanceValue->Class();
  }

  [[nodiscard]] const DelegateShape *DelegateSignature() const noexcept {
    return DelegateValue ? &*DelegateValue : nullptr;
  }

  [[nodiscard]] const StableTypeKey *ConvertedKey() const noexcept {
    return FormValue == ParameterForm::Converted ? &ConvertedKeyValue : nullptr;
  }

  [[nodiscard]] const ConvertedParameterShape *
  ConvertedSignature() const noexcept {
    return ConvertedValue ? &*ConvertedValue : nullptr;
  }

  [[nodiscard]] bool Retains() const noexcept { return RetainsValue; }

  [[nodiscard]] bool IsOmittable() const noexcept {
    return FormValue != ParameterForm::Required &&
           FormValue != ParameterForm::Delegate &&
           FormValue != ParameterForm::Converted &&
           FormValue != ParameterForm::Instance;
  }

  [[nodiscard]] bool AcceptsNil() const noexcept { return AcceptsNilValue; }

  [[nodiscard]] bool HasDefault() const noexcept {
    return DefaultValue.has_value();
  }

  [[nodiscard]] const Value *Default() const noexcept {
    return DefaultValue ? &*DefaultValue : nullptr;
  }

  [[nodiscard]] friend bool operator==(const ParameterDescriptor &Left,
                                       const ParameterDescriptor &Right) {
    return Left.FormValue == Right.FormValue &&
           Left.KindValue == Right.KindValue &&
           Left.DefaultValue == Right.DefaultValue &&
           Left.AcceptsNilValue == Right.AcceptsNilValue &&
           Left.RetainsValue == Right.RetainsValue &&
           Left.DelegateValue == Right.DelegateValue &&
           Left.ConvertedKeyValue == Right.ConvertedKeyValue &&
           Left.ConvertedValue == Right.ConvertedValue &&
           Left.InstanceValue == Right.InstanceValue;
  }

  [[nodiscard]] friend bool operator!=(const ParameterDescriptor &Left,
                                       const ParameterDescriptor &Right) {
    return !(Left == Right);
  }

private:
  ParameterForm FormValue = ParameterForm::Required;
  std::optional<ValueKind> KindValue;
  std::optional<Value> DefaultValue;
  std::optional<DelegateShape> DelegateValue;
  StableTypeKey ConvertedKeyValue;
  std::optional<ConvertedParameterShape> ConvertedValue;
  std::optional<InstanceParameterShape> InstanceValue;
  bool AcceptsNilValue = false;
  bool RetainsValue = false;
};

enum class ParameterShapeStatus {
  Valid,
  RequiredAfterRelaxed,
  VariadicNotFinal,
  MissingValueKind,
  MisplacedDefault,
  DefaultTypeMismatch,
  MalformedDelegate,
  MalformedConverted,
  UnregisteredInstanceClass
};

[[nodiscard]] constexpr std::string_view
ParameterShapeStatusText(ParameterShapeStatus Status) noexcept {
  switch (Status) {
  case ParameterShapeStatus::Valid:
    return "valid";
  case ParameterShapeStatus::RequiredAfterRelaxed:
    return "required_after_relaxed";
  case ParameterShapeStatus::VariadicNotFinal:
    return "variadic_not_final";
  case ParameterShapeStatus::MissingValueKind:
    return "missing_value_kind";
  case ParameterShapeStatus::MisplacedDefault:
    return "misplaced_default";
  case ParameterShapeStatus::DefaultTypeMismatch:
    return "default_type_mismatch";
  case ParameterShapeStatus::MalformedDelegate:
    return "malformed_delegate";
  case ParameterShapeStatus::MalformedConverted:
    return "malformed_converted";
  case ParameterShapeStatus::UnregisteredInstanceClass:
    return "unregistered_instance_class";
  }
  return "valid";
}

struct ParameterShapeIssue final {
  ParameterShapeStatus Status = ParameterShapeStatus::Valid;
  std::size_t Position = 0;

  [[nodiscard]] bool IsValid() const noexcept {
    return Status == ParameterShapeStatus::Valid;
  }
};

[[nodiscard]] inline ParameterShapeIssue
ValidateParameterShape(std::span<const ParameterDescriptor> Parameters) {
  bool SawRelaxed = false;
  for (std::size_t Index = 0; Index < Parameters.size(); ++Index) {
    const ParameterDescriptor &Parameter = Parameters[Index];
    const std::size_t Position = Index + 1;

    if (Parameter.IsVariadic()) {
      if (Position != Parameters.size())
        return ParameterShapeIssue{ParameterShapeStatus::VariadicNotFinal,
                                   Position};
      if (Parameter.Kind() != nullptr)
        return ParameterShapeIssue{ParameterShapeStatus::MissingValueKind,
                                   Position};
      if (Parameter.HasDefault())
        return ParameterShapeIssue{ParameterShapeStatus::MisplacedDefault,
                                   Position};
      continue;
    }

    if (Parameter.IsDelegate()) {
      const DelegateShape *Declared = Parameter.DelegateSignature();
      if (Declared == nullptr || Parameter.Kind() != nullptr ||
          Parameter.HasDefault() || Parameter.AcceptsNil() ||
          Parameter.Retains())
        return ParameterShapeIssue{ParameterShapeStatus::MalformedDelegate,
                                   Position};
      for (const DelegateParameterShape &Staged : Declared->Parameters) {
        if (Staged.Form != DelegateValueForm::Instance)
          continue;
        if (Staged.Resolve == nullptr || !Staged.Resolve().IsValid())
          return ParameterShapeIssue{
              ParameterShapeStatus::UnregisteredInstanceClass, Position};
      }
      if (SawRelaxed)
        return ParameterShapeIssue{ParameterShapeStatus::RequiredAfterRelaxed,
                                   Position};
      continue;
    }

    if (Parameter.IsConverted()) {
      if (Parameter.ConvertedSignature() == nullptr ||
          Parameter.ConvertedKey() == nullptr ||
          !Parameter.ConvertedKey()->IsValid() || Parameter.Kind() != nullptr ||
          Parameter.HasDefault() || Parameter.AcceptsNil() ||
          Parameter.Retains())
        return ParameterShapeIssue{ParameterShapeStatus::MalformedConverted,
                                   Position};
      if (SawRelaxed)
        return ParameterShapeIssue{ParameterShapeStatus::RequiredAfterRelaxed,
                                   Position};
      continue;
    }

    if (Parameter.IsInstance()) {
      const InstanceParameterShape *Declared = Parameter.InstanceSignature();
      if (Declared == nullptr || Parameter.Kind() != nullptr ||
          Parameter.HasDefault() || Parameter.AcceptsNil() ||
          Parameter.Retains())
        return ParameterShapeIssue{
            ParameterShapeStatus::UnregisteredInstanceClass, Position};

      if (!Declared->Class().IsValid())
        return ParameterShapeIssue{
            ParameterShapeStatus::UnregisteredInstanceClass, Position};
      if (SawRelaxed)
        return ParameterShapeIssue{ParameterShapeStatus::RequiredAfterRelaxed,
                                   Position};
      continue;
    }

    if (Parameter.DelegateSignature() != nullptr)
      return ParameterShapeIssue{ParameterShapeStatus::MalformedDelegate,
                                 Position};
    if (Parameter.InstanceSignature() != nullptr)
      return ParameterShapeIssue{
          ParameterShapeStatus::UnregisteredInstanceClass, Position};
    if (Parameter.ConvertedSignature() != nullptr)
      return ParameterShapeIssue{ParameterShapeStatus::MalformedConverted,
                                 Position};

    const ValueKind *Kind = Parameter.Kind();
    if (Kind == nullptr)
      return ParameterShapeIssue{ParameterShapeStatus::MissingValueKind,
                                 Position};

    if (Parameter.Form() == ParameterForm::Required && SawRelaxed)
      return ParameterShapeIssue{ParameterShapeStatus::RequiredAfterRelaxed,
                                 Position};

    const bool IsDefaulted = Parameter.Form() == ParameterForm::Defaulted;
    if (IsDefaulted != Parameter.HasDefault())
      return ParameterShapeIssue{ParameterShapeStatus::MisplacedDefault,
                                 Position};

    if (const Value *Default = Parameter.Default()) {
      if (ValueKindOf(*Default) != *Kind)
        return ParameterShapeIssue{ParameterShapeStatus::DefaultTypeMismatch,
                                   Position};
    }

    if (Parameter.IsOmittable())
      SawRelaxed = true;
  }
  return ParameterShapeIssue();
}

struct ParameterArity final {
  std::size_t Minimum = 0;
  std::optional<std::size_t> Maximum;
  std::size_t FixedCount = 0;
  bool IsVariadic = false;
};

[[nodiscard]] inline ParameterArity
ArityOf(std::span<const ParameterDescriptor> Parameters) {
  ParameterArity Arity;
  for (const ParameterDescriptor &Parameter : Parameters) {
    if (Parameter.IsVariadic()) {
      Arity.IsVariadic = true;
      continue;
    }
    ++Arity.FixedCount;
    if (!Parameter.IsOmittable())
      ++Arity.Minimum;
  }
  if (!Arity.IsVariadic)
    Arity.Maximum = Arity.FixedCount;
  return Arity;
}

namespace Detail {

template <class Signature, class Target> class DefaultedCallable;

template <class Return, class... Parameters, class Target>
class DefaultedCallable<Return(Parameters...), Target> final {
public:
  static constexpr std::size_t FixedParameterCount =
      (std::size_t{0} + ... +
       (IsVariadicParameterType<Parameters> ? std::size_t{0} : std::size_t{1}));

  DefaultedCallable(Target Selected, std::vector<Value> Defaults)
      : TargetValue(std::move(Selected)), DefaultsValue(std::move(Defaults)) {}

  Return operator()(Parameters... Arguments) {
    return std::invoke(TargetValue, std::forward<Parameters>(Arguments)...);
  }

  [[nodiscard]] std::span<const Value> Defaults() const noexcept {
    return DefaultsValue;
  }

private:
  Target TargetValue;
  std::vector<Value> DefaultsValue;
};

template <class Type> struct IsDefaultedCallable : std::false_type {};

template <class Signature, class Target>
struct IsDefaultedCallable<DefaultedCallable<Signature, Target>>
    : std::true_type {};

[[nodiscard]] inline Value NormalizedDefault(bool Source) {
  return Value(Source);
}

[[nodiscard]] inline Value NormalizedDefault(int Source) {
  return Value(Source);
}

[[nodiscard]] inline Value NormalizedDefault(double Source) {
  return Value(Source);
}

[[nodiscard]] inline Value NormalizedDefault(std::string Source) {
  return Value(std::move(Source));
}

[[nodiscard]] inline Value NormalizedDefault(const char *Source) {
  return Value(std::string(Source ? Source : ""));
}

} // namespace Detail

template <class Callable, class... Defaults>
[[nodiscard]] auto WithDefaults(Callable &&Target, Defaults &&...Values) {
  using Normalized = std::remove_cvref_t<Callable>;
  using Signature = typename Detail::CallableSignature<Normalized>::Type;
  using Wrapper = Detail::DefaultedCallable<Signature, std::decay_t<Callable>>;

  static_assert(sizeof...(Defaults) <= Wrapper::FixedParameterCount,
                "a callable cannot declare more defaults than it has fixed "
                "parameters");

  std::vector<Value> Collected{
      Detail::NormalizedDefault(std::forward<Defaults>(Values))...};
  return Wrapper(std::forward<Callable>(Target), std::move(Collected));
}

} // namespace Luna
