// Declared parameter shapes: optional, defaulted, and variadic parameters.
//
// The checks here cover the registration half of the shape - what the adapter
// describes, what the canonical signature carries, which shapes registration
// refuses, and what reflection would report - plus the two Luna-owned variadic
// forms and their lifetimes. Invocation through the real virtual machine is
// covered by the declared-parameter integration case.

// clang-format off
#include <luna/binding/argument_pack.hpp>
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/conversion.hpp>
#include <luna/binding/parameter_descriptor.hpp>
#include <luna/binding/supported_callable.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/detail/callable_adapter.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/state/state.hpp>

#include "state/invocation/parameters/argument_frame.hpp"
#include "state/registration/parameter_shape.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "parameter shape check failed: " << Description << '\n';
}

using Luna::ParameterDescriptor;
using Luna::ParameterForm;
using Luna::ParameterShapeStatus;
using Luna::ValueKind;

[[nodiscard]] int Scaled(int Value, std::optional<int> Factor) {
  return Value * (Factor ? *Factor : 1);
}

[[nodiscard]] int Offset(int Value, int Amount) { return Value + Amount; }

// A required parameter after an optional one is a shape registration refuses.
[[nodiscard]] int Misordered(std::optional<int> First, int Second) {
  return (First ? *First : 0) + Second;
}

[[nodiscard]] int CountedView(Luna::ArgumentView Arguments) {
  return static_cast<int>(Arguments.Size());
}

[[nodiscard]] int CountedPack(int Base, Luna::ArgumentPack Arguments) {
  return Base + static_cast<int>(Arguments.Size());
}

static_assert(Luna::SupportedCallable<decltype(&Scaled)>,
              "a trailing optional parameter is one declared call shape");
static_assert(Luna::SupportedCallable<decltype(&CountedView)>,
              "the callback-lifetime variadic view is a declared parameter");
static_assert(Luna::SupportedCallable<decltype(&CountedPack)>,
              "the owning variadic pack is a declared parameter");
static_assert(Luna::SupportedCallable<decltype(&Misordered)>,
              "a misordered shape compiles and is refused at registration");

// At most one variadic parameter, and only as the final one.
static_assert(!Luna::SupportedCallable<int (*)(Luna::ArgumentView, int)>,
              "a variadic parameter is never followed by another parameter");
static_assert(
    !Luna::SupportedCallable<int (*)(Luna::ArgumentView, Luna::ArgumentPack)>,
    "a callable declares at most one variadic parameter");

[[nodiscard]] std::string
FailureMessage(const Luna::RegistrationResult &Result) {
  const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
  return Diagnostic ? Diagnostic->Message() : std::string();
}

void CheckShapeValidationRules() {
  const std::vector<ParameterDescriptor> Valid{
      ParameterDescriptor::ForRequired(ValueKind::Integer),
      ParameterDescriptor::ForDefaulted(ValueKind::Integer, Luna::Value(2)),
      ParameterDescriptor::ForOptional(ValueKind::String),
      ParameterDescriptor::ForVariadic(false)};
  const auto Accepted = Luna::ValidateParameterShape(Valid);
  Check(Accepted.IsValid(),
        "required, defaulted, optional, and a final variadic form one shape");

  const Luna::ParameterArity Arity = Luna::ArityOf(Valid);
  Check(Arity.Minimum == 1 && Arity.FixedCount == 3 && Arity.IsVariadic &&
            !Arity.Maximum,
        "a variadic shape has one minimum arity and no maximum");

  const std::vector<ParameterDescriptor> Misordered{
      ParameterDescriptor::ForOptional(ValueKind::Integer),
      ParameterDescriptor::ForRequired(ValueKind::Integer)};
  const auto RequiredAfter = Luna::ValidateParameterShape(Misordered);
  Check(RequiredAfter.Status == ParameterShapeStatus::RequiredAfterRelaxed &&
            RequiredAfter.Position == 2,
        "a required parameter after an optional one is refused by position");

  const std::vector<ParameterDescriptor> EarlyVariadic{
      ParameterDescriptor::ForVariadic(false),
      ParameterDescriptor::ForRequired(ValueKind::Integer)};
  const auto NotFinal = Luna::ValidateParameterShape(EarlyVariadic);
  Check(NotFinal.Status == ParameterShapeStatus::VariadicNotFinal &&
            NotFinal.Position == 1,
        "a variadic parameter that is not final is refused");

  const std::vector<ParameterDescriptor> WrongDefault{
      ParameterDescriptor::ForDefaulted(ValueKind::Integer,
                                        Luna::Value(std::string("two")))};
  const auto Mismatched = Luna::ValidateParameterShape(WrongDefault);
  Check(Mismatched.Status == ParameterShapeStatus::DefaultTypeMismatch &&
            Mismatched.Position == 1,
        "a default of another type is refused at its own position");

  const std::vector<ParameterDescriptor> Empty;
  Check(Luna::ValidateParameterShape(Empty).IsValid(),
        "a callable with no parameters declares a valid shape");
}

