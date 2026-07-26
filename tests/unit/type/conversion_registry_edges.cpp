// Focused coverage of the conversion registry edges the type, boundary, and
// structural suites do not already own: the ordered exact/safe/user rank
// categories, nullability of every declared type, deterministic refusal of
// unsupported and unavailable types, empty aggregates, enumeration and class
// identity, and the one real resource bound a publication has to reserve.

// clang-format off
#include <luna/binding/conversion.hpp>
#include <luna/binding/value.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/type/conversion_frame.hpp"
#include "state/type/structural_types.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/testing/structural_test_hooks.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Luna::FixedTypeKey;
using Luna::StableTypeKey;
using Luna::TypeDescriptor;
using Luna::TypeKind;
using Luna::Value;
using Luna::Detail::ConversionRankCategory;
using Luna::Detail::ConversionRankCategoryText;
using Luna::Detail::ConversionSubject;
using Luna::Detail::ConversionSubjectKind;
using Luna::Detail::DescribeConversionFailure;
using Luna::Detail::IsInternalStructuredFailure;
using Luna::Detail::StructuralDeclarationStatus;
using Luna::Detail::StructuredFailure;
using Luna::Detail::StructuredValue;
using Luna::Detail::TypeGeneration;
using Luna::Detail::TypeRecord;
using Frame = Luna::Detail::ConversionFrame;
using Hooks = Luna::Detail::StructuralConversionTestHooks;
using ScriptValue = Luna::Detail::ScriptValue;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "conversion registry edge check failed: " << Description << '\n';
}

[[nodiscard]] TypeDescriptor Fixed(FixedTypeKey Key) {
  return TypeDescriptor::ForFixed(Key);
}

[[nodiscard]] std::string
ReturnMessage(const Luna::Detail::StructuredDiagnostic &Diagnostic,
              std::size_t Position = 1) {
  const ConversionSubject Subject{.Kind = ConversionSubjectKind::Callable,
                                  .Name = "Studio.Publish"};
  return DescribeConversionFailure(
      Subject, Luna::Detail::ConversionDirection::Return, Position, Diagnostic);
}

[[nodiscard]] std::string
ArgumentMessage(const Luna::Detail::StructuredDiagnostic &Diagnostic,
                std::size_t Position = 1) {
  const ConversionSubject Subject{.Kind = ConversionSubjectKind::Callable,
                                  .Name = "Studio.Apply"};
  return DescribeConversionFailure(Subject,
                                   Luna::Detail::ConversionDirection::Argument,
                                   Position, Diagnostic);
}

// The registry's rank category and the public rank a converter author reports
// are one ordered vocabulary, so the private and public spellings must agree.
[[nodiscard]] Luna::ConversionRank PublicRankOf(ConversionRankCategory Rank) {
  switch (Rank) {
  case ConversionRankCategory::Exact:
    return Luna::ConversionRank::Exact;
  case ConversionRankCategory::SafeBuiltIn:
    return Luna::ConversionRank::SafeBuiltIn;
  case ConversionRankCategory::User:
    return Luna::ConversionRank::User;
  }
  return Luna::ConversionRank::User;
}

// One consumer type per rank category. Nothing but the reported rank differs,
// so a rank is observably a converter's own declaration rather than a score
// Luna derives.
struct ExactHandle final {
  double Value = 0.0;
};

struct SafeHandle final {
  double Value = 0.0;
};

struct UserHandle final {
  double Value = 0.0;
};

} // namespace

