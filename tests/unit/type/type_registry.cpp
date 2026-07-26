// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/value.hpp>
#include <luna/detail/callable_adapter.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/registration/plan.hpp"
#include "state/registration/validation.hpp"
#include "state/reflection/database.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/transaction/generation_set.hpp"
#include "state/transaction/preparation.hpp"
#include "state/transaction/transaction.hpp"
#include "state/type/foundation_types.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Luna::Detail::ClassifyTypeDeclaration;
using Luna::Detail::ConversionRankCategory;
using Luna::Detail::DescriptorPlanEntry;
using Luna::Detail::LuauRepresentation;
using Luna::Detail::MakeTypePlanEntry;
using Luna::Detail::PlanEntryKind;
using Luna::Detail::PreparationStatus;
using Luna::Detail::RegistrationTransaction;
using Luna::Detail::TypeDeclarationStatus;
using Luna::Detail::TypeGeneration;
using Luna::Detail::TypeRecord;
using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "canonical type registry check failed: " << Description << '\n';
}

[[nodiscard]] bool Contains(const Luna::ErrorDiagnostic *Diagnostic,
                            std::string_view Text) {
  return Diagnostic && Diagnostic->Message().find(Text) != std::string::npos;
}

// Two distinct converters for the declarations under test. They are never
// invoked here; only their identity as converters matters.
[[nodiscard]] Luna::Detail::ArgumentReadResult FirstReader(lua_State *, int) {
  return {.Status = Luna::Detail::ArgumentReadStatus::InternalFailure};
}

[[nodiscard]] Luna::Detail::ArgumentReadResult SecondReader(lua_State *, int) {
  return {.Status = Luna::Detail::ArgumentReadStatus::TypeMismatch,
          .ReceivedType = "table"};
}

[[nodiscard]] bool FirstWriter(lua_State *, const Luna::Value &) {
  return false;
}

[[nodiscard]] Luna::TypeDescriptor IntegerSequence() {
  std::vector<Luna::TypeDescriptor> Children;
  Children.push_back(Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Int32));
  return Luna::TypeDescriptor::ForStructure(Luna::TypeKind::Sequence,
                                            std::move(Children));
}

// One complete declaration of a nested type built on the foundation int32.
[[nodiscard]] TypeRecord
SequenceRecord(Luna::Detail::TypeReadFunction Read = &FirstReader,
               Luna::Detail::TypeWriteFunction Write = &FirstWriter,
               std::string PublicName = "sequence of signed 32-bit integer") {
  TypeRecord Record;
  Record.Descriptor = IntegerSequence();
  if (const auto Identity = Luna::Detail::TypeIdentityRegistry::ComputeIdentity(
          Record.Descriptor))
    Record.Identity = *Identity;
  Record.PublicName = std::move(PublicName);
  Record.Representation = LuauRepresentation::Table;
  Record.IsReadable = true;
  Record.IsWritable = true;
  Record.Rank = ConversionRankCategory::SafeBuiltIn;
  Record.Read = Read;
  Record.Write = Write;

  const std::shared_ptr<const TypeGeneration> Foundation =
      TypeGeneration::Foundation();
  if (const auto *Element = Foundation->Find(
          Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Int32)))
    Record.NestedTypes.push_back(Element->Identity);
  return Record;
}