void CheckAdapterDescribesDeclaredShapes() {
  const auto OptionalDescriptor =
      Luna::Detail::MakeErasedCallableDescriptor(&Scaled);
  const Luna::CallableMetadata &OptionalMetadata =
      OptionalDescriptor.Metadata();
  Check(OptionalMetadata.HasRichParameters() &&
            OptionalMetadata.ParameterTypes().empty(),
        "a declared shape is described by parameter descriptors");
  const auto OptionalParameters = OptionalMetadata.Parameters();
  Check(OptionalParameters.size() == 2 &&
            OptionalParameters[0].Form() == ParameterForm::Required &&
            OptionalParameters[1].Form() == ParameterForm::Optional &&
            OptionalParameters[1].AcceptsNil(),
        "a trailing optional parameter accepts nil and omission");

  const auto DefaultedDescriptor = Luna::Detail::MakeErasedCallableDescriptor(
      Luna::WithDefaults(&Offset, 5));
  const auto DefaultedParameters = DefaultedDescriptor.Metadata().Parameters();
  Check(DefaultedParameters.size() == 2 &&
            DefaultedParameters[0].Form() == ParameterForm::Required &&
            DefaultedParameters[1].Form() == ParameterForm::Defaulted,
        "a declared default lands on the trailing parameter");
  const Luna::Value *Default = DefaultedParameters.size() == 2
                                   ? DefaultedParameters[1].Default()
                                   : nullptr;
  Check(Default != nullptr && *Default == Luna::Value(5) &&
            !DefaultedParameters[1].AcceptsNil(),
        "the default is immutable metadata and does not accept nil");

  const auto ViewDescriptor =
      Luna::Detail::MakeErasedCallableDescriptor(&CountedView);
  const auto ViewParameters = ViewDescriptor.Metadata().Parameters();
  Check(ViewParameters.size() == 1 && ViewParameters[0].IsVariadic() &&
            !ViewParameters[0].Retains() && ViewParameters[0].Kind() == nullptr,
        "the callback-lifetime view is one final variadic parameter");

  const auto PackDescriptor =
      Luna::Detail::MakeErasedCallableDescriptor(&CountedPack);
  const auto PackParameters = PackDescriptor.Metadata().Parameters();
  Check(PackParameters.size() == 2 && PackParameters[1].IsVariadic() &&
            PackParameters[1].Retains(),
        "the owning pack is one final variadic parameter that retains");

  // A foundation callable keeps describing itself exactly as before.
  const auto FoundationDescriptor =
      Luna::Detail::MakeErasedCallableDescriptor(&Offset);
  Check(!FoundationDescriptor.Metadata().HasRichParameters() &&
            FoundationDescriptor.Metadata().ParameterTypes().size() == 2,
        "a foundation callable keeps its exact value-kind metadata");
}

void CheckCanonicalSignatureCarriesTheShape() {
  const auto Descriptor = Luna::Detail::MakeErasedCallableDescriptor(
      Luna::WithDefaults(&Scaled, 3));
  const auto Signature =
      Luna::Detail::CanonicalDeclaredSignature(Descriptor.Metadata());
  Check(Signature.ParameterTypes.size() == 2 &&
            Signature.RequiredParameterCount == 1 && !Signature.IsVariadic,
        "an omittable parameter is carried by the required count");

  const auto VariadicDescriptor =
      Luna::Detail::MakeErasedCallableDescriptor(&CountedPack);
  const auto VariadicSignature =
      Luna::Detail::CanonicalDeclaredSignature(VariadicDescriptor.Metadata());
  Check(
      VariadicSignature.ParameterTypes.size() == 1 &&
          VariadicSignature.RequiredParameterCount == 1 &&
          VariadicSignature.IsVariadic,
      "a variadic tail is carried by the variadic flag, not a parameter type");
  Check(VariadicSignature.IsValid(),
        "the canonical signature of a declared shape is complete");
}

