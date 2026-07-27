// clang-format off
#include <luna/binding/conversion.hpp>
#include <luna/binding/value.hpp>
#include <luna/luna.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/type/conversion_frame.hpp"
#include "state/type/structural_types.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/testing/structural_test_hooks.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"

#include <rapidcheck.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Luna::FixedTypeKey;
using Luna::StableTypeKey;
using Luna::TypeDescriptor;
using Luna::TypeKind;
using Luna::Value;
using Luna::Detail::HasSameStructure;
using Luna::Detail::ScriptValue;
using Luna::Detail::StructuredFailure;
using Luna::Detail::StructuredValue;
using Luna::Detail::TypeGeneration;
using Luna::Detail::TypeRecord;
using Hooks = Luna::Detail::StructuralConversionTestHooks;

class SeedCursor final {
public:
  explicit SeedCursor(const std::vector<std::uint8_t> &Bytes) noexcept
      : Source(&Bytes) {}

  [[nodiscard]] std::uint8_t Next() noexcept {
    const std::size_t Size = Source->size();
    const std::uint8_t Base = Size == 0 ? 0 : (*Source)[Position % Size];
    const std::uint8_t Mixed =
        static_cast<std::uint8_t>(Base + Position * 37U + 11U);
    ++Position;
    return Mixed;
  }

  [[nodiscard]] std::size_t Bounded(std::size_t Limit) noexcept {
    if (Limit == 0)
      return 0;
    return static_cast<std::size_t>(Next()) % Limit;
  }

private:
  const std::vector<std::uint8_t> *Source;
  std::size_t Position = 0;
};

[[nodiscard]] TypeDescriptor Fixed(FixedTypeKey Key) {
  return TypeDescriptor::ForFixed(Key);
}

[[nodiscard]] StableTypeKey EnumerationKey() {
  return StableTypeKey("Studio.Alignment");
}

[[nodiscard]] StableTypeKey ClassKey() {
  return StableTypeKey("Studio.Vector");
}

[[nodiscard]] std::string GenerateText(SeedCursor &Seeds) {
  const std::size_t Length = Seeds.Bounded(5);
  std::string Text;
  for (std::size_t Index = 0; Index < Length; ++Index)
    Text.push_back(static_cast<char>('a' + Seeds.Bounded(26)));
  return Text;
}

[[nodiscard]] bool IsTextType(const TypeDescriptor &Type) {
  const auto Key = Type.FixedKey();
  if (!Key)
    return false;
  return *Key == FixedTypeKey::String || *Key == FixedTypeKey::StringView ||
         *Key == FixedTypeKey::CString;
}

[[nodiscard]] TypeDescriptor GenerateLeafType(SeedCursor &Seeds,
                                              bool &UsesEnumeration) {
  switch (Seeds.Bounded(8)) {
  case 0:
    return Fixed(FixedTypeKey::Boolean);
  case 1:
    return Fixed(FixedTypeKey::Int32);
  case 2:
    return Fixed(FixedTypeKey::Double);
  case 3:
    return Fixed(FixedTypeKey::Float);
  case 4:
    return Fixed(FixedTypeKey::String);
  case 5:
    return Fixed(FixedTypeKey::StringView);
  case 6:
    return Fixed(FixedTypeKey::CString);
  default:
    UsesEnumeration = true;
    return TypeDescriptor::ForEnumeration(EnumerationKey());
  }
}

[[nodiscard]] TypeDescriptor GenerateMapKeyType(SeedCursor &Seeds) {
  return Seeds.Bounded(2) == 0 ? Fixed(FixedTypeKey::Int32)
                               : Fixed(FixedTypeKey::String);
}