void CheckFoundationGeneration() {
  const std::shared_ptr<const TypeGeneration> Types =
      TypeGeneration::Foundation();
  Check(Types != nullptr, "the foundation type generation exists");
  if (!Types)
    return;

  Check(Types->Size() == 5,
        "the foundation generation describes void, bool, int32, double, and "
        "string");
  Check(Types == TypeGeneration::Foundation(),
        "every State observes one shared foundation generation");

  const struct Expected final {
    Luna::FixedTypeKey Key;
    std::string_view PublicName;
    LuauRepresentation Representation;
    bool IsReadable;
  } Cases[]{
      {Luna::FixedTypeKey::Void, "void", LuauRepresentation::None, false},
      {Luna::FixedTypeKey::Boolean, "boolean", LuauRepresentation::Boolean,
       true},
      {Luna::FixedTypeKey::Int32, "signed 32-bit integer",
       LuauRepresentation::Number, true},
      {Luna::FixedTypeKey::Double, "number", LuauRepresentation::Number, true},
      {Luna::FixedTypeKey::String, "string", LuauRepresentation::String, true}};

  for (const Expected &Case : Cases) {
    const Luna::TypeDescriptor Descriptor =
        Luna::TypeDescriptor::ForFixed(Case.Key);
    const TypeRecord *Record = Types->Find(Descriptor);
    Check(Record != nullptr, "the foundation generation describes each type");
    if (!Record)
      continue;

    Check(Record->PublicName == Case.PublicName,
          "each foundation type keeps its exact public name");
    Check(Record->Representation == Case.Representation,
          "each foundation type keeps its Luau representation");
    Check(Record->IsReadable == Case.IsReadable &&
              Record->IsWritable == Case.IsReadable,
          "the foundation types are readable and writable except void");
    Check(!Record->IsNullable, "no foundation type accepts nil");
    Check(Record->Rank == ConversionRankCategory::Exact,
          "a foundation conversion is an exact conversion");
    Check(Record->NestedTypes.empty(), "a foundation leaf has no nested type");
    Check(Record->IsComplete(), "every foundation declaration is complete");

    const auto Identity =
        Luna::Detail::TypeIdentityRegistry::ComputeIdentity(Descriptor);
    Check(Identity.has_value() && Record->Identity == *Identity,
          "a type identity follows its canonical descriptor");
    Check(Types->Find(Record->Identity) == Record,
          "a type is found by its identity");
  }

  const TypeRecord *Text =
      Types->Find(Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::String));
  Check(Text != nullptr && Text->MaximumByteCount.has_value() &&
            *Text->MaximumByteCount == 1'048'576,
        "the string type keeps the inherited 1,048,576-byte policy");

  const TypeRecord *Void =
      Types->Find(Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Void));
  Check(Void != nullptr && Void->IsVoid() && !Void->Read && !Void->Write &&
            !Void->ValueRepresentation,
        "void carries no value and no converter");

  // Value-kind lookup answers with the same records the descriptors do.
  Check(Types->Find(Luna::ValueKind::Integer) ==
            Types->Find(
                Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Int32)),
        "a foundation value kind resolves its canonical type");
  Check(Types->PublicNameOf(Luna::ValueKind::Integer) ==
            "signed 32-bit integer",
        "diagnostics read the integer name from the registry");
  Check(Types->PublicNameOf(Luna::ValueKind::Number) == "number" &&
            Types->PublicNameOf(Luna::ValueKind::Boolean) == "boolean" &&
            Types->PublicNameOf(Luna::ValueKind::String) == "string",
        "diagnostics read every foundation name from the registry");

  // Availability is a registry question, with direction and void handled
  // explicitly.
  Check(Types->IsAvailableForRead(
            Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Int32)),
        "int32 is available for reading");
  Check(!Types->IsAvailableForRead(
            Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Void)),
        "void is never available as a parameter");
  Check(Types->IsAvailable(
            Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Void), true),
        "void is available as a return shape");
  Check(!Types->IsAvailable(
            Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Float), false),
        "an undeclared type is unavailable");
  Check(!Types->IsAvailable(IntegerSequence(), false),
        "an undeclared structural type is unavailable");
  Check(!Types->Contains(Luna::TypeDescriptor::Unsupported()),
        "an unsupported descriptor is never described");
  Check(Types->At(Types->Size()) == nullptr,
        "an out-of-range type index has no record");
}

