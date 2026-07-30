// clang-format off
#include "state/registration/validation.hpp"

#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/symbol_descriptor.hpp"
#include "state/registration/checks.hpp"
#include "state/registration/parameter_shape.hpp"
#include "state/registration/plan.hpp"
#include "state/registration/return_shape.hpp"
#include "state/transaction/capture.hpp"
#include "state/transaction/generation_set.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] constexpr bool
RequiresNativeTarget(PlanEntryKind Category) noexcept {
  return Category == PlanEntryKind::Function ||
         Category == PlanEntryKind::DispatchTarget;
}

[[nodiscard]] bool
HasParentScope(const RegistrationValidationRequest &Request) {
  return !Request.ParentQualifiedName.empty();
}

[[nodiscard]] std::optional<ErrorDiagnostic>
CheckIdentifier(const RegistrationValidationRequest &Request) {
  if (Request.Precedence == RegistrationPrecedence::FoundationRootFunction)
    return ValidateGlobalIdentifier(Request.Name);
  return ValidateCanonicalQualifiedName(Request.Name);
}

[[nodiscard]] std::optional<ErrorDiagnostic>
CheckOwnerThread(const TransactionCapture &Capture,
                 const std::string &Subject) {
  if (!Capture.IsOwnerThread())
    return ForeignThreadDiagnostic(Subject);
  return std::nullopt;
}

[[nodiscard]] std::optional<ErrorDiagnostic>
CheckLifecycle(const TransactionCapture &Capture, const std::string &Subject) {
  if (!Capture.VirtualMachineIsReady)
    return StateNotReadyDiagnostic(Subject);
  if (Capture.IsFrozen())
    return FrozenStateDiagnostic(Subject);
  return std::nullopt;
}

[[nodiscard]] std::optional<ErrorDiagnostic>
CheckScope(const RegistrationValidationRequest &Request,
           const std::string &Subject) {
  if (!HasParentScope(Request))
    return std::nullopt;
  if (!Request.ScopeIsOwned)
    return ForeignScopeDiagnostic(Subject, Request.ParentQualifiedName);
  if (!Request.ScopeIsCurrent)
    return StaleScopeDiagnostic(Subject);
  return std::nullopt;
}

[[nodiscard]] std::optional<ErrorDiagnostic>
CheckTarget(const RegistrationValidationRequest &Request,
            const std::string &Subject) {
  if (!RequiresNativeTarget(Request.Category))
    return std::nullopt;
  if (!Request.HasTarget)
    return NullCallableDiagnostic(Subject);
  if (Request.Entry && Request.Entry->Callable &&
      !Request.Entry->Callable->HasTarget())
    return NullCallableDiagnostic(Subject);
  return std::nullopt;
}

[[nodiscard]] std::optional<ErrorDiagnostic>
CheckCollision(const RegistrationValidationRequest &Request,
               const SymbolView &Symbols, const std::string &Subject) {
  if (Request.VmPathHoldsUnownedValue)
    return UnownedPathDiagnostic(Subject, Request.VmPathValueKindText);

  if (const auto Existing = Symbols.Find(Request.Name)) {
    if (Request.JoinsOverloadSet &&
        Request.Category == PlanEntryKind::Function &&
        Existing->Category == PlanEntryKind::Function)
      return std::nullopt;
    if (Existing->Category == Request.Category)
      return DuplicateNameDiagnostic(Subject);
    return IncompatibleCategoryDiagnostic(
        Subject, PlanEntryKindText(Existing->Category), Existing->IsPending);
  }

  if (Request.VmPathIsOccupied)
    return DuplicateNameDiagnostic(Subject);

  return std::nullopt;
}

[[nodiscard]] std::vector<TypeRecord>
PendingTypeDeclarations(const SymbolView &Symbols) {
  std::vector<TypeRecord> Pending;
  for (const DescriptorPlanEntry &Planned : Symbols.PendingEntries()) {
    if (Planned.TypeConversion)
      Pending.push_back(*Planned.TypeConversion);
    for (const TypeRecord &Parameter : Planned.ParameterTypeConversions)
      Pending.push_back(Parameter);
  }
  return Pending;
}

[[nodiscard]] bool IsDeclaredPending(const std::vector<TypeRecord> &Pending,
                                     const TypeDescriptor &Type) {
  for (const TypeRecord &Record : Pending) {
    if (Record.Descriptor == Type)
      return true;
  }
  return false;
}