[[nodiscard]] TypeDescriptor GenerateType(SeedCursor &Seeds, int Depth,
                                          bool &UsesEnumeration) {
  if (Depth <= 0)
    return GenerateLeafType(Seeds, UsesEnumeration);

  switch (Seeds.Bounded(10)) {
  case 0:
  case 1:
  case 2:
    return GenerateLeafType(Seeds, UsesEnumeration);
  case 3:
    return Luna::Detail::OptionalTypeOf(
        GenerateType(Seeds, Depth - 1, UsesEnumeration));
  case 4:
    return Luna::Detail::SequenceTypeOf(
        GenerateType(Seeds, Depth - 1, UsesEnumeration));
  case 5: {
    TypeDescriptor Element = GenerateType(Seeds, Depth - 1, UsesEnumeration);
    return Luna::Detail::FixedArrayTypeOf(std::move(Element),
                                          1 + Seeds.Bounded(3));
  }
  case 6: {
    TypeDescriptor Key = GenerateMapKeyType(Seeds);
    TypeDescriptor Mapped = GenerateType(Seeds, Depth - 1, UsesEnumeration);
    return Luna::Detail::MapTypeOf(std::move(Key), std::move(Mapped));
  }
  case 7: {
    TypeDescriptor First = GenerateType(Seeds, Depth - 1, UsesEnumeration);
    TypeDescriptor Second = GenerateType(Seeds, Depth - 1, UsesEnumeration);
    return Luna::Detail::PairTypeOf(std::move(First), std::move(Second));
  }
  case 8: {
    std::vector<TypeDescriptor> Elements;
    const std::size_t Count = 2 + Seeds.Bounded(3);
    for (std::size_t Index = 0; Index < Count; ++Index)
      Elements.push_back(GenerateType(Seeds, Depth - 1, UsesEnumeration));
    return Luna::Detail::TupleTypeOf(std::move(Elements));
  }
  default:
    return Luna::Detail::SequenceTypeOf(
        GenerateLeafType(Seeds, UsesEnumeration));
  }
}

struct StagedValue final {
  ScriptValue Script;
  StructuredValue Staged;
};

[[nodiscard]] StagedValue GenerateValue(const TypeDescriptor &Type,
                                        SeedCursor &Seeds);

[[nodiscard]] StagedValue GenerateLeafValue(const TypeDescriptor &Type,
                                            SeedCursor &Seeds) {
  if (Type.Kind() == TypeKind::Enumeration) {
    const int Enumerator = static_cast<int>(Seeds.Bounded(8));
    return StagedValue{ScriptValue::Number(static_cast<double>(Enumerator)),
                       StructuredValue::Scalar(Value(Enumerator))};
  }

  const auto Key = Type.FixedKey();
  if (!Key)
    return StagedValue{ScriptValue::Nil(), StructuredValue::Null()};

  switch (*Key) {
  case FixedTypeKey::Boolean: {
    const bool Flag = Seeds.Bounded(2) == 0;
    return StagedValue{ScriptValue::Boolean(Flag),
                       StructuredValue::Scalar(Value(Flag))};
  }
  case FixedTypeKey::Int32: {
    const int Number = static_cast<int>(Seeds.Next()) - 128;
    return StagedValue{ScriptValue::Number(static_cast<double>(Number)),
                       StructuredValue::Scalar(Value(Number))};
  }
  case FixedTypeKey::Double: {
    const double Number = (static_cast<double>(Seeds.Next()) - 128.0) / 4.0;
    return StagedValue{ScriptValue::Number(Number),
                       StructuredValue::Scalar(Value(Number))};
  }
  case FixedTypeKey::Float: {
    const double Narrowed = static_cast<double>(
        static_cast<float>((static_cast<double>(Seeds.Next()) - 128.0) / 4.0));
    return StagedValue{ScriptValue::Number(Narrowed),
                       StructuredValue::Scalar(Value(Narrowed))};
  }
  case FixedTypeKey::String:
  case FixedTypeKey::StringView:
  case FixedTypeKey::CString: {
    const std::string Text = GenerateText(Seeds);
    return StagedValue{ScriptValue::Text(Text),
                       StructuredValue::Scalar(Value(Text))};
  }
  default:
    break;
  }
  return StagedValue{ScriptValue::Nil(), StructuredValue::Null()};
}