void CheckCanonicalOrderingIsDeclarationIndependent() {
  std::vector<TypeRecord> Forward = Luna::Detail::FoundationTypeRecords();
  std::vector<TypeRecord> Reversed = Luna::Detail::FoundationTypeRecords();
  std::reverse(Reversed.begin(), Reversed.end());

  TypeDeclarationStatus FirstStatus = TypeDeclarationStatus::Acceptable;
  TypeDeclarationStatus SecondStatus = TypeDeclarationStatus::Acceptable;
  const std::shared_ptr<const TypeGeneration> First =
      TypeGeneration::Build(std::move(Forward), FirstStatus);
  const std::shared_ptr<const TypeGeneration> Second =
      TypeGeneration::Build(std::move(Reversed), SecondStatus);
  Check(First != nullptr && Second != nullptr,
        "equivalent declaration sets both build");
  if (!First || !Second)
    return;

  Check(First->Size() == Second->Size(),
        "declaration order never changes the generation size");
  bool SameOrder = true;
  for (std::size_t Index = 0; Index < First->Size(); ++Index) {
    const TypeRecord *Left = First->At(Index);
    const TypeRecord *Right = Second->At(Index);
    SameOrder = SameOrder && Left && Right &&
                Left->Identity == Right->Identity &&
                Left->Descriptor == Right->Descriptor;
  }
  Check(SameOrder, "canonical order is independent of declaration order");
}

void CheckDeclarationClassification() {
  const std::shared_ptr<const TypeGeneration> Types =
      TypeGeneration::Foundation();
  if (!Types)
    return;

  const TypeRecord Sequence = SequenceRecord();
  Check(ClassifyTypeDeclaration(*Types, {}, Sequence) ==
            TypeDeclarationStatus::Acceptable,
        "a new complete declaration is acceptable");

  const TypeRecord Same = SequenceRecord();
  Check(ClassifyTypeDeclaration(*Types, std::span(&Same, 1), Sequence) ==
            TypeDeclarationStatus::IdempotentDuplicate,
        "an exact repeat of a declaration adds nothing");

  const TypeRecord OtherConverter = SequenceRecord(&SecondReader);
  Check(ClassifyTypeDeclaration(*Types, std::span(&Same, 1), OtherConverter) ==
            TypeDeclarationStatus::ConflictingConverter,
        "a second converter for one type conflicts");

  const TypeRecord OtherMetadata =
      SequenceRecord(&FirstReader, &FirstWriter, "list of integers");
  Check(ClassifyTypeDeclaration(*Types, std::span(&Same, 1), OtherMetadata) ==
            TypeDeclarationStatus::IncompatibleDuplicate,
        "an incompatible duplicate declaration is rejected");

  // A second identity for one canonical descriptor, and one identity shared by
  // two unequal descriptors, are both collisions.
  TypeRecord Renamed = SequenceRecord();
  Renamed.Identity = *Luna::Detail::TypeIdentityRegistry::ComputeIdentity(
      Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Float));
  Check(ClassifyTypeDeclaration(*Types, std::span(&Same, 1), Renamed) ==
            TypeDeclarationStatus::DescriptorCollision,
        "one canonical descriptor never takes a second identity");

  TypeRecord Colliding = SequenceRecord();
  Colliding.Descriptor =
      Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Float);
  Colliding.PublicName = "float";
  Check(ClassifyTypeDeclaration(*Types, std::span(&Same, 1), Colliding) ==
            TypeDeclarationStatus::DescriptorCollision,
        "two unequal descriptors never share one identity");

  TypeRecord MissingNested = SequenceRecord();
  MissingNested.NestedTypes.clear();
  MissingNested.NestedTypes.push_back(
      *Luna::Detail::TypeIdentityRegistry::ComputeIdentity(
          Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Float)));
  Check(ClassifyTypeDeclaration(*Types, {}, MissingNested) ==
            TypeDeclarationStatus::UnavailableNestedType,
        "a declaration cannot nest an unavailable type");

  TypeRecord Incomplete = SequenceRecord();
  Incomplete.Read = nullptr;
  Check(ClassifyTypeDeclaration(*Types, {}, Incomplete) ==
            TypeDeclarationStatus::IncompleteRecord,
        "a readable declaration without a reader is incomplete");

  TypeRecord Unnamed = SequenceRecord();
  Unnamed.PublicName.clear();
  Check(!Unnamed.IsComplete(), "an unnamed declaration is incomplete");
}

