// clang-format off
#include "state/registration/parameter_shape.hpp"

#include <luna/binding/parameter_descriptor.hpp>
#include <luna/binding/value.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/registration/checks.hpp"
#include "state/registration/return_shape.hpp"
#include "state/type/structural_types.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/type_record.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] TypeDescriptor
DeclaredParameterType(const ParameterDescriptor &Parameter) {
  if (const ValueKind *Kind = Parameter.Kind())
    return CanonicalValueType(*Kind);
  if (const DelegateShape *Declared = Parameter.DelegateSignature())
    return CanonicalDelegateType(*Declared);
  if (const StableTypeKey *Key = Parameter.ConvertedKey())
    return TypeDescriptor::ForConverted(*Key);
  if (Parameter.IsInstance()) {
    // An instance operand is the registered class itself, so it is the very
    // canonical type a receiver of that class already carries.
    if (const StableTypeKey *Key = Parameter.InstanceKey())
      return TypeDescriptor::ForClass(*Key);
    return TypeDescriptor::Unsupported();
  }
  return TypeDescriptor::ForFixed(FixedTypeKey::Value);
}

[[nodiscard]] ParameterDisposition
DispositionOf(const ParameterDescriptor &Parameter) {
  switch (Parameter.Form()) {
  case ParameterForm::Required:
    return ParameterDisposition::Required;
  case ParameterForm::Optional:
    return ParameterDisposition::Optional;
  case ParameterForm::Defaulted:
    return ParameterDisposition::Defaulted;
  case ParameterForm::Variadic:
    return ParameterDisposition::Variadic;
  case ParameterForm::Delegate:
    // A subscribed handler is always supplied, so it is reflected as
    // required and its call shape lives in its canonical callable type.
    return ParameterDisposition::Required;
  case ParameterForm::Converted:
    // A converted operand is always supplied, exactly like a delegate
    // parameter; its call shape lives in its canonical converted type.
    return ParameterDisposition::Required;
  case ParameterForm::Instance:
    // An instance operand is likewise always supplied, and its identity lives
    // in its canonical class type.
    return ParameterDisposition::Required;
  }
  return ParameterDisposition::Required;
}

[[nodiscard]] std::string DefaultText(const Value &Default) {
  if (const bool *Flag = std::get_if<bool>(&Default))
    return *Flag ? "true" : "false";
  if (const int *Integer = std::get_if<int>(&Default))
    return std::to_string(*Integer);
  if (const double *Number = std::get_if<double>(&Default))
    return FormatConversionNumber(*Number);
  if (const std::string *Text = std::get_if<std::string>(&Default))
    return "\"" + *Text + "\"";
  return std::string();
}

[[nodiscard]] std::string ShapeReason(const ParameterShapeIssue &Issue) {
  const std::string Position = std::to_string(Issue.Position);
  switch (Issue.Status) {
  case ParameterShapeStatus::RequiredAfterRelaxed:
    return "required parameter " + Position +
           " follows an optional or defaulted parameter.";
  case ParameterShapeStatus::VariadicNotFinal:
    return "variadic parameter " + Position +
           " is not the final parameter; a callable has at most one variadic "
           "parameter and it is final.";
  case ParameterShapeStatus::MissingValueKind:
    return "parameter " + Position + " names no canonical Luna type.";
  case ParameterShapeStatus::MisplacedDefault:
    return "parameter " + Position +
           " and its default metadata disagree on whether it is defaulted.";
  case ParameterShapeStatus::DefaultTypeMismatch:
    return "the default of parameter " + Position +
           " is not a value of that parameter's declared type.";
  case ParameterShapeStatus::MalformedDelegate:
    return "parameter " + Position +
           " declares a subscribed handler without a canonical delegate call "
           "shape.";
  case ParameterShapeStatus::MalformedConverted:
    return "parameter " + Position +
           " declares a converted operand without a canonical converted "
           "type.";
  case ParameterShapeStatus::UnregisteredInstanceClass:
    return "parameter " + Position +
           " declares an instance of a class that was never registered with "
           "RegisterClass; register the class before declaring a member that "
           "takes one of its instances.";
  case ParameterShapeStatus::Valid:
    break;
  }
  return "the declared parameter shape could not be classified.";
}

} // namespace

CallableSignatureDescriptor
CanonicalDeclaredSignature(const CallableMetadata &Metadata) {
  CallableSignatureDescriptor Signature;

  const std::span<const ParameterDescriptor> Parameters = Metadata.Parameters();
  for (const ParameterDescriptor &Parameter : Parameters) {
    if (Parameter.IsVariadic()) {
      Signature.IsVariadic = true;
      continue;
    }
    Signature.ParameterTypes.push_back(DeclaredParameterType(Parameter));
    if (!Parameter.IsOmittable())
      ++Signature.RequiredParameterCount;
  }

  Signature.ReturnType = CanonicalReturnType(Metadata.ReturnType());
  return WithCanonicalReceiver(Metadata, std::move(Signature));
}