[[nodiscard]] StagedValue GenerateListValue(const TypeDescriptor &Type,
                                            std::size_t Count, bool Homogeneous,
                                            SeedCursor &Seeds) {
  const std::span<const TypeDescriptor> Children = Type.Children();
  std::vector<ScriptValue> Scripts;
  std::vector<StructuredValue> Staged;
  for (std::size_t Index = 0; Index < Count; ++Index) {
    const TypeDescriptor &Element =
        Homogeneous ? Children[0] : Children[Index % Children.size()];
    StagedValue Generated = GenerateValue(Element, Seeds);
    Scripts.push_back(std::move(Generated.Script));
    Staged.push_back(std::move(Generated.Staged));
  }
  return StagedValue{ScriptValue::Array(std::move(Scripts)),
                     StructuredValue::List(std::move(Staged))};
}

StagedValue GenerateValue(const TypeDescriptor &Type, SeedCursor &Seeds) {
  const std::span<const TypeDescriptor> Children = Type.Children();
  switch (Type.Kind()) {
  case TypeKind::Fixed:
  case TypeKind::Enumeration:
    return GenerateLeafValue(Type, Seeds);
  case TypeKind::Optional:
    return GenerateValue(Children[0], Seeds);
  case TypeKind::Sequence:
    return GenerateListValue(Type, Seeds.Bounded(4), true, Seeds);
  case TypeKind::Array:
    return GenerateListValue(Type, Type.ArrayExtent(), true, Seeds);
  case TypeKind::Pair:
  case TypeKind::Tuple:
    return GenerateListValue(Type, Children.size(), false, Seeds);
  case TypeKind::Map: {
    const std::size_t Count = Seeds.Bounded(3);
    const bool TextKeys = IsTextType(Children[0]);
    std::vector<ScriptValue> Entries;
    std::vector<StructuredValue> Staged;
    for (std::size_t Index = 0; Index < Count; ++Index) {
      if (TextKeys) {
        const std::string Key = "K" + std::to_string(Index);
        Entries.push_back(ScriptValue::Text(Key));
        Staged.push_back(StructuredValue::Scalar(Value(Key)));
      } else {
        const int Key = static_cast<int>(Index) + 1;
        Entries.push_back(ScriptValue::Number(static_cast<double>(Key)));
        Staged.push_back(StructuredValue::Scalar(Value(Key)));
      }
      StagedValue Mapped = GenerateValue(Children[1], Seeds);
      Entries.push_back(std::move(Mapped.Script));
      Staged.push_back(std::move(Mapped.Staged));
    }
    return StagedValue{ScriptValue::Table(std::move(Entries)),
                       StructuredValue::Map(std::move(Staged))};
  }
  default:
    break;
  }
  return StagedValue{ScriptValue::Nil(), StructuredValue::Null()};
}

[[nodiscard]] ScriptValue RefusedValue(const TypeDescriptor &Type) {
  if (Type.Kind() == TypeKind::Optional)
    return RefusedValue(Type.Children()[0]);
  if (IsTextType(Type))
    return ScriptValue::Number(1.0);
  return ScriptValue::Text("refused");
}

[[nodiscard]] int PublishedValueCount(const TypeDescriptor &Type) {
  switch (Type.Kind()) {
  case TypeKind::Pair:
    return 2;
  case TypeKind::Tuple:
  case TypeKind::ReturnPack:
    return static_cast<int>(Type.ChildCount());
  default:
    return 1;
  }
}

