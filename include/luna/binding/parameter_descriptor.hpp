#pragma once

// The reflected call shape of one callable parameter.
//
// A parameter is required, optional, defaulted, or variadic, and the difference
// is metadata rather than a second registration path:
//
//   * A required parameter must be supplied.
//   * An optional parameter maps both omission and an explicit nil to the empty
//     value; a present non-nil value converts as its ordinary type.
//   * A defaulted parameter carries one immutable Luna-owned default value. The
//     default applies only to omission: an explicit nil is a supplied value and
//     follows the parameter's ordinary conversion, which accepts nil only when
//     the parameter type itself accepts nil. The default is validated here, at
//     registration, and materialized only after its candidate is selected.
//   * A variadic parameter consumes every remaining call argument. A callable
//     has at most one and it is final.
//
// Nothing in this header names a virtual machine, and no macro is required to
// describe a shape.

// clang-format off
#include <luna/binding/supported_callable.hpp>
#include <luna/binding/value.hpp>

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

// Call-shape form of one parameter.
enum class ParameterForm { Required, Optional, Defaulted, Variadic };

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
  }
  return "required";
}

// The foundation value kind one value carries.
[[nodiscard]] constexpr ValueKind ValueKindOf(const Value &Source) noexcept {
  if (std::holds_alternative<bool>(Source))
    return ValueKind::Boolean;
  if (std::holds_alternative<int>(Source))
    return ValueKind::Integer;
  if (std::holds_alternative<double>(Source))
    return ValueKind::Number;
  return ValueKind::String;
}

// One immutable parameter descriptor. A variadic parameter has no single value
// kind; every other form names exactly one.
class ParameterDescriptor final {
public:
  ParameterDescriptor() = default;

  [[nodiscard]] static ParameterDescriptor ForRequired(ValueKind Kind) {
    ParameterDescriptor Descriptor;
    Descriptor.FormValue = ParameterForm::Required;
    Descriptor.KindValue = Kind;
    return Descriptor;
  }

  // A trailing optional parameter. Omission and explicit nil are both empty.
  [[nodiscard]] static ParameterDescriptor ForOptional(ValueKind Kind) {
    ParameterDescriptor Descriptor;
    Descriptor.FormValue = ParameterForm::Optional;
    Descriptor.KindValue = Kind;
    Descriptor.AcceptsNilValue = true;
    return Descriptor;
  }

  // A defaulted parameter. `AcceptsNil` states whether the parameter's own type
  // converts an explicit nil, which is the only way an explicit nil is accepted
  // for a defaulted parameter.
  [[nodiscard]] static ParameterDescriptor
  ForDefaulted(ValueKind Kind, Value Default, bool AcceptsNil = false) {
    ParameterDescriptor Descriptor;
    Descriptor.FormValue = ParameterForm::Defaulted;
    Descriptor.KindValue = Kind;
    Descriptor.DefaultValue = std::move(Default);
    Descriptor.AcceptsNilValue = AcceptsNil;
    return Descriptor;
  }

  // The final variadic parameter. `Retains` selects the owning `ArgumentPack`
  // form over the callback-lifetime `ArgumentView` form.
  [[nodiscard]] static ParameterDescriptor ForVariadic(bool Retains) {
    ParameterDescriptor Descriptor;
    Descriptor.FormValue = ParameterForm::Variadic;
    Descriptor.RetainsValue = Retains;
    return Descriptor;
  }

  [[nodiscard]] ParameterForm Form() const noexcept { return FormValue; }

  [[nodiscard]] const ValueKind *Kind() const noexcept {
    return KindValue ? &*KindValue : nullptr;
  }

  [[nodiscard]] bool IsVariadic() const noexcept {
    return FormValue == ParameterForm::Variadic;
  }

  // A variadic parameter that retains its arguments beyond the invocation.
  [[nodiscard]] bool Retains() const noexcept { return RetainsValue; }

  // The parameter may be omitted: it is optional, defaulted, or variadic.
  [[nodiscard]] bool IsOmittable() const noexcept {
    return FormValue != ParameterForm::Required;
  }

  // An explicit nil is a value this parameter's own conversion accepts.
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
           Left.RetainsValue == Right.RetainsValue;
  }

  [[nodiscard]] friend bool operator!=(const ParameterDescriptor &Left,
                                       const ParameterDescriptor &Right) {
    return !(Left == Right);
  }

private:
  ParameterForm FormValue = ParameterForm::Required;
  std::optional<ValueKind> KindValue;
  std::optional<Value> DefaultValue;
  bool AcceptsNilValue = false;
  bool RetainsValue = false;
};

// First deterministic reason one declared parameter shape is refused:
//
//   * `RequiredAfterRelaxed` - a required parameter follows an optional or
//     defaulted one.
//   * `VariadicNotFinal` - a variadic parameter is not the final one, or there
//     is more than one.
//   * `MissingValueKind` - a parameter names no value kind, or a variadic
//     parameter names one.
//   * `MisplacedDefault` - a defaulted parameter carries no default, or another
//     form carries one.
//   * `DefaultTypeMismatch` - the default value's type is not the parameter's
//     declared type.
enum class ParameterShapeStatus {
  Valid,
  RequiredAfterRelaxed,
  VariadicNotFinal,
  MissingValueKind,
  MisplacedDefault,
  DefaultTypeMismatch
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
  }
  return "valid";
}

// One refusal, with the one-based parameter position it belongs to. Position
// zero means the shape as a whole is refused.
struct ParameterShapeIssue final {
  ParameterShapeStatus Status = ParameterShapeStatus::Valid;
  std::size_t Position = 0;

  [[nodiscard]] bool IsValid() const noexcept {
    return Status == ParameterShapeStatus::Valid;
  }
};

// Validates one declared parameter shape. Every rule is decided in ascending
// parameter position, so an equivalent shape always reports one identical first
// refusal.
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

// The call arity one declared shape accepts. A variadic shape has no maximum.
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

// One target plus the immutable default values of its trailing parameters. The
// wrapper's own call operator is exactly the declared signature, so the
// ordinary descriptor builder reads the same shape a plain function would
// declare and the defaults ride along as metadata.
template <class Signature, class Target> class DefaultedCallable;

template <class Return, class... Parameters, class Target>
class DefaultedCallable<Return(Parameters...), Target> final {
public:
  // Only a fixed parameter can carry a default; a variadic tail never does.
  static constexpr std::size_t FixedParameterCount =
      (std::size_t{0} + ... +
       (IsVariadicParameterType<Parameters> ? std::size_t{0} : std::size_t{1}));

  DefaultedCallable(Target Selected, std::vector<Value> Defaults)
      : TargetValue(std::move(Selected)), DefaultsValue(std::move(Defaults)) {}

  Return operator()(Parameters... Arguments) {
    return std::invoke(TargetValue, std::forward<Parameters>(Arguments)...);
  }

  // Defaults of the trailing parameters, in declared order.
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

// One consumer default normalized into a foundation value. Only the foundation
// value types are defaults, so a malformed default is a compile-time rejection
// rather than a runtime surprise.
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

// Declares immutable default values for the trailing parameters of one target.
// The defaults are validated and reflected at registration; each one applies
// only when its parameter is omitted.
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
