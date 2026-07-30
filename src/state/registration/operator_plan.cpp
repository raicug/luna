// clang-format off
#include "state/registration/operator_plan.hpp"

#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/class_operator.hpp>
#include <luna/binding/parameter_descriptor.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/reflection/ids.hpp>

#include "state/registration/checks.hpp"
#include "state/registration/scope_plan.hpp"
#include "state/userdata/class_operators.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] std::string OperatorSubject(const StagedOperator &Declaration) {
  const std::string Named =
      std::string(ParentQualifiedName(Declaration.QualifiedName)) + "." +
      std::string(ClassOperatorText(Declaration.Selected));
  return SubjectText(SymbolKindText(SymbolKind::Operator), Named);
}

} // namespace

StagedOperator *FindStagedOperator(std::vector<StagedOperator> &Operators,
                                   ClassOperator Selected) noexcept {
  for (StagedOperator &Staged : Operators) {
    if (Staged.Selected == Selected)
      return &Staged;
  }
  return nullptr;
}

StagedOperator *FindStagedOperator(std::vector<StagedOperator> &Operators,
                                   std::string_view Segment) noexcept {
  for (StagedOperator &Staged : Operators) {
    if (Staged.Segment == Segment)
      return &Staged;
  }
  return nullptr;
}

std::optional<ErrorDiagnostic>
ValidateStagedOperator(const StagedOperator &Declaration) {
  const std::string Subject = OperatorSubject(Declaration);

  if (!Declaration.Refusal.empty())
    return MalformedMetadataDiagnostic(Subject, Declaration.Refusal);
  if (!Declaration.HasTarget())
    return NullCallableDiagnostic(Subject);

  const ClassOperatorDescriptor *Described =
      FindClassOperator(Declaration.Selected);
  if (Described == nullptr)
    return MalformedMetadataDiagnostic(
        Subject, "this operator is not one Luna supports on a class.");

  const CallableMetadata &Metadata = Declaration.Callable->Metadata();
  if (Metadata.ReturnType().IsAsynchronous())
    return MalformedMetadataDiagnostic(
        Subject, "an operator publishes its value inside the expression that "
                 "invoked it, so it cannot deliver that value later.");
  if (!Declaration.DeclaresReceiver || Metadata.Receiver() == nullptr)
    return MalformedMetadataDiagnostic(
        Subject, "an operator operates on one value of its class, so it "
                 "declares that value as its receiver.");

  if (Described->ForwardsEveryArgument)
    return std::nullopt;

  const std::span<const ParameterDescriptor> Declared = Metadata.Parameters();
  if (Declared.size() != Described->OperandCount)
    return MalformedMetadataDiagnostic(
        Subject, "this operator is supplied " +
                     std::to_string(Described->OperandCount) +
                     " operand(s) beyond its receiver, but the declaration "
                     "takes " +
                     std::to_string(Declared.size()) + ".");
  for (const ParameterDescriptor &Parameter : Declared) {
    if (Parameter.Form() != ParameterForm::Required &&
        Parameter.Form() != ParameterForm::Converted)
      return MalformedMetadataDiagnostic(
          Subject, "every operand of this operator is always supplied, so it "
                   "cannot be declared optional, defaulted, or variadic.");
  }

  const ReturnDisposition Returns = Metadata.ReturnType().Disposition();
  if (Declaration.Selected == ClassOperator::Assign) {
    if (Returns != ReturnDisposition::Void &&
        Returns != ReturnDisposition::Suppress)
      return MalformedMetadataDiagnostic(
          Subject, "an assignment operator publishes no value, so its target "
                   "must return void.");
    return std::nullopt;
  }

  if (Declaration.Selected != ClassOperator::Call &&
      (Returns == ReturnDisposition::Void ||
       Returns == ReturnDisposition::Suppress ||
       Returns == ReturnDisposition::Pack))
    return MalformedMetadataDiagnostic(
        Subject, "this operator must publish exactly one value.");
  return std::nullopt;
}

} // namespace Luna::Detail