[[nodiscard]] Luna::OwnedValue OwnedFromScript(const ScriptValue &Source) {
  switch (Source.Kind()) {
  case ScriptValue::ScriptKind::Nil:
    return Luna::OwnedValue::Nil();
  case ScriptValue::ScriptKind::Boolean:
    return Luna::OwnedValue::Boolean(Source.BooleanValue());
  case ScriptValue::ScriptKind::Number:
    return Luna::OwnedValue::Number(Source.NumberValue());
  case ScriptValue::ScriptKind::Text:
    return Luna::OwnedValue::Text(Source.TextValue());
  case ScriptValue::ScriptKind::Array: {
    Luna::OwnedValue Table = Luna::OwnedValue::Table();
    for (const ScriptValue &Element : Source.Items())
      Table.Append(OwnedFromScript(Element));
    return Table;
  }
  case ScriptValue::ScriptKind::Table: {
    Luna::OwnedValue Table = Luna::OwnedValue::Table();
    const std::vector<ScriptValue> &Items = Source.Items();
    for (std::size_t Index = 0; Index + 1 < Items.size(); Index += 2) {
      if (Items[Index].Kind() == ScriptValue::ScriptKind::Text)
        Table.SetField(Items[Index].TextValue(),
                       OwnedFromScript(Items[Index + 1]));
      else
        Table.Append(OwnedFromScript(Items[Index + 1]));
    }
    return Table;
  }
  }
  return Luna::OwnedValue::Nil();
}

} // namespace