void CheckReflectedParametersDistinguishEveryForm() {
  const auto Descriptor = Luna::Detail::MakeErasedCallableDescriptor(
      Luna::WithDefaults(&CountedPack, 7));
  const auto Reflected =
      Luna::Detail::MakeReflectedParameters(Descriptor.Metadata());
  Check(Reflected.size() == 2, "every declared parameter is reflected");
  if (Reflected.size() != 2)
    return;
  Check(Reflected[0].Disposition == Luna::ParameterDisposition::Defaulted &&
            Reflected[0].HasDefault && Reflected[0].DefaultText == "7",
        "a defaulted parameter reflects its immutable default text");
  Check(Reflected[1].Disposition == Luna::ParameterDisposition::Variadic &&
            !Reflected[1].HasDefault,
        "a variadic parameter is reflected as variadic and carries no default");
  Check(Reflected[0].Type.IsValid() && Reflected[1].Type.IsValid() &&
            !Reflected[0].Name.empty() && !Reflected[1].Name.empty(),
        "every reflected parameter names one canonical type");
}

void CheckRegistrationRefusesMalformedShapes() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  const auto Refused = Registry.RegisterFunction("Misordered", &Misordered);
  Check(!Refused.IsSuccess(),
        "a required parameter after an optional one is refused");
  const Luna::ErrorDiagnostic *Diagnostic = Refused.Diagnostic();
  Check(Diagnostic != nullptr &&
            Diagnostic->Category() == Luna::ErrorCategory::Internal,
        "a malformed declared shape is an internal refusal");
  Check(FailureMessage(Refused).find("required parameter 2") !=
            std::string::npos,
        "the refusal names the offending parameter position");

  const auto WrongDefault = Registry.RegisterFunction(
      "WrongDefault", Luna::WithDefaults(&Offset, std::string("five")));
  Check(!WrongDefault.IsSuccess(),
        "a default of another type is refused at registration");
  Check(FailureMessage(WrongDefault).find("default of parameter 2") !=
            std::string::npos,
        "the refusal names the parameter whose default disagrees");

  const auto Accepted =
      Registry.RegisterFunction("Scaled", Luna::WithDefaults(&Scaled, 4));
  Check(Accepted.IsSuccess(),
        "a valid declared shape registers after a refused one");
}

void CheckVariadicFormsAndTheirLifetimes() {
  Luna::ValuePack Values;
  Values.Append(Luna::OwnedValue::Number(1.0));
  Values.Append(Luna::OwnedValue::Text("two"));
  const Luna::ArgumentPack Pack(std::move(Values), 3);

  Check(Pack.Size() == 2 && Pack.FirstPosition() == 3 && Pack.Position(1) == 4,
        "an owning pack reports the one-based call position of each element");

  Luna::ArgumentView Retained;
  {
    Luna::Detail::ResetArgumentBoundaryDiagnostics();
    Luna::Detail::ArgumentFrame Frame(Pack);
    const Luna::ArgumentView View = Frame.View();
    Check(View.IsActive() && View.Size() == 2 && View.Position(0) == 3,
          "a live view reports its frame's arguments and positions");
    Check(View.ToNumber(0) == std::optional<double>(1.0) &&
              View.ToText(1) == std::optional<std::string>("two"),
          "a live view reads every element of its frame");
    Check(View.Path(1) == "argument 4",
          "a view names the complete path of one element");
    const Luna::ArgumentPack Owned = View.ToOwned();
    Check(Owned == Pack, "copying out of a frame is the documented retention");
    Retained = View;
  }

  Check(!Retained.IsActive() && Retained.Size() == 0 && Retained.At(0).IsNil(),
        "a view outliving its frame answers as an inert empty view");
  Check(Luna::Detail::ExpiredArgumentAccessCount() > 0,
        "an access through an expired view is recorded");
  Luna::Detail::ResetArgumentBoundaryDiagnostics();
}

} // namespace

int RunDeclaredParameterShapeTests() {
  FailureCount = 0;
  CheckShapeValidationRules();
  CheckAdapterDescribesDeclaredShapes();
  CheckCanonicalSignatureCarriesTheShape();
  CheckReflectedParametersDistinguishEveryForm();
  CheckRegistrationRefusesMalformedShapes();
  CheckVariadicFormsAndTheirLifetimes();
  return FailureCount == 0 ? 0 : 1;
}