namespace Luna {

template <> class TypeConverter<ExactHandle> {
public:
  [[nodiscard]] ConversionProbe Probe(ValueView Source,
                                      const ConversionContext &Context) const {
    if (!Source.ToNumber())
      return RejectedProbe(Context.Describe("expected a number"));
    return ViableProbe(ConversionRank::Exact);
  }

  [[nodiscard]] ConversionResult<ExactHandle>
  Read(ValueView Source, ConversionContext &Context) const {
    ConversionResult<ExactHandle> Result;
    const std::optional<double> Number = Source.ToNumber();
    if (!Number) {
      Result.Status = ConversionStatus::TypeMismatch;
      Result.Diagnostic = Context.Describe("expected a number");
      return Result;
    }
    Result.Status = ConversionStatus::Success;
    Result.ConvertedValue = ExactHandle{*Number};
    return Result;
  }

  [[nodiscard]] WriteResult Write(const ExactHandle &Source,
                                  ConversionContext &Context) const {
    const OwnedValue Published = OwnedValue::Number(Source.Value);
    if (const WriteResult Reserved =
            Context.Reserve(Published.RequiredReservation());
        !Reserved.IsSuccess())
      return Reserved;
    return Context.Publish(Published);
  }
};

template <> class TypeConverter<SafeHandle> {
public:
  [[nodiscard]] ConversionProbe Probe(ValueView Source,
                                      const ConversionContext &Context) const {
    if (!Source.ToNumber())
      return RejectedProbe(Context.Describe("expected a number"));
    return ViableProbe(ConversionRank::SafeBuiltIn);
  }

  [[nodiscard]] ConversionResult<SafeHandle>
  Read(ValueView Source, ConversionContext &Context) const {
    ConversionResult<SafeHandle> Result;
    const std::optional<double> Number = Source.ToNumber();
    if (!Number) {
      Result.Status = ConversionStatus::TypeMismatch;
      Result.Diagnostic = Context.Describe("expected a number");
      return Result;
    }
    Result.Status = ConversionStatus::Success;
    Result.ConvertedValue = SafeHandle{*Number};
    return Result;
  }

  [[nodiscard]] WriteResult Write(const SafeHandle &Source,
                                  ConversionContext &Context) const {
    const OwnedValue Published = OwnedValue::Number(Source.Value);
    if (const WriteResult Reserved =
            Context.Reserve(Published.RequiredReservation());
        !Reserved.IsSuccess())
      return Reserved;
    return Context.Publish(Published);
  }
};

template <> class TypeConverter<UserHandle> {
public:
  [[nodiscard]] ConversionProbe Probe(ValueView Source,
                                      const ConversionContext &Context) const {
    if (!Source.ToNumber())
      return RejectedProbe(Context.Describe("expected a number"));
    return ViableProbe(ConversionRank::User);
  }

  [[nodiscard]] ConversionResult<UserHandle>
  Read(ValueView Source, ConversionContext &Context) const {
    ConversionResult<UserHandle> Result;
    const std::optional<double> Number = Source.ToNumber();
    if (!Number) {
      Result.Status = ConversionStatus::TypeMismatch;
      Result.Diagnostic = Context.Describe("expected a number");
      return Result;
    }
    Result.Status = ConversionStatus::Success;
    Result.ConvertedValue = UserHandle{*Number};
    return Result;
  }

  [[nodiscard]] WriteResult Write(const UserHandle &Source,
                                  ConversionContext &Context) const {
    const OwnedValue Published = OwnedValue::Number(Source.Value);
    if (const WriteResult Reserved =
            Context.Reserve(Published.RequiredReservation());
        !Reserved.IsSuccess())
      return Reserved;
    return Context.Publish(Published);
  }
};

} // namespace Luna