void CheckDerivedGenerations() {
  const std::shared_ptr<const TypeGeneration> Foundation =
      TypeGeneration::Foundation();
  if (!Foundation)
    return;

  TypeDeclarationStatus Status = TypeDeclarationStatus::Acceptable;
  std::vector<TypeRecord> Added;
  Added.push_back(SequenceRecord());
  const std::shared_ptr<const TypeGeneration> Extended =
      TypeGeneration::Derive(*Foundation, std::move(Added), Status);
  Check(Extended != nullptr && Status == TypeDeclarationStatus::Acceptable,
        "an acceptable declaration derives the next generation");
  Check(Extended != nullptr && Extended->Size() == Foundation->Size() + 1,
        "the next generation keeps every prior type");
  Check(Extended != nullptr &&
            Extended->Generation() == Foundation->Generation() + 1,
        "deriving advances the type generation number");
  Check(Extended != nullptr && Extended->IsAvailableForRead(IntegerSequence()),
        "the derived generation makes the new type available");
  Check(Foundation->Size() == 5,
        "deriving leaves the prior generation untouched");

  std::vector<TypeRecord> Conflicting;
  Conflicting.push_back(SequenceRecord());
  Conflicting.push_back(SequenceRecord(&SecondReader));
  TypeDeclarationStatus ConflictStatus = TypeDeclarationStatus::Acceptable;
  const std::shared_ptr<const TypeGeneration> Rejected = TypeGeneration::Derive(
      *Foundation, std::move(Conflicting), ConflictStatus);
  Check(Rejected == nullptr &&
            ConflictStatus == TypeDeclarationStatus::ConflictingConverter,
        "a conflicting converter rejects the whole derivation");
  Check(Foundation->Size() == 5,
        "a rejected derivation leaves the prior generation untouched");
}