int RunCanonicalConversionRoundTripProperties() {
  // clang-format off
  // Feature: reflection-driven-binding-system, Property 22: Canonical conversions round-trip without partial aggregates
  const bool Passed = rc::check(
      // clang-format on
      "Canonical conversions round-trip without partial aggregates",
      [](const std::vector<std::uint8_t> &GeneratedSeeds,
         std::int32_t GeneratedNumber) {
        SeedCursor Seeds(GeneratedSeeds);

        bool UsesEnumeration = false;
        const TypeDescriptor Type = GenerateType(Seeds, 2, UsesEnumeration);
        RC_ASSERT(Type.IsValid());

        std::vector<TypeRecord> Extra;
        if (UsesEnumeration) {
          Extra.push_back(Luna::Detail::DeclareEnumerationTypeRecord(
              EnumerationKey(), "Studio.Alignment"));
        }

        const StagedValue Generated = GenerateValue(Type, Seeds);

        const std::shared_ptr<const TypeGeneration> First =
            Hooks::GenerationFor(Type, Extra);
        const std::shared_ptr<const TypeGeneration> Second =
            Hooks::GenerationFor(Type, Extra);
        RC_ASSERT(First != nullptr);
        RC_ASSERT(Second != nullptr);
        const TypeRecord *FirstRecord = First->Find(Type);
        const TypeRecord *SecondRecord = Second->Find(Type);
        RC_ASSERT(FirstRecord != nullptr);
        RC_ASSERT(SecondRecord != nullptr);
        RC_ASSERT(FirstRecord->Rank == SecondRecord->Rank);
        RC_ASSERT(FirstRecord->PublicName == SecondRecord->PublicName);
        RC_ASSERT(FirstRecord->PublicName ==
                  Luna::Detail::StructuralPublicName(Type));
        RC_ASSERT(First->IsAvailableForRead(Type));
        RC_ASSERT(First->IsAvailableForWrite(Type));

        const std::shared_ptr<const TypeGeneration> Foundation =
            TypeGeneration::Foundation();
        RC_ASSERT(Foundation != nullptr);
        for (const FixedTypeKey Key :
             {FixedTypeKey::Void, FixedTypeKey::Boolean, FixedTypeKey::Int32,
              FixedTypeKey::Double, FixedTypeKey::String}) {
          const TypeRecord *Inherited = First->Find(Fixed(Key));
          const TypeRecord *Original = Foundation->Find(Fixed(Key));
          RC_ASSERT(Inherited != nullptr);
          RC_ASSERT(Original != nullptr);
          RC_ASSERT(Inherited->PublicName == Original->PublicName);
          RC_ASSERT(Inherited->Rank ==
                    Luna::Detail::ConversionRankCategory::Exact);
        }

        const auto Read = Hooks::Read(Generated.Script, Type, Extra);
        RC_ASSERT(Read.Accepted);
        RC_ASSERT(HasSameStructure(Read.ConvertedValue, Generated.Staged));
        RC_ASSERT(Read.StackDepthDelta == 0);

        const auto Repeated = Hooks::Read(Generated.Script, Type, Extra);
        RC_ASSERT(Repeated.Accepted == Read.Accepted);
        RC_ASSERT(
            HasSameStructure(Repeated.ConvertedValue, Read.ConvertedValue));

        const auto Written = Hooks::Write(Generated.Staged, Type, Extra);
        RC_ASSERT(Written.Accepted);
        RC_ASSERT(Written.PublishedCount == 1);
        RC_ASSERT(Written.StackDepthDelta == 1);
        RC_ASSERT(Written.RoundTripMatches);

        const auto Nothing = Hooks::PublishReturn(StructuredValue::Null(),
                                                  Fixed(FixedTypeKey::Void));
        RC_ASSERT(Nothing.Accepted);
        RC_ASSERT(Nothing.PublishedCount == 0);
        RC_ASSERT(Nothing.StackDepthDelta == 0);

        const auto One = Hooks::PublishReturn(Generated.Staged, Type, Extra);
        RC_ASSERT(One.Accepted);
        RC_ASSERT(One.PublishedCount == PublishedValueCount(Type));
        RC_ASSERT(One.RoundTripMatches);
        RC_ASSERT(One.StackDepthDelta == One.PublishedCount);

        const std::string PackText = GenerateText(Seeds);
        const TypeDescriptor Pack = Luna::Detail::ReturnPackTypeOf(
            {Fixed(FixedTypeKey::Boolean), Type, Fixed(FixedTypeKey::String)});
        std::vector<StructuredValue> PackElements;
        PackElements.push_back(StructuredValue::Scalar(Value(true)));
        PackElements.push_back(Generated.Staged);
        PackElements.push_back(StructuredValue::Scalar(Value(PackText)));
        const StructuredValue PackSource =
            StructuredValue::List(std::move(PackElements));
        const auto Many = Hooks::PublishReturn(PackSource, Pack, Extra);
        RC_ASSERT(Many.Accepted);
        RC_ASSERT(Many.PublishedCount == 3);
        RC_ASSERT(Many.StackDepthDelta == 3);
        RC_ASSERT(Many.RoundTripMatches);

        const auto NullRead =
            Hooks::Read(ScriptValue::Nil(), Fixed(FixedTypeKey::Null));
        RC_ASSERT(NullRead.Accepted);
        RC_ASSERT(NullRead.ConvertedValue.IsNull());
        const TypeDescriptor Absent = Luna::Detail::OptionalTypeOf(
            Type.Kind() == TypeKind::Optional ? Fixed(FixedTypeKey::Int32)
                                              : Type);
        const auto AbsentRead = Hooks::Read(ScriptValue::Nil(), Absent, Extra);
        RC_ASSERT(AbsentRead.Accepted);
        RC_ASSERT(AbsentRead.ConvertedValue.IsNull());
        const auto AbsentWritten =
            Hooks::Write(StructuredValue::Null(), Absent, Extra);
        RC_ASSERT(AbsentWritten.Accepted);
        RC_ASSERT(AbsentWritten.PublishedCount == 1);
        RC_ASSERT(AbsentWritten.RoundTripMatches);

        const TypeDescriptor ArgumentPack =
            Luna::Detail::ArgumentPackTypeOf({Type});
        const std::size_t SuppliedCount = 1 + Seeds.Bounded(3);
        const std::size_t FirstPosition = 1 + Seeds.Bounded(3);
        std::vector<ScriptValue> Supplied;
        std::vector<StructuredValue> SuppliedStaged;
        for (std::size_t Index = 0; Index < SuppliedCount; ++Index) {
          Supplied.push_back(Generated.Script);
          SuppliedStaged.push_back(Generated.Staged);
        }
        const auto PackRead =
            Hooks::ReadPack(Supplied, ArgumentPack, FirstPosition, Extra);
        RC_ASSERT(PackRead.Accepted);
        RC_ASSERT(
            HasSameStructure(PackRead.ConvertedValue,
                             StructuredValue::List(std::move(SuppliedStaged))));
        RC_ASSERT(PackRead.StackDepthDelta == 0);

        std::vector<ScriptValue> Corrupted = Supplied;
        Corrupted.back() = RefusedValue(Type);
        const auto PackFailure =
            Hooks::ReadPack(Corrupted, ArgumentPack, FirstPosition, Extra);
        RC_ASSERT(!PackFailure.Accepted);
        RC_ASSERT(PackFailure.Diagnostic.Position ==
                  FirstPosition + SuppliedCount - 1);
        RC_ASSERT(PackFailure.ConvertedValue.Size() == 0);
        RC_ASSERT(PackFailure.StackDepthDelta == 0);

        const TypeDescriptor Nested =
            Luna::Detail::SequenceTypeOf(Luna::Detail::SequenceTypeOf(Type));
        const auto NestedAccepted = Hooks::Read(
            ScriptValue::Array({ScriptValue::Array({Generated.Script}),
                                ScriptValue::Array({Generated.Script})}),
            Nested, Extra);
        RC_ASSERT(NestedAccepted.Accepted);
        RC_ASSERT(NestedAccepted.ConvertedValue.Size() == 2);

        const auto NestedFailure = Hooks::Read(
            ScriptValue::Array({ScriptValue::Array({Generated.Script}),
                                ScriptValue::Array({RefusedValue(Type)})}),
            Nested, Extra);
        RC_ASSERT(!NestedFailure.Accepted);
        RC_ASSERT(NestedFailure.Diagnostic.Failure != StructuredFailure::None);
        RC_ASSERT(NestedFailure.Diagnostic.Path == "[2][1]");
        RC_ASSERT(!NestedFailure.Diagnostic.ExpectedType.empty());
        RC_ASSERT(NestedFailure.ConvertedValue.Size() == 0);
        RC_ASSERT(NestedFailure.StackDepthDelta == 0);

        const Luna::Detail::ConversionSubject Subject{
            .Kind = Luna::Detail::ConversionSubjectKind::Callable,
            .Name = "Studio.Apply"};
        const std::string Message = Luna::Detail::DescribeConversionFailure(
            Subject, Luna::Detail::ConversionDirection::Argument, 3,
            NestedFailure.Diagnostic);
        RC_ASSERT(Message.find("Callable 'Studio.Apply' argument 3[2][1]") ==
                  0);

        const TypeDescriptor NestedText = Luna::Detail::SequenceTypeOf(
            Luna::Detail::SequenceTypeOf(Fixed(FixedTypeKey::CString)));
        std::vector<StructuredValue> GoodRow;
        GoodRow.push_back(
            StructuredValue::Scalar(Value(GenerateText(Seeds) + "ok")));
        std::vector<StructuredValue> BadRow;
        BadRow.push_back(
            StructuredValue::Scalar(Value(std::string("a\0b", 3))));
        std::vector<StructuredValue> Rows;
        Rows.push_back(StructuredValue::List(std::move(GoodRow)));
        Rows.push_back(StructuredValue::List(std::move(BadRow)));
        const auto WriteFailure =
            Hooks::Write(StructuredValue::List(std::move(Rows)), NestedText);
        RC_ASSERT(!WriteFailure.Accepted);
        RC_ASSERT(WriteFailure.Diagnostic.Failure ==
                  StructuredFailure::EmbeddedNullByte);
        RC_ASSERT(WriteFailure.Diagnostic.Path == "[2][1]");
        RC_ASSERT(WriteFailure.PublishedCount == 0);
        RC_ASSERT(WriteFailure.StackDepthDelta == 0);
        RC_ASSERT(!WriteFailure.RoundTripMatches);

        const TypeDescriptor Class = TypeDescriptor::ForClass(ClassKey());
        std::vector<TypeRecord> ClassRecords = Extra;
        ClassRecords.push_back(
            Luna::Detail::DeclareClassTypeRecord(ClassKey(), "Studio.Vector"));
        const TypeDescriptor ClassSequence =
            Luna::Detail::SequenceTypeOf(Class);
        const std::shared_ptr<const TypeGeneration> WithClass =
            Hooks::GenerationFor(ClassSequence, ClassRecords);
        RC_ASSERT(WithClass != nullptr);
        RC_ASSERT(WithClass->IsAvailableForRead(ClassSequence));
        const auto ClassRead =
            Hooks::Read(ScriptValue::Nil(), Class, ClassRecords);
        RC_ASSERT(!ClassRead.Accepted);
        RC_ASSERT(ClassRead.Diagnostic.Failure ==
                  StructuredFailure::UnavailableType);
        RC_ASSERT(ClassRead.StackDepthDelta == 0);

        const Luna::OwnedValue Owned = OwnedFromScript(Generated.Script);
        Luna::Detail::ResetConversionBoundaryDiagnostics();
        {
          Luna::Detail::ConversionFrame Writing(
              Luna::ConversionDirection::Write, "Studio.Apply", 2);
          const Luna::ValueView Root = Writing.Open(Owned);
          RC_ASSERT(Root.IsActive());

          const Luna::ConversionContext Probing = Writing.ProbeContext();
          RC_ASSERT(Probing.IsProbing());
          RC_ASSERT(!Writing.CommitContext().IsProbing());

          static_cast<void>(Probing.Kind());
          static_cast<void>(Probing.Size());
          static_cast<void>(Probing.Path());
          RC_ASSERT(Luna::Detail::ProbeViolationCount() == 0);
          RC_ASSERT(Writing.ProbeViolations().empty());

          Luna::ConversionContext Escaped = Probing;
          RC_ASSERT(Escaped.Reserve(Owned.RequiredReservation()).Status ==
                    Luna::WriteStatus::ProbeViolation);
          RC_ASSERT(Escaped.Publish(Owned).Status ==
                    Luna::WriteStatus::ProbeViolation);
          RC_ASSERT(Escaped.PublishPack(Luna::ValuePack()).Status ==
                    Luna::WriteStatus::ProbeViolation);
          RC_ASSERT(Luna::Detail::ProbeViolationCount() == 3);
          RC_ASSERT(!Writing.HasReservation());
          RC_ASSERT(!Writing.IsPublished());
          RC_ASSERT(!Writing.PublishedResult().has_value());

          Luna::ConversionContext Committing = Writing.CommitContext();
          RC_ASSERT(
              Committing.Reserve(Owned.RequiredReservation()).IsSuccess());
          RC_ASSERT(Committing.Publish(Owned).IsSuccess());
          RC_ASSERT(Writing.IsPublished());
          RC_ASSERT(Writing.PublishedResult().has_value());
          RC_ASSERT(*Writing.PublishedResult() == Owned);
        }
        Luna::Detail::ResetConversionBoundaryDiagnostics();

        Luna::State State;
        RC_ASSERT(State.IsReady());
        int Observed = 0;
        RC_ASSERT(
            State.Bindings()
                .Register("Identity", [](int Argument) { return Argument; })
                .IsSuccess());
        RC_ASSERT(
            State.Bindings()
                .Register("Observe", [&](int Argument) { Observed = Argument; })
                .IsSuccess());
        const auto Execution = State.Execute(
            "Observe(Identity(" +
            std::to_string(static_cast<int>(GeneratedNumber)) + "))\n");
        RC_ASSERT(Execution.IsSuccess());
        RC_ASSERT(Observed == static_cast<int>(GeneratedNumber));
      });

  return Passed ? 0 : 1;
}