CallableSignatureDescriptor
WithCanonicalReceiver(const CallableMetadata &Metadata,
                      CallableSignatureDescriptor Signature) {
  if (const ReceiverMetadata *Receiver = Metadata.Receiver()) {
    Signature.ReceiverType = TypeDescriptor::ForClass(Receiver->Class());
    Signature.ReceiverIsConst = Receiver->IsConst();
  }
  return Signature;
}

std::optional<ErrorDiagnostic>
CheckDeclaredParameterShape(const CallableMetadata &Metadata,
                            const CallableSignatureDescriptor &Signature,
                            std::string_view Subject) {
  const std::span<const ParameterDescriptor> Parameters = Metadata.Parameters();

  const ParameterShapeIssue Issue = ValidateParameterShape(Parameters);
  if (!Issue.IsValid())
    return MalformedMetadataDiagnostic(Subject, ShapeReason(Issue));

  const CallableSignatureDescriptor Expected =
      CanonicalDeclaredSignature(Metadata);
  if (Expected.ParameterTypes.size() != Signature.ParameterTypes.size())
    return MalformedMetadataDiagnostic(
        Subject, "the callable metadata and the canonical signature disagree "
                 "on the parameter count.");

  for (std::size_t Index = 0; Index < Expected.ParameterTypes.size(); ++Index) {
    if (Expected.ParameterTypes[Index] != Signature.ParameterTypes[Index])
      return MalformedMetadataDiagnostic(
          Subject, "the callable metadata and the canonical signature disagree "
                   "on parameter " +
                       std::to_string(Index + 1) + ".");
  }

  if (Expected.RequiredParameterCount != Signature.RequiredParameterCount ||
      Expected.IsVariadic != Signature.IsVariadic)
    return MalformedMetadataDiagnostic(
        Subject, "the callable metadata and the canonical signature disagree "
                 "on the declared call shape.");

  if (Expected.ReturnType != Signature.ReturnType)
    return MalformedMetadataDiagnostic(
        Subject, "the callable metadata and the canonical signature disagree "
                 "on the return value.");
  return std::nullopt;
}

std::vector<ReflectionReturnFields>
MakeReflectedReturns(const CallableMetadata &Metadata) {
  return MakeReflectedReturnFields(Metadata.ReturnType());
}

ReturnShape ReflectedReturnShape(const CallableMetadata &Metadata) {
  return ReflectedReturnShapeOf(Metadata.ReturnType());
}

std::string
CanonicalSignatureText(const CallableSignatureDescriptor &Signature) {
  std::string Text = CanonicalTypeText(Signature.ReturnType);

  if (Signature.ReceiverType) {
    Text += "[";
    Text += CanonicalTypeText(*Signature.ReceiverType);
    if (Signature.ReceiverIsConst)
      Text += " const";
    Text += "]";
  }
  Text += "(";
  for (std::size_t Index = 0; Index < Signature.ParameterTypes.size();
       ++Index) {
    if (Index != 0)
      Text += ",";
    Text += CanonicalTypeText(Signature.ParameterTypes[Index]);

    if (Index >= Signature.RequiredParameterCount)
      Text += "?";
  }
  if (Signature.IsVariadic) {
    if (!Signature.ParameterTypes.empty())
      Text += ",";
    Text += "...";
  }
  Text += ")";
  return Text;
}

std::vector<ReflectionParameterFields>
MakeReflectedParameters(const CallableMetadata &Metadata) {
  std::vector<ReflectionParameterFields> Reflected;
  const std::span<const ParameterDescriptor> Parameters = Metadata.Parameters();
  Reflected.reserve(Parameters.size());

  for (std::size_t Index = 0; Index < Parameters.size(); ++Index) {
    const ParameterDescriptor &Parameter = Parameters[Index];
    ReflectionParameterFields Fields;
    Fields.Name = "Argument" + std::to_string(Index + 1);
    Fields.Descriptor = DeclaredParameterType(Parameter);
    if (const auto Identity =
            TypeIdentityRegistry::ComputeIdentity(Fields.Descriptor))
      Fields.Type = *Identity;
    Fields.Disposition = DispositionOf(Parameter);
    if (const Value *Default = Parameter.Default()) {
      Fields.HasDefault = true;
      Fields.DefaultText = DefaultText(*Default);
    }
    Reflected.push_back(std::move(Fields));
  }
  return Reflected;
}

std::vector<TypeRecord>
MakeParameterTypeConversions(const CallableMetadata &Metadata) {
  std::vector<TypeRecord> Converted;
  for (const ParameterDescriptor &Parameter : Metadata.Parameters()) {
    const StableTypeKey *Key = Parameter.ConvertedKey();
    if (!Key)
      continue;
    Converted.push_back(DeclareConvertedTypeRecord(
        *Key, CanonicalTypeText(TypeDescriptor::ForConverted(*Key))));
  }
  return Converted;
}

} // namespace Luna::Detail