void CheckTransactionalRejection() {
  Luna::State Owner;
  Check(Owner.IsReady(), "a new State is ready");

  const std::shared_ptr<const Luna::Detail::GenerationSet> Committed =
      Hooks::GenerationsOf(Owner);
  Check(Committed != nullptr && Committed->Types() != nullptr &&
            Committed->Types()->Size() == 5,
        "a State starts on the migrated foundation type generation");

  const auto Capture = Hooks::CaptureTransactionEntryOf(Owner);
  Check(Capture.has_value(), "a ready State captures a transaction entry");
  if (!Capture)
    return;

  Luna::Detail::ReflectionDatabase *Reflection =
      Hooks::ReflectionDatabaseOf(Owner);
  Check(Reflection != nullptr, "a State owns one reflection database");
  if (!Reflection)
    return;

  // One accepted declaration validates, prepares, and produces a candidate
  // generation that nothing observes yet.
  {
    RegistrationTransaction Transaction(*Capture);
    DescriptorPlanEntry Entry =
        MakeTypePlanEntry("Studio.IntegerList", SequenceRecord());

    Luna::Detail::RegistrationValidationRequest Request;
    Request.Precedence = Luna::Detail::RegistrationPrecedence::GeneralOperation;
    Request.Name = "Studio.IntegerList";
    Request.Entry = &Entry;
    Request.Category = PlanEntryKind::Type;
    const auto Diagnostic = Luna::Detail::ValidateRegistration(
        Request, Transaction.Entry(), Transaction.Symbols());
    Check(!Diagnostic.has_value(),
          "a new complete type declaration passes validation");

    static_cast<void>(Transaction.Append(std::move(Entry)));
    Luna::Detail::PreparedGenerations Prepared;
    const PreparationStatus Status =
        Luna::Detail::PrepareGenerations(Transaction, *Reflection, Prepared);
    Check(Status == PreparationStatus::Prepared && Prepared.IsPrepared(),
          "a type declaration prepares a candidate generation");
    Check(Prepared.TypesAdvance,
          "a plan that declares a type advances the type generation");
    Check(Prepared.Candidate != nullptr &&
              Prepared.Candidate->Types() != nullptr &&
              Prepared.Candidate->Types()->Size() == 6,
          "the candidate generation set carries the candidate types");
    Check(Hooks::GenerationsOf(Owner)->Types()->Size() == 5,
          "preparation publishes nothing");
  }

  // A conflicting converter is rejected at validation, in the same precedence
  // step as type availability.
  {
    RegistrationTransaction Transaction(*Capture);
    static_cast<void>(Transaction.Append(
        MakeTypePlanEntry("Studio.IntegerList", SequenceRecord())));

    DescriptorPlanEntry Conflict =
        MakeTypePlanEntry("Studio.OtherList", SequenceRecord(&SecondReader));
    Luna::Detail::RegistrationValidationRequest Request;
    Request.Precedence = Luna::Detail::RegistrationPrecedence::GeneralOperation;
    Request.Name = "Studio.OtherList";
    Request.Entry = &Conflict;
    Request.Category = PlanEntryKind::Type;
    const auto Diagnostic = Luna::Detail::ValidateRegistration(
        Request, Transaction.Entry(), Transaction.Symbols());
    Check(Diagnostic.has_value() &&
              Contains(Diagnostic ? &*Diagnostic : nullptr,
                       "already declared with a different converter"),
          "a conflicting converter is rejected deterministically");
    Check(Diagnostic.has_value() &&
              Diagnostic->Category() == Luna::ErrorCategory::Internal,
          "a converter conflict is an internal refusal");
  }

  // An incompatible duplicate declaration and a descriptor collision are
  // rejected the same way.
  {
    RegistrationTransaction Transaction(*Capture);
    static_cast<void>(Transaction.Append(
        MakeTypePlanEntry("Studio.IntegerList", SequenceRecord())));

    DescriptorPlanEntry Incompatible = MakeTypePlanEntry(
        "Studio.RenamedList",
        SequenceRecord(&FirstReader, &FirstWriter, "list of integers"));
    Luna::Detail::RegistrationValidationRequest Request;
    Request.Precedence = Luna::Detail::RegistrationPrecedence::GeneralOperation;
    Request.Name = "Studio.RenamedList";
    Request.Entry = &Incompatible;
    Request.Category = PlanEntryKind::Type;
    const auto Diagnostic = Luna::Detail::ValidateRegistration(
        Request, Transaction.Entry(), Transaction.Symbols());
    Check(Diagnostic.has_value() &&
              Contains(Diagnostic ? &*Diagnostic : nullptr,
                       "already declared with incompatible metadata"),
          "an incompatible duplicate declaration is rejected");

    TypeRecord Colliding = SequenceRecord();
    Colliding.Descriptor =
        Luna::TypeDescriptor::ForFixed(Luna::FixedTypeKey::Float);
    Colliding.PublicName = "float";
    DescriptorPlanEntry Collision =
        MakeTypePlanEntry("Studio.CollidingType", std::move(Colliding));
    Luna::Detail::RegistrationValidationRequest CollisionRequest;
    CollisionRequest.Precedence =
        Luna::Detail::RegistrationPrecedence::GeneralOperation;
    CollisionRequest.Name = "Studio.CollidingType";
    CollisionRequest.Entry = &Collision;
    CollisionRequest.Category = PlanEntryKind::Type;
    const auto CollisionDiagnostic = Luna::Detail::ValidateRegistration(
        CollisionRequest, Transaction.Entry(), Transaction.Symbols());
    Check(CollisionDiagnostic.has_value() &&
              Contains(CollisionDiagnostic ? &*CollisionDiagnostic : nullptr,
                       "shares its type identity"),
          "a canonical-descriptor collision is rejected");
  }

  // Preparation is the transactional backstop: a plan that reaches it with a
  // conflict is rejected before installation, and nothing is published.
  {
    RegistrationTransaction Transaction(*Capture);
    static_cast<void>(Transaction.Append(
        MakeTypePlanEntry("Studio.IntegerList", SequenceRecord())));
    static_cast<void>(Transaction.Append(
        MakeTypePlanEntry("Studio.OtherList", SequenceRecord(&SecondReader))));

    Luna::Detail::PreparedGenerations Prepared;
    const PreparationStatus Status =
        Luna::Detail::PrepareGenerations(Transaction, *Reflection, Prepared);
    Check(Status == PreparationStatus::InconsistentTypes,
          "preparation rejects a conflicting type plan");
    Check(Prepared.TypeStatus == TypeDeclarationStatus::ConflictingConverter,
          "the rejection names the conflicting converter");
    Check(!Prepared.IsPrepared(), "a rejected plan prepares nothing");
    Check(Hooks::GenerationsOf(Owner)->Types()->Size() == 5,
          "a rejected plan leaves the committed type generation unchanged");
  }

  // The State remains usable, and an ordinary registration still publishes a
  // generation set carrying the foundation types.
  const auto Registration = Owner.Bindings().Register(
      "Add", +[](int Left, int Right) { return Left + Right; });
  Check(Registration.IsSuccess(),
        "the State still registers after a rejected type plan");
  const std::shared_ptr<const Luna::Detail::GenerationSet> Published =
      Hooks::GenerationsOf(Owner);
  Check(Published != nullptr && Published->Types() != nullptr &&
            Published->Types() == TypeGeneration::Foundation(),
        "a plan that declares no type keeps the type generation it captured");
  const auto Execution = Owner.Execute("return Add(2, 3)");
  Check(Execution.IsSuccess(),
        "invocation still converts through the captured type generation");
}