namespace {

// The three categories are one ordered vocabulary in both spellings, and
// ordering is by category position rather than by any accumulated score.
static_assert(static_cast<int>(ConversionRankCategory::Exact) <
                      static_cast<int>(ConversionRankCategory::SafeBuiltIn) &&
                  static_cast<int>(ConversionRankCategory::SafeBuiltIn) <
                      static_cast<int>(ConversionRankCategory::User),
              "The registry's rank categories must stay ordered exact, safe "
              "built-in, user.");
static_assert(static_cast<int>(Luna::ConversionRank::Exact) <
                      static_cast<int>(Luna::ConversionRank::SafeBuiltIn) &&
                  static_cast<int>(Luna::ConversionRank::SafeBuiltIn) <
                      static_cast<int>(Luna::ConversionRank::User),
              "The public rank categories must stay ordered exact, safe "
              "built-in, user.");

void CheckRankCategoriesAreOrderedAndShared() {
  constexpr std::array Ordered{ConversionRankCategory::Exact,
                               ConversionRankCategory::SafeBuiltIn,
                               ConversionRankCategory::User};
  constexpr std::array<std::string_view, 3> ExpectedText{
      "exact", "safe_builtin", "user"};
  for (std::size_t Index = 0; Index < Ordered.size(); ++Index) {
    Check(ConversionRankCategoryText(Ordered[Index]) == ExpectedText[Index],
          "each rank category keeps its canonical text");
    Check(Luna::ConversionRankText(PublicRankOf(Ordered[Index])) ==
              ExpectedText[Index],
          "the public rank vocabulary matches the registry's category by "
          "category");
  }
}

void CheckDeclaredRanksFollowTheirConversion() {
  const std::shared_ptr<const TypeGeneration> BuiltIn =
      Luna::Detail::BuiltInTypeGeneration();
  Check(BuiltIn != nullptr, "the built-in generation exists");
  if (!BuiltIn)
    return;

  // A foundation conversion is exact; so is null, which is the one value its
  // own type describes.
  for (const FixedTypeKey Key :
       {FixedTypeKey::Void, FixedTypeKey::Boolean, FixedTypeKey::Int32,
        FixedTypeKey::Double, FixedTypeKey::String, FixedTypeKey::Null}) {
    const TypeRecord *Record = BuiltIn->Find(Fixed(Key));
    Check(Record != nullptr && Record->Rank == ConversionRankCategory::Exact,
          "a foundation type and null convert exactly");
  }

  // A representation-changing built-in scalar is a safe built-in conversion.
  for (const FixedTypeKey Key :
       {FixedTypeKey::Float, FixedTypeKey::StringView, FixedTypeKey::CString}) {
    const TypeRecord *Record = BuiltIn->Find(Fixed(Key));
    Check(Record != nullptr &&
              Record->Rank == ConversionRankCategory::SafeBuiltIn,
          "a planned built-in scalar converts as a safe built-in");
  }

  // No registry-owned declaration ever claims the user category: that rank
  // belongs to a consumer's own converter.
  bool AnyUserRank = false;
  for (const TypeRecord &Record : BuiltIn->All())
    AnyUserRank = AnyUserRank || Record.Rank == ConversionRankCategory::User;
  Check(!AnyUserRank,
        "the user rank category is reserved for consumer converters");

  // A structural aggregate is a safe built-in conversion, and an optional
  // inherits the rank of the type it wraps rather than inventing one.
  const struct AggregateCase final {
    TypeDescriptor Type;
    ConversionRankCategory Rank;
    std::string_view Description;
  } Cases[]{{Luna::Detail::SequenceTypeOf(Fixed(FixedTypeKey::Int32)),
             ConversionRankCategory::SafeBuiltIn,
             "a sequence is a safe built-in"},
            {Luna::Detail::MapTypeOf(Fixed(FixedTypeKey::String),
                                     Fixed(FixedTypeKey::Int32)),
             ConversionRankCategory::SafeBuiltIn, "a map is a safe built-in"},
            {Luna::Detail::PairTypeOf(Fixed(FixedTypeKey::Int32),
                                      Fixed(FixedTypeKey::String)),
             ConversionRankCategory::SafeBuiltIn, "a pair is a safe built-in"},
            {Luna::Detail::OptionalTypeOf(Fixed(FixedTypeKey::Int32)),
             ConversionRankCategory::Exact,
             "an optional of an exact type stays exact"},
            {Luna::Detail::OptionalTypeOf(Fixed(FixedTypeKey::Float)),
             ConversionRankCategory::SafeBuiltIn,
             "an optional of a safe built-in stays a safe built-in"}};

  for (const AggregateCase &Case : Cases) {
    const std::shared_ptr<const TypeGeneration> Types =
        Hooks::GenerationFor(Case.Type);
    const TypeRecord *Record = Types ? Types->Find(Case.Type) : nullptr;
    Check(Record != nullptr && Record->Rank == Case.Rank, Case.Description);
  }
}

void CheckConsumerConvertersReportTheirOwnRank() {
  // The frame token vocabulary is the public read/write direction, which is
  // separate from the argument/return direction diagnostics report.
  Frame Reading(Luna::ConversionDirection::Read, "Studio.Draw", 1);
  const Luna::ValueView Number = Reading.Open(Luna::OwnedValue::Number(2.5));
  const Luna::ConversionContext Probing = Reading.ProbeContext();

  const Luna::ConversionProbe Exact =
      Luna::ProbeValue<ExactHandle>(Number, Probing);
  const Luna::ConversionProbe Safe =
      Luna::ProbeValue<SafeHandle>(Number, Probing);
  const Luna::ConversionProbe User =
      Luna::ProbeValue<UserHandle>(Number, Probing);
  Check(Exact.IsViable && Safe.IsViable && User.IsViable,
        "each consumer converter finds the same value viable");
  Check(Exact.Rank == Luna::ConversionRank::Exact &&
            Safe.Rank == Luna::ConversionRank::SafeBuiltIn &&
            User.Rank == Luna::ConversionRank::User,
        "each consumer converter reports its own rank category");
  Check(static_cast<int>(Exact.Rank) < static_cast<int>(Safe.Rank) &&
            static_cast<int>(Safe.Rank) < static_cast<int>(User.Rank),
        "three viable candidates order exact before safe built-in before user");

  // The same value, the same generation, and the same policy probe identically
  // every time.
  Check(Luna::ProbeValue<UserHandle>(Number, Probing).Rank == User.Rank &&
            Luna::ProbeValue<ExactHandle>(Number, Probing).Rank == Exact.Rank,
        "ranking the same value twice is deterministic");
  Check(Reading.ProbeViolations().empty(),
        "ranking three candidates mutates nothing");
}

void CheckNullabilityIsDeclaredAndEnforced() {
  const std::shared_ptr<const TypeGeneration> BuiltIn =
      Luna::Detail::BuiltInTypeGeneration();
  if (!BuiltIn)
    return;

  for (const FixedTypeKey Key :
       {FixedTypeKey::Boolean, FixedTypeKey::Int32, FixedTypeKey::Double,
        FixedTypeKey::String, FixedTypeKey::Float, FixedTypeKey::StringView,
        FixedTypeKey::CString}) {
    const TypeRecord *Record = BuiltIn->Find(Fixed(Key));
    Check(Record != nullptr && !Record->IsNullable,
          "no scalar value type accepts nil");
  }

  const TypeRecord *Null = BuiltIn->Find(Fixed(FixedTypeKey::Null));
  Check(Null != nullptr && Null->IsNullable,
        "the null type is the nullable built-in");

  const TypeDescriptor Optional =
      Luna::Detail::OptionalTypeOf(Fixed(FixedTypeKey::Int32));
  const TypeDescriptor Sequence =
      Luna::Detail::SequenceTypeOf(Fixed(FixedTypeKey::Int32));
  const std::shared_ptr<const TypeGeneration> WithOptional =
      Hooks::GenerationFor(Optional);
  const std::shared_ptr<const TypeGeneration> WithSequence =
      Hooks::GenerationFor(Sequence);
  const TypeRecord *OptionalRecord =
      WithOptional ? WithOptional->Find(Optional) : nullptr;
  const TypeRecord *SequenceRecord =
      WithSequence ? WithSequence->Find(Sequence) : nullptr;
  Check(OptionalRecord != nullptr && OptionalRecord->IsNullable,
        "an optional is nullable");
  Check(SequenceRecord != nullptr && !SequenceRecord->IsNullable,
        "an aggregate is not nullable");

  // Enforcement matches the declaration in both directions.
  const auto ScalarNil =
      Hooks::Read(ScriptValue::Nil(), Fixed(FixedTypeKey::Int32));
  Check(!ScalarNil.Accepted &&
            ScalarNil.Diagnostic.Failure == StructuredFailure::TypeMismatch &&
            ScalarNil.Diagnostic.ReceivedType == "nil",
        "a non-nullable scalar refuses nil");
  Check(ArgumentMessage(ScalarNil.Diagnostic) ==
            "Callable 'Studio.Apply' argument 1 expected signed 32-bit integer "
            "but received nil.",
        "the nil refusal keeps the foundation wording");

  const auto AggregateNil = Hooks::Read(ScriptValue::Nil(), Sequence);
  Check(!AggregateNil.Accepted &&
            AggregateNil.Diagnostic.Failure == StructuredFailure::TypeMismatch,
        "a non-nullable aggregate refuses nil");

  const auto OptionalNil = Hooks::Read(ScriptValue::Nil(), Optional);
  Check(OptionalNil.Accepted && OptionalNil.ConvertedValue.IsNull(),
        "a nullable optional accepts nil");

  const auto WrittenNull =
      Hooks::Write(StructuredValue::Null(), Fixed(FixedTypeKey::Int32));
  Check(!WrittenNull.Accepted && WrittenNull.PublishedCount == 0 &&
            WrittenNull.StackDepthDelta == 0,
        "writing an absent value through a non-nullable type publishes "
        "nothing");
}

void CheckUnsupportedAndUnavailableTypesAreRefused() {
  const std::shared_ptr<const TypeGeneration> BuiltIn =
      Luna::Detail::BuiltInTypeGeneration();
  if (!BuiltIn)
    return;

  // An invalid canonical descriptor is never declared, and the refusal names
  // it.
  std::vector<TypeRecord> Declared;
  TypeDescriptor Blocking;
  Check(Luna::Detail::DeclareStructuralTypes(
            *BuiltIn, TypeDescriptor::Unsupported(), Declared, Blocking) ==
            StructuralDeclarationStatus::UnsupportedDescriptor,
        "an unsupported descriptor is refused");
  Check(Declared.empty() && Blocking == TypeDescriptor::Unsupported(),
        "a refused declaration adds nothing and names the blocking type");

  // A structural type over an aggregate of unsupported children is refused the
  // same way, before any child record is added.
  std::vector<TypeDescriptor> BadChildren;
  BadChildren.push_back(TypeDescriptor::Unsupported());
  Check(Luna::Detail::DeclareStructuralTypes(
            *BuiltIn, Luna::Detail::TupleTypeOf(std::move(BadChildren)),
            Declared,
            Blocking) == StructuralDeclarationStatus::UnsupportedDescriptor,
        "an aggregate over an unsupported child is refused");
  Check(Declared.empty(), "a refused aggregate declares no child record");

  // An unregistered class or enumeration leaf is available only once its own
  // registration declares it.
  const TypeDescriptor UnknownClass =
      TypeDescriptor::ForClass(StableTypeKey("Studio.Unregistered"));
  Check(Luna::Detail::DeclareStructuralTypes(
            *BuiltIn, Luna::Detail::SequenceTypeOf(UnknownClass), Declared,
            Blocking) == StructuralDeclarationStatus::UnavailableLeaf,
        "an unregistered class leaf is unavailable");
  Check(Declared.empty() && Blocking == UnknownClass,
        "the unavailable leaf itself is named");

  const TypeDescriptor UnknownEnumeration =
      TypeDescriptor::ForEnumeration(StableTypeKey("Studio.Unregistered"));
  Check(Luna::Detail::DeclareStructuralTypes(*BuiltIn, UnknownEnumeration,
                                             Declared, Blocking) ==
            StructuralDeclarationStatus::UnavailableLeaf,
        "an unregistered enumeration leaf is unavailable");

  // Conversion through a type the captured generation cannot describe is one
  // deterministic internal refusal rather than a guess.
  Check(Hooks::GenerationFor(TypeDescriptor::Unsupported()) == nullptr,
        "an unsupported type never enters a generation");
  const auto Read =
      Hooks::Read(ScriptValue::Number(1), TypeDescriptor::Unsupported());
  Check(!Read.Accepted &&
            Read.Diagnostic.Failure == StructuredFailure::UnavailableType,
        "reading an unavailable type is refused");
  Check(IsInternalStructuredFailure(Read.Diagnostic.Failure),
        "an unavailable type is an internal refusal");
  Check(ArgumentMessage(Read.Diagnostic)
                .find("has no available conversion in the captured type "
                      "registry") != std::string::npos,
        "the unavailable-type refusal names the captured registry");

  const auto Written = Hooks::Write(StructuredValue::Scalar(Value(1)),
                                    TypeDescriptor::Unsupported());
  Check(!Written.Accepted && Written.PublishedCount == 0 &&
            Written.StackDepthDelta == 0,
        "writing an unavailable type publishes nothing");
}

void CheckEnumerationAndClassIdentity() {
  const StableTypeKey Key("Studio.Alignment");
  const TypeRecord Enumeration =
      Luna::Detail::DeclareEnumerationTypeRecord(Key, "Studio.Alignment");
  const TypeRecord Class =
      Luna::Detail::DeclareClassTypeRecord(Key, "Studio.Alignment");

  const auto EnumerationIdentity =
      Luna::Detail::TypeIdentityRegistry::ComputeIdentity(
          TypeDescriptor::ForEnumeration(Key));
  const auto ClassIdentity =
      Luna::Detail::TypeIdentityRegistry::ComputeIdentity(
          TypeDescriptor::ForClass(Key));
  Check(EnumerationIdentity.has_value() && ClassIdentity.has_value(),
        "an enumeration and a class both resolve a canonical identity");
  Check(Enumeration.Identity == *EnumerationIdentity &&
            Class.Identity == *ClassIdentity,
        "a declared leaf identity follows its canonical descriptor");
  Check(Enumeration.Identity != Class.Identity,
        "one stable key used as an enumeration and as a class stays two "
        "distinct types");
  Check(Enumeration.Descriptor.Kind() == TypeKind::Enumeration &&
            Class.Descriptor.Kind() == TypeKind::Class &&
            Enumeration.Descriptor.Key() == Class.Descriptor.Key(),
        "both leaves keep the same stable key and their own kind");

  // A different key is a different type, and both remain complete declarations.
  const TypeRecord Other = Luna::Detail::DeclareEnumerationTypeRecord(
      StableTypeKey("Studio.Anchor"), "Studio.Anchor");
  Check(Other.Identity != Enumeration.Identity,
        "two enumeration keys resolve two identities");
  Check(Enumeration.IsComplete() && Class.IsComplete() && Other.IsComplete(),
        "every declared leaf is a complete record");
}

void CheckEmptyAggregatesConvertAndPublish() {
  const struct EmptyCase final {
    TypeDescriptor Type;
    std::string_view Description;
  } Cases[]{{Luna::Detail::MapTypeOf(Fixed(FixedTypeKey::String),
                                     Fixed(FixedTypeKey::Int32)),
             "an empty map"},
            {Luna::Detail::FixedArrayTypeOf(Fixed(FixedTypeKey::Int32), 0),
             "a zero-extent fixed array"},
            {Luna::Detail::TupleTypeOf({}), "an empty tuple"}};

  for (const EmptyCase &Case : Cases) {
    const auto Read = Hooks::Read(ScriptValue::Array({}), Case.Type);
    Check(Read.Accepted && Read.ConvertedValue.Size() == 0,
          "an empty table reads as an empty aggregate");
    Check(Read.StackDepthDelta == 0, "reading an empty aggregate leaves the "
                                     "stack untouched");

    const StructuredValue Staged = Case.Type.Kind() == TypeKind::Map
                                       ? StructuredValue::Map({})
                                       : StructuredValue::List({});
    const auto Written = Hooks::Write(Staged, Case.Type);
    Check(Written.Accepted && Written.PublishedCount == 1 &&
              Written.RoundTripMatches,
          "an empty aggregate publishes one empty table and round-trips");
  }

  // An empty return pack is the ordered-multiple-value shape with no values.
  const auto EmptyPack = Hooks::PublishReturn(
      StructuredValue::List({}), Luna::Detail::ReturnPackTypeOf({}));
  Check(EmptyPack.Accepted && EmptyPack.PublishedCount == 0 &&
            EmptyPack.StackDepthDelta == 0,
        "an empty return pack publishes zero values");
}

void CheckPublicationReservesRealStackCapacity() {
  // A homogeneous return pack has no Luna-chosen element cap, so a large pack
  // publishes its ordered values.
  const TypeDescriptor Pack =
      Luna::Detail::ReturnPackTypeOf({Fixed(FixedTypeKey::Int32)});

  std::vector<StructuredValue> Accepted;
  for (int Element = 0; Element < 1'000; ++Element)
    Accepted.push_back(StructuredValue::Scalar(Value(Element)));
  const auto Published =
      Hooks::PublishReturn(StructuredValue::List(std::move(Accepted)), Pack);
  Check(Published.Accepted && Published.PublishedCount == 1'000 &&
            Published.RoundTripMatches,
        "a large return pack publishes every ordered value");

  // The one remaining bound is the capacity the virtual machine can actually
  // reserve. Reserving it fails before anything is published, and the refusal
  // reports the number of slots the publication needed.
  constexpr std::size_t Unreservable = 8'192;
  std::vector<StructuredValue> Refused;
  for (std::size_t Element = 0; Element < Unreservable; ++Element)
    Refused.push_back(StructuredValue::Scalar(Value(1)));
  const auto Rejected =
      Hooks::PublishReturn(StructuredValue::List(std::move(Refused)), Pack);
  Check(!Rejected.Accepted &&
            Rejected.Diagnostic.Failure == StructuredFailure::StackUnavailable,
        "a publication that cannot reserve its resources is refused");
  Check(Rejected.PublishedCount == 0 && Rejected.StackDepthDelta == 0,
        "a refused reservation publishes no partial return pack");
  Check(Rejected.Diagnostic.ReceivedCount == Unreservable + 1,
        "the refusal reports the slot count the publication needed");
  Check(ReturnMessage(Rejected.Diagnostic) ==
            "Internal error: Callable 'Studio.Publish' return 1 could not "
            "reserve " +
                std::to_string(Unreservable + 1) + " stack slots.",
        "the reservation refusal is one deterministic internal diagnostic");
  Check(IsInternalStructuredFailure(Rejected.Diagnostic.Failure),
        "a reservation failure is an internal refusal");
}

} // namespace

int RunConversionRegistryEdgeCaseTests() {
  FailureCount = 0;

  CheckRankCategoriesAreOrderedAndShared();
  CheckDeclaredRanksFollowTheirConversion();
  CheckConsumerConvertersReportTheirOwnRank();
  CheckNullabilityIsDeclaredAndEnforced();
  CheckUnsupportedAndUnavailableTypesAreRefused();
  CheckEnumerationAndClassIdentity();
  CheckEmptyAggregatesConvertAndPublish();
  CheckPublicationReservesRealStackCapacity();

  Luna::Detail::ResetConversionBoundaryDiagnostics();
  return FailureCount == 0 ? 0 : 1;
}