[[nodiscard]] std::optional<ErrorDiagnostic> CheckOneTypeDeclaration(
    const TypeRecord &Candidate, const TypeGeneration &Types,
    const std::vector<TypeRecord> &Pending, const std::string &Subject) {
  const std::string TypeText = CanonicalTypeText(Candidate.Descriptor);

  switch (ClassifyTypeDeclaration(Types, Pending, Candidate)) {
  case TypeDeclarationStatus::Acceptable:
  case TypeDeclarationStatus::IdempotentDuplicate:
    return std::nullopt;
  case TypeDeclarationStatus::IncompleteRecord:
    return MalformedMetadataDiagnostic(
        Subject, "the canonical type declaration is incomplete.");
  case TypeDeclarationStatus::ConflictingConverter:
    return ConflictingConverterDiagnostic(Subject, TypeText);
  case TypeDeclarationStatus::IncompatibleDuplicate:
    return IncompatibleTypeDeclarationDiagnostic(Subject, TypeText);
  case TypeDeclarationStatus::UnavailableNestedType:
    return UnavailableTypeDiagnostic(Subject, "a nested type", TypeText);
  case TypeDeclarationStatus::DescriptorCollision:
    return TypeDescriptorCollisionDiagnostic(Subject, TypeText);
  }
  return MalformedMetadataDiagnostic(
      Subject, "the canonical type declaration could not be classified.");
}