void CheckInvocationCapturesOneGeneration() {
  Luna::State Owner;
  Check(Owner.Bindings()
            .Register(
                "Describe",
                +[](std::string Text) { return static_cast<int>(Text.size()); })
            .IsSuccess(),
        "a string callable registers");

  const auto Valid = Owner.Execute("return Describe('abcd')");
  Check(Valid.IsSuccess(), "a valid argument converts through the registry");

  const auto Mismatch = Owner.Execute("return Describe(7)");
  Check(!Mismatch.IsSuccess() && Mismatch.Diagnostic() != nullptr &&
            Contains(Mismatch.Diagnostic(),
                     "argument 1 expected string but received number"),
        "the registry supplies the exact foundation type wording");

  Check(Owner.Bindings()
            .Register(
                "Count", +[](int Value) { return Value; })
            .IsSuccess(),
        "an integer callable registers");
  const auto Fractional = Owner.Execute("return Count(3.5)");
  Check(!Fractional.IsSuccess() &&
            Contains(Fractional.Diagnostic(),
                     "expected an integral value but received 3.5"),
        "integer classification is unchanged behind the registry");

  const auto Recovered = Owner.Execute("return Count(41)");
  Check(Recovered.IsSuccess(),
        "the State recovers after a conversion rejection");
}

} // namespace

int RunCanonicalTypeRegistryTests() {
  FailureCount = 0;
  CheckFoundationGeneration();
  CheckCanonicalOrderingIsDeclarationIndependent();
  CheckDeclarationClassification();
  CheckDerivedGenerations();
  CheckTransactionalRejection();
  CheckInvocationCapturesOneGeneration();
  return FailureCount == 0 ? 0 : 1;
}
