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
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

// Only these categories install a native target, so only they can fail with a
// null target.
[[nodiscard]] constexpr bool
RequiresNativeTarget(PlanEntryKind Category) noexcept {
  return Category == PlanEntryKind::Function ||
         Category == PlanEntryKind::DispatchTarget;
}

// Only a declaration below the root scope can fail scope ownership.
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

// Duplicate and incompatible-category detection reads Luna's canonical symbol
// model: the committed symbols of the captured generation plus every symbol the
// active transaction has planned so far.
[[nodiscard]] std::optional<ErrorDiagnostic>
CheckCollision(const RegistrationValidationRequest &Request,
               const SymbolView &Symbols, const std::string &Subject) {
  // A value Luna does not own as this symbol is reported before the canonical
  // model is consulted: a committed scope whose table a script replaced must
  // collide rather than look like a reopened duplicate.
  if (Request.VmPathHoldsUnownedValue)
    return UnownedPathDiagnostic(Subject, Request.VmPathValueKindText);

  if (const auto Existing = Symbols.Find(Request.Name)) {
    // One qualified name owns one overload set. A callable candidate whose
    // canonical signature differs from every candidate of that set groups with
    // them instead of colliding with them.
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

// Every canonical type the active transaction has already planned. The
// declaration under validation is not part of it yet, which is what lets one
// declaration be compared against the earlier ones.
[[nodiscard]] std::vector<TypeRecord>
PendingTypeDeclarations(const SymbolView &Symbols) {
  std::vector<TypeRecord> Pending;
  for (const DescriptorPlanEntry &Planned : Symbols.PendingEntries()) {
    if (Planned.TypeConversion)
      Pending.push_back(*Planned.TypeConversion);
  }
  return Pending;
}

// A canonical type is usable when the captured generation describes it or the
// same transaction is declaring it.
[[nodiscard]] bool IsDeclaredPending(const std::vector<TypeRecord> &Pending,
                                     const TypeDescriptor &Type) {
  for (const TypeRecord &Record : Pending) {
    if (Record.Descriptor == Type)
      return true;
  }
  return false;
}

// One planned type declaration is rejected before installation whenever it
// conflicts with the committed generation or with an earlier declaration of the
// same transaction.
[[nodiscard]] std::optional<ErrorDiagnostic>
CheckTypeDeclaration(const RegistrationValidationRequest &Request,
                     const TypeGeneration &Types, const SymbolView &Symbols,
                     const std::string &Subject) {
  const DescriptorPlanEntry *Entry = Request.Entry;
  if (!Entry || !Entry->TypeConversion)
    return std::nullopt;

  const TypeRecord &Candidate = *Entry->TypeConversion;
  const std::vector<TypeRecord> Pending = PendingTypeDeclarations(Symbols);
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
CheckTypeAvailability(const RegistrationValidationRequest &Request,
                      const TypeGeneration &Types, const SymbolView &Symbols,
                      const std::string &Subject) {
  const DescriptorPlanEntry *Entry = Request.Entry;
  if (!Entry)
    return std::nullopt;

  // A declaration may use the type it is declaring, and any type an earlier
  // declaration of the same transaction contributes.
  std::vector<TypeRecord> Pending = PendingTypeDeclarations(Symbols);
  if (Entry->TypeConversion)
    Pending.push_back(*Entry->TypeConversion);

  const auto IsUsable = [&Types, &Pending](const TypeDescriptor &Type,
                                           bool AllowVoid) {
    return IsAvailableCanonicalType(Types, Type, AllowVoid) ||
           IsDeclaredPending(Pending, Type);
  };

  if (const auto &Signature = Entry->Symbol.Signature) {
    // A return shape publishes one value per ordered element rather than one
    // aggregate, so what must be available is every type the shape publishes.
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

  return std::nullopt;
}

// Metadata is well formed when the canonical descriptor and the callable's own
// metadata describe the same shape and every payload the category requires is
// present.
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

    // A declared optional, defaulted, or variadic shape is validated against
    // the same canonical signature, including its immutable default metadata.
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

  // Type availability and converter conflicts are one precedence step: they
  // read the captured type generation plus the declarations the transaction has
  // planned so far, never a later generation.
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