[[nodiscard]] std::optional<ErrorDiagnostic>
CheckTypeDeclaration(const RegistrationValidationRequest &Request,
                     const TypeGeneration &Types, const SymbolView &Symbols,
                     const std::string &Subject) {
  const DescriptorPlanEntry *Entry = Request.Entry;
  if (!Entry)
    return std::nullopt;

  const std::vector<TypeRecord> Pending = PendingTypeDeclarations(Symbols);

  if (Entry->TypeConversion) {
    if (auto Diagnostic = CheckOneTypeDeclaration(*Entry->TypeConversion, Types,
                                                  Pending, Subject))
      return Diagnostic;
  }
  for (const TypeRecord &Candidate : Entry->ParameterTypeConversions) {
    if (auto Diagnostic =
            CheckOneTypeDeclaration(Candidate, Types, Pending, Subject))
      return Diagnostic;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<ErrorDiagnostic>
CheckTypeAvailability(const RegistrationValidationRequest &Request,
                      const TypeGeneration &Types, const SymbolView &Symbols,
                      const std::string &Subject) {
  const DescriptorPlanEntry *Entry = Request.Entry;
  if (!Entry)
    return std::nullopt;

  std::vector<TypeRecord> Pending = PendingTypeDeclarations(Symbols);
  if (Entry->TypeConversion)
    Pending.push_back(*Entry->TypeConversion);
  for (const TypeRecord &Candidate : Entry->ParameterTypeConversions)
    Pending.push_back(Candidate);

  const auto DelegateIsUsable = [&Types](const TypeDescriptor &Type) {
    if (!IsCanonicalDelegateType(Type))
      return false;
    const std::span<const TypeDescriptor> Children = Type.Children();
    for (std::size_t Index = 0; Index < Children.size(); ++Index) {
      if (!IsAvailableCanonicalType(Types, Children[Index], Index == 0))
        return false;
    }
    return true;
  };

  const auto IsUsable = [&Types, &Pending, &DelegateIsUsable](
                            const TypeDescriptor &Type, bool AllowVoid) {
    return IsAvailableCanonicalType(Types, Type, AllowVoid) ||
           IsDeclaredPending(Pending, Type) || DelegateIsUsable(Type);
  };

  if (const auto &Signature = Entry->Symbol.Signature) {
    const std::vector<TypeDescriptor> Published =
        PublishedReturnTypes(Signature->ReturnType);
    for (const TypeDescriptor &Returned : Published) {
      if (!IsUsable(Returned, true))
        return UnavailableTypeDiagnostic(Subject, "the return value",
                                         CanonicalTypeText(Returned));
    }

    for (std::size_t Index = 0; Index < Signature->ParameterTypes.size();
         ++Index) {
      const TypeDescriptor &Parameter = Signature->ParameterTypes[Index];
      if (!IsUsable(Parameter, false))
        return UnavailableTypeDiagnostic(
            Subject, "parameter " + std::to_string(Index + 1),
            CanonicalTypeText(Parameter));
    }

    if (Signature->ReceiverType && !IsUsable(*Signature->ReceiverType, false))
      return UnavailableTypeDiagnostic(
          Subject, "the receiver", CanonicalTypeText(*Signature->ReceiverType));
  }

  if (Entry->Symbol.AssociatedType &&
      !IsUsable(*Entry->Symbol.AssociatedType, false))
    return UnavailableTypeDiagnostic(
        Subject, "the associated type",
        CanonicalTypeText(*Entry->Symbol.AssociatedType));

  if (Request.DeclaredValueType != nullptr &&
      !IsUsable(*Request.DeclaredValueType, false))
    return UnavailableTypeDiagnostic(
        Subject, "the declared value type",
        CanonicalTypeText(*Request.DeclaredValueType));

  return std::nullopt;
}

[[nodiscard]] std::optional<ErrorDiagnostic>
CheckDescriptorCompleteness(const RegistrationValidationRequest &Request,
                            const std::string &Subject) {
  const DescriptorPlanEntry *Entry = Request.Entry;
  if (!Entry)
    return MalformedMetadataDiagnostic(
        Subject, "Luna could not describe the declaration canonically.");

  if (Entry->Category != Request.Category)
    return MalformedMetadataDiagnostic(
        Subject, "the planned category does not match the requested one.");

  if (Entry->Symbol.QualifiedName != Request.Name)
    return MalformedMetadataDiagnostic(
        Subject,
        "the planned qualified name does not match the requested one.");

  if (!Entry->IsValid())
    return MalformedMetadataDiagnostic(
        Subject, "the canonical descriptor is incomplete.");

  if (Entry->Category == PlanEntryKind::Function) {
    const auto &Signature = Entry->Symbol.Signature;
    if (!Signature)
      return MalformedMetadataDiagnostic(
          Subject, "the callable descriptor has no canonical signature.");

    const CallableMetadata &Metadata = Entry->Callable->Metadata();

    if (Metadata.HasRichParameters())
      return CheckDeclaredParameterShape(Metadata, *Signature, Subject);

    if (Metadata.ParameterTypes().size() != Signature->ParameterTypes.size())
      return MalformedMetadataDiagnostic(
          Subject, "the callable metadata and the canonical signature "
                   "disagree on the parameter count.");

    for (std::size_t Index = 0; Index < Signature->ParameterTypes.size();
         ++Index) {
      if (CanonicalValueType(Metadata.ParameterTypes()[Index]) !=
          Signature->ParameterTypes[Index])
        return MalformedMetadataDiagnostic(
            Subject, "the callable metadata and the canonical signature "
                     "disagree on parameter " +
                         std::to_string(Index + 1) + ".");
    }

    const TypeDescriptor Expected = CanonicalReturnType(Metadata.ReturnType());
    if (Expected != Signature->ReturnType)
      return MalformedMetadataDiagnostic(
          Subject, "the callable metadata and the canonical signature "
                   "disagree on the return value.");
  }

  return std::nullopt;
}

} // namespace

std::string RequestSubject(const RegistrationValidationRequest &Request) {
  if (Request.Precedence == RegistrationPrecedence::FoundationRootFunction)
    return GlobalSubject(Request.Name);
  if (!Request.SubjectKindText.empty())
    return SubjectText(Request.SubjectKindText, Request.Name);
  return SubjectText(PlanEntryKindText(Request.Category), Request.Name);
}

bool IsAvailableCanonicalType(const TypeGeneration &Types,
                              const TypeDescriptor &Type,
                              bool AllowVoid) noexcept {
  return Types.IsAvailable(Type, AllowVoid);
}

bool IsAvailableCanonicalType(const TypeDescriptor &Type,
                              bool AllowVoid) noexcept {
  const std::shared_ptr<const TypeGeneration> Types =
      TypeGeneration::Foundation();
  return Types && Types->IsAvailable(Type, AllowVoid);
}

std::optional<ErrorDiagnostic>
ValidateTransactionEntry(const TransactionCapture &Capture,
                         std::string_view Subject) {
  const std::string Text(Subject);
  if (auto Diagnostic = CheckOwnerThread(Capture, Text))
    return Diagnostic;
  return CheckLifecycle(Capture, Text);
}

std::optional<ErrorDiagnostic>
ValidateRegistration(const RegistrationValidationRequest &Request,
                     const TransactionCapture &Capture,
                     const SymbolView &Symbols) {
  const std::string Subject = RequestSubject(Request);

  if (Request.Precedence == RegistrationPrecedence::FoundationRootFunction) {
    if (auto Diagnostic = CheckIdentifier(Request))
      return Diagnostic;
    if (auto Diagnostic = CheckOwnerThread(Capture, Subject))
      return Diagnostic;
    if (auto Diagnostic = CheckLifecycle(Capture, Subject))
      return Diagnostic;
  } else {
    if (auto Diagnostic = CheckOwnerThread(Capture, Subject))
      return Diagnostic;
    if (auto Diagnostic = CheckLifecycle(Capture, Subject))
      return Diagnostic;
    if (auto Diagnostic = CheckIdentifier(Request))
      return Diagnostic;
    if (auto Diagnostic = CheckScope(Request, Subject))
      return Diagnostic;
  }

  if (auto Diagnostic = CheckTarget(Request, Subject))
    return Diagnostic;
  if (auto Diagnostic = CheckCollision(Request, Symbols, Subject))
    return Diagnostic;

  const TypeGeneration &Types = *Capture.SharedGenerations()->Types();
  if (auto Diagnostic = CheckTypeAvailability(Request, Types, Symbols, Subject))
    return Diagnostic;
  if (auto Diagnostic = CheckTypeDeclaration(Request, Types, Symbols, Subject))
    return Diagnostic;
  if (auto Diagnostic = CheckDescriptorCompleteness(Request, Subject))
    return Diagnostic;

  return std::nullopt;
}

} // namespace Luna::Detail
