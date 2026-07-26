// clang-format off
#include <luna/binding/value.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/type/structural_types.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/testing/structural_test_hooks.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"

#include <cstddef>
#include <iostream>
#include <memory>
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
using Luna::Detail::ConversionDirection;
using Luna::Detail::ConversionSubject;
using Luna::Detail::ConversionSubjectKind;
using Luna::Detail::DescribeConversionFailure;
using Luna::Detail::HasSameStructure;
using Luna::Detail::MaximumInvocationStringBytes;
using Luna::Detail::ScriptValue;
using Luna::Detail::StructuralDeclarationStatus;
using Luna::Detail::StructuredFailure;
using Luna::Detail::StructuredKind;
using Luna::Detail::StructuredValue;
using Luna::Detail::TypeGeneration;
using Luna::Detail::TypeRecord;
using Hooks = Luna::Detail::StructuralConversionTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "structural conversion check failed: " << Description << '\n';
}

[[nodiscard]] TypeDescriptor Fixed(FixedTypeKey Key) {
  return TypeDescriptor::ForFixed(Key);
}

[[nodiscard]] std::string
ArgumentMessage(const Luna::Detail::StructuredDiagnostic &Diagnostic,
                std::size_t Position = 1) {
  const ConversionSubject Subject{.Kind = ConversionSubjectKind::Callable,
                                  .Name = "Studio.Apply"};
  return DescribeConversionFailure(Subject, ConversionDirection::Argument,
                                   Position, Diagnostic);
}

[[nodiscard]] StructuredValue Scalar(Value Source) {
  return StructuredValue::Scalar(std::move(Source));
}

[[nodiscard]] StructuredValue IntegerList(std::vector<int> Values) {
  std::vector<StructuredValue> Elements;
  for (const int Element : Values)
    Elements.push_back(Scalar(Value(Element)));
  return StructuredValue::List(std::move(Elements));
}

// -- planned built-in scalars ------------------------------------------------

void CheckSinglePrecision() {
  const TypeDescriptor Float = Fixed(FixedTypeKey::Float);

  const auto Accepted = Hooks::Read(ScriptValue::Number(0.5), Float);
  Check(Accepted.Accepted &&
            Accepted.ConvertedValue.Kind() == StructuredKind::Scalar,
        "float reads a Luau number");
  Check(Accepted.StackDepthDelta == 0,
        "reading a float leaves no value behind");

  const auto Narrowed = Hooks::Read(ScriptValue::Number(0.1), Float);
  Check(Narrowed.Accepted && Narrowed.ConvertedValue.ScalarValue() &&
            *Narrowed.ConvertedValue.ScalarValue() ==
                Value(static_cast<double>(static_cast<float>(0.1))),
        "float narrows exactly once, to single precision");

  const auto OutOfRange = Hooks::Read(ScriptValue::Number(1e39), Float);
  Check(!OutOfRange.Accepted && OutOfRange.Diagnostic.Failure ==
                                    StructuredFailure::SinglePrecisionRange,
        "a value outside the single-precision range is refused");
  Check(ArgumentMessage(OutOfRange.Diagnostic)
                .find("expected single-precision range") != std::string::npos,
        "the single-precision refusal names the permitted range");

  const auto Mismatch = Hooks::Read(ScriptValue::Text("half"), Float);
  Check(!Mismatch.Accepted &&
            Mismatch.Diagnostic.Failure == StructuredFailure::TypeMismatch,
        "float refuses a string");
  Check(ArgumentMessage(Mismatch.Diagnostic) ==
            "Callable 'Studio.Apply' argument 1 expected single-precision "
            "number but received string.",
        "a float mismatch keeps the foundation wording shape");

  const auto Written = Hooks::Write(Scalar(Value(2.5)), Float);
  Check(Written.Accepted && Written.PublishedCount == 1 &&
            Written.RoundTripMatches,
        "float round-trips through the registry");

  const auto Refused = Hooks::Write(Scalar(Value(1e39)), Float);
  Check(!Refused.Accepted && Refused.StackDepthDelta == 0,
        "a refused float publication leaves the stack untouched");
}

void CheckTextTypes() {
  for (const FixedTypeKey Key :
       {FixedTypeKey::StringView, FixedTypeKey::CString}) {
    const TypeDescriptor Text = Fixed(Key);

    const auto Accepted = Hooks::Read(ScriptValue::Text("abc"), Text);
    Check(Accepted.Accepted && Accepted.ConvertedValue.ScalarValue() &&
              *Accepted.ConvertedValue.ScalarValue() ==
                  Value(std::string("abc")),
          "a text type reads a Luau string");

    const auto Mismatch = Hooks::Read(ScriptValue::Number(4), Text);
    Check(!Mismatch.Accepted &&
              Mismatch.Diagnostic.Failure == StructuredFailure::TypeMismatch,
          "a text type refuses a number");

    const auto Written = Hooks::Write(Scalar(Value(std::string("abc"))), Text);
    Check(Written.Accepted && Written.RoundTripMatches,
          "a text type round-trips through the registry");
  }

  // A C string cannot carry an embedded null byte; a string view can.
  const std::string Embedded("a\0b", 3);
  const auto ViewAccepted =
      Hooks::Read(ScriptValue::Text(Embedded), Fixed(FixedTypeKey::StringView));
  Check(ViewAccepted.Accepted, "a string view accepts an embedded null byte");

  const auto CStringRefused =
      Hooks::Read(ScriptValue::Text(Embedded), Fixed(FixedTypeKey::CString));
  Check(!CStringRefused.Accepted && CStringRefused.Diagnostic.Failure ==
                                        StructuredFailure::EmbeddedNullByte,
        "a C string refuses an embedded null byte");
  Check(CStringRefused.Diagnostic.ReceivedCount == 3,
        "the embedded-null refusal reports the received byte count");

  // The inherited foundation string policy is preserved exactly, and the
  // diagnostic reports the received and permitted size.
  const auto AtLimit = Hooks::Read(
      ScriptValue::Text(std::string(MaximumInvocationStringBytes, 'a')),
      Fixed(FixedTypeKey::StringView));
  Check(AtLimit.Accepted, "a string of exactly the permitted size is accepted");

  const auto OverLimit = Hooks::Read(
      ScriptValue::Text(std::string(MaximumInvocationStringBytes + 1, 'a')),
      Fixed(FixedTypeKey::StringView));
  Check(!OverLimit.Accepted &&
            OverLimit.Diagnostic.Failure == StructuredFailure::StringTooLong &&
            OverLimit.Diagnostic.ReceivedCount ==
                MaximumInvocationStringBytes + 1 &&
            OverLimit.Diagnostic.PermittedCount == MaximumInvocationStringBytes,
        "the inherited 1,048,576-byte policy reports received and permitted "
        "size");
  Check(ArgumentMessage(OverLimit.Diagnostic) ==
            "Callable 'Studio.Apply' argument 1 received 1048577 string bytes; "
            "maximum is 1048576.",
        "the string-policy refusal keeps the foundation wording");
}

void CheckNullType() {
  const TypeDescriptor Null = Fixed(FixedTypeKey::Null);

  const auto Accepted = Hooks::Read(ScriptValue::Nil(), Null);
  Check(Accepted.Accepted && Accepted.ConvertedValue.IsNull(),
        "null reads nil");

  const auto Mismatch = Hooks::Read(ScriptValue::Number(0), Null);
  Check(!Mismatch.Accepted &&
            Mismatch.Diagnostic.Failure == StructuredFailure::TypeMismatch,
        "null refuses a number");

  const auto Written = Hooks::Write(StructuredValue::Null(), Null);
  Check(Written.Accepted && Written.PublishedCount == 1 &&
            Written.RoundTripMatches,
        "null publishes exactly one nil");
}

// -- optional ---------------------------------------------------------------

void CheckOptional() {
  const TypeDescriptor Optional =
      Luna::Detail::OptionalTypeOf(Fixed(FixedTypeKey::Int32));

  const auto Absent = Hooks::Read(ScriptValue::Nil(), Optional);
  Check(Absent.Accepted && Absent.ConvertedValue.IsNull(),
        "an optional maps explicit nil to the absent value");

  const auto Present = Hooks::Read(ScriptValue::Number(7), Optional);
  Check(Present.Accepted && Present.ConvertedValue.ScalarValue() &&
            *Present.ConvertedValue.ScalarValue() == Value(7),
        "a present optional uses the canonical inner conversion");

  const auto Fractional = Hooks::Read(ScriptValue::Number(3.5), Optional);
  Check(!Fractional.Accepted &&
            ArgumentMessage(Fractional.Diagnostic) ==
                "Callable 'Studio.Apply' argument 1 expected an integral value "
                "but received 3.5.",
        "a present optional keeps the exact inner integer classification");

  const auto WrittenAbsent = Hooks::Write(StructuredValue::Null(), Optional);
  Check(WrittenAbsent.Accepted && WrittenAbsent.PublishedCount == 1 &&
            WrittenAbsent.RoundTripMatches,
        "an absent optional publishes nil");

  const auto WrittenPresent = Hooks::Write(Scalar(Value(11)), Optional);
  Check(WrittenPresent.Accepted && WrittenPresent.RoundTripMatches,
        "a present optional publishes its inner value");
}

// -- sequences, fixed arrays, pairs, tuples ---------------------------------

void CheckSequences() {
  const TypeDescriptor Sequence =
      Luna::Detail::SequenceTypeOf(Fixed(FixedTypeKey::Int32));

  const auto Empty = Hooks::Read(ScriptValue::Array({}), Sequence);
  Check(Empty.Accepted && Empty.ConvertedValue.Kind() == StructuredKind::List &&
            Empty.ConvertedValue.Size() == 0,
        "an empty table reads as an empty sequence");

  const auto Ordered = Hooks::Read(
      ScriptValue::Array({ScriptValue::Number(4), ScriptValue::Number(5),
                          ScriptValue::Number(6)}),
      Sequence);
  Check(Ordered.Accepted &&
            HasSameStructure(Ordered.ConvertedValue, IntegerList({4, 5, 6})),
        "a sequence preserves element order");

  const auto Hole = Hooks::Read(
      ScriptValue::Table({ScriptValue::Number(1), ScriptValue::Number(10),
                          ScriptValue::Number(3), ScriptValue::Number(30)}),
      Sequence);
  Check(!Hole.Accepted &&
            Hole.Diagnostic.Failure == StructuredFailure::ForeignTableKey,
        "a hole in the element positions is refused");

  const auto Foreign = Hooks::Read(
      ScriptValue::Table({ScriptValue::Text("name"), ScriptValue::Number(1)}),
      Sequence);
  Check(!Foreign.Accepted &&
            Foreign.Diagnostic.Failure == StructuredFailure::ForeignTableKey,
        "a foreign table key is refused");

  const auto NestedFailure = Hooks::Read(
      ScriptValue::Array({ScriptValue::Number(1), ScriptValue::Text("two")}),
      Sequence);
  Check(!NestedFailure.Accepted && NestedFailure.Diagnostic.Path == "[2]",
        "a nested sequence failure reports its one-based element path");
  Check(ArgumentMessage(NestedFailure.Diagnostic, 2) ==
            "Callable 'Studio.Apply' argument 2[2] expected signed 32-bit "
            "integer but received string.",
        "a nested failure names the callable, argument position, and path");
  Check(NestedFailure.ConvertedValue.Size() == 0,
        "a refused sequence stages no partial aggregate");

  const auto Written = Hooks::Write(IntegerList({1, 2, 3}), Sequence);
  Check(Written.Accepted && Written.PublishedCount == 1 &&
            Written.RoundTripMatches,
        "a sequence publishes one table and round-trips");

  // Element counts are a shape rule, never a configured cap.
  std::vector<int> Many;
  for (int Element = 0; Element < 300; ++Element)
    Many.push_back(Element);
  const auto Large = Hooks::Write(IntegerList(Many), Sequence);
  Check(Large.Accepted && Large.RoundTripMatches,
        "a sequence imposes no element-count cap");
}

void CheckFixedArrays() {
  const TypeDescriptor Array =
      Luna::Detail::FixedArrayTypeOf(Fixed(FixedTypeKey::Int32), 3);

  const auto Exact = Hooks::Read(
      ScriptValue::Array({ScriptValue::Number(1), ScriptValue::Number(2),
                          ScriptValue::Number(3)}),
      Array);
  Check(Exact.Accepted && Exact.ConvertedValue.Size() == 3,
        "a fixed array accepts exactly its extent");

  const auto Short = Hooks::Read(
      ScriptValue::Array({ScriptValue::Number(1), ScriptValue::Number(2)}),
      Array);
  Check(!Short.Accepted &&
            Short.Diagnostic.Failure ==
                StructuredFailure::ElementCountMismatch &&
            Short.Diagnostic.ReceivedCount == 2 &&
            Short.Diagnostic.PermittedCount == 3,
        "a fixed array reports the received and permitted element count");
  Check(ArgumentMessage(Short.Diagnostic) ==
            "Callable 'Studio.Apply' argument 1 received 2 elements; array of "
            "3 signed 32-bit integer permits 3.",
        "the array shape refusal names both counts");

  const auto WrittenShort = Hooks::Write(IntegerList({1, 2}), Array);
  Check(!WrittenShort.Accepted && WrittenShort.StackDepthDelta == 0,
        "a fixed array refuses a wrong-sized staged value before publishing");
}

void CheckPairsAndTuples() {
  const TypeDescriptor Pair = Luna::Detail::PairTypeOf(
      Fixed(FixedTypeKey::Int32), Fixed(FixedTypeKey::String));
  const auto ReadPair = Hooks::Read(
      ScriptValue::Array({ScriptValue::Number(3), ScriptValue::Text("three")}),
      Pair);
  Check(ReadPair.Accepted && ReadPair.ConvertedValue.Size() == 2,
        "a pair reads its two ordered elements");

  const auto PairArity =
      Hooks::Read(ScriptValue::Array({ScriptValue::Number(3)}), Pair);
  Check(!PairArity.Accepted && PairArity.Diagnostic.Failure ==
                                   StructuredFailure::ElementCountMismatch,
        "a pair requires exactly two elements");

  std::vector<TypeDescriptor> TupleChildren{Fixed(FixedTypeKey::Int32),
                                            Fixed(FixedTypeKey::Boolean),
                                            Fixed(FixedTypeKey::Double)};
  const TypeDescriptor Tuple = Luna::Detail::TupleTypeOf(TupleChildren);
  const auto ReadTuple = Hooks::Read(
      ScriptValue::Array({ScriptValue::Number(1), ScriptValue::Boolean(true),
                          ScriptValue::Number(2.5)}),
      Tuple);
  Check(ReadTuple.Accepted && ReadTuple.ConvertedValue.Size() == 3,
        "a tuple reads its ordered elements");

  const auto TupleFailure = Hooks::Read(
      ScriptValue::Array({ScriptValue::Number(1), ScriptValue::Boolean(true),
                          ScriptValue::Text("half")}),
      Tuple);
  Check(!TupleFailure.Accepted && TupleFailure.Diagnostic.Path == "[3]",
        "a tuple reports the position of its first failing element");
}

// -- associative maps -------------------------------------------------------

void CheckMaps() {
  const TypeDescriptor Map = Luna::Detail::MapTypeOf(
      Fixed(FixedTypeKey::String), Fixed(FixedTypeKey::Int32));

  const auto Read = Hooks::Read(
      ScriptValue::Table({ScriptValue::Text("beta"), ScriptValue::Number(2),
                          ScriptValue::Text("alpha"), ScriptValue::Number(1)}),
      Map);
  Check(Read.Accepted && Read.ConvertedValue.Kind() == StructuredKind::Map &&
            Read.ConvertedValue.Size() == 2,
        "a map reads its key and value pairs");
  Check(Read.Accepted && Read.ConvertedValue.KeyAt(0) &&
            Read.ConvertedValue.KeyAt(0)->ScalarValue() &&
            *Read.ConvertedValue.KeyAt(0)->ScalarValue() ==
                Value(std::string("alpha")),
        "map entries are ordered canonically rather than by traversal order");

  const auto ValueFailure =
      Hooks::Read(ScriptValue::Table(
                      {ScriptValue::Text("alpha"), ScriptValue::Text("one")}),
                  Map);
  Check(!ValueFailure.Accepted && ValueFailure.Diagnostic.Path == "[\"alpha\"]",
        "a failing map value reports its complete key path");

  // The canonical key path plus the `.Key` field is what makes a key failure
  // distinguishable from a value failure at the same entry.
  const auto KeyFailure = Hooks::Read(
      ScriptValue::Table({ScriptValue::Number(4), ScriptValue::Number(1)}),
      Map);
  Check(!KeyFailure.Accepted && KeyFailure.Diagnostic.Path == "[4].Key",
        "a failing map key reports its key path and the key field");
  Check(ArgumentMessage(KeyFailure.Diagnostic, 2) ==
            "Callable 'Studio.Apply' argument 2[4].Key expected string but "
            "received number.",
        "a nested map key failure produces the documented path form");

  const TypeDescriptor NumberKeyed = Luna::Detail::MapTypeOf(
      Fixed(FixedTypeKey::Int32), Fixed(FixedTypeKey::Int32));
  const auto UnsupportedKey = Hooks::Read(
      ScriptValue::Table({ScriptValue::Array({}), ScriptValue::Number(1)}),
      NumberKeyed);
  Check(!UnsupportedKey.Accepted && UnsupportedKey.Diagnostic.Failure ==
                                        StructuredFailure::UnsupportedMapKey,
        "a table, function, or userdata map key is refused");

  std::vector<StructuredValue> Entries;
  Entries.push_back(Scalar(Value(std::string("alpha"))));
  Entries.push_back(Scalar(Value(1)));
  Entries.push_back(Scalar(Value(std::string("beta"))));
  Entries.push_back(Scalar(Value(2)));
  const auto Written =
      Hooks::Write(StructuredValue::Map(std::move(Entries)), Map);
  Check(Written.Accepted && Written.PublishedCount == 1 &&
            Written.RoundTripMatches,
        "a map publishes one table and round-trips");
}

// -- nesting ----------------------------------------------------------------

void CheckNestedAggregates() {
  const TypeDescriptor Nested =
      Luna::Detail::SequenceTypeOf(Luna::Detail::MapTypeOf(
          Fixed(FixedTypeKey::String),
          Luna::Detail::OptionalTypeOf(Fixed(FixedTypeKey::Int32))));

  const auto Accepted = Hooks::Read(
      ScriptValue::Array(
          {ScriptValue::Table({ScriptValue::Text("a"), ScriptValue::Number(1)}),
           ScriptValue::Table({ScriptValue::Text("b"), ScriptValue::Nil()})}),
      Nested);
  Check(Accepted.Accepted && Accepted.ConvertedValue.Size() == 2,
        "a nested aggregate converts through the same registry");

  const auto Failure = Hooks::Read(
      ScriptValue::Array(
          {ScriptValue::Table({ScriptValue::Text("a"), ScriptValue::Number(1)}),
           ScriptValue::Table(
               {ScriptValue::Text("b"), ScriptValue::Text("two")})}),
      Nested);
  Check(!Failure.Accepted && Failure.Diagnostic.Path == "[2][\"b\"]",
        "a deeply nested failure reports the complete element and key path");
  Check(ArgumentMessage(Failure.Diagnostic, 3) ==
            "Callable 'Studio.Apply' argument 3[2][\"b\"] expected signed "
            "32-bit integer but received string.",
        "one atomic diagnostic names the callable, position, and full path");

  // Nesting has no configured depth: ten optional/sequence layers convert.
  TypeDescriptor Deep = Fixed(FixedTypeKey::Int32);
  for (int Layer = 0; Layer < 5; ++Layer)
    Deep = Luna::Detail::SequenceTypeOf(Luna::Detail::OptionalTypeOf(Deep));

  ScriptValue DeepValue = ScriptValue::Number(9);
  for (int Layer = 0; Layer < 5; ++Layer)
    DeepValue = ScriptValue::Array({DeepValue});
  const auto DeepRead = Hooks::Read(DeepValue, Deep);
  Check(DeepRead.Accepted, "nesting depth is not capped");
}

// -- return shapes ----------------------------------------------------------

void CheckReturnShapes() {
  const auto Nothing =
      Hooks::PublishReturn(StructuredValue::Null(), Fixed(FixedTypeKey::Void));
  Check(Nothing.Accepted && Nothing.PublishedCount == 0 &&
            Nothing.StackDepthDelta == 0,
        "void publishes zero values");

  const auto Scalar32 =
      Hooks::PublishReturn(Scalar(Value(5)), Fixed(FixedTypeKey::Int32));
  Check(Scalar32.Accepted && Scalar32.PublishedCount == 1 &&
            Scalar32.RoundTripMatches,
        "a scalar return publishes one value");

  const TypeDescriptor Pair = Luna::Detail::PairTypeOf(
      Fixed(FixedTypeKey::Int32), Fixed(FixedTypeKey::String));
  std::vector<StructuredValue> PairElements;
  PairElements.push_back(Scalar(Value(7)));
  PairElements.push_back(Scalar(Value(std::string("seven"))));
  const auto PairReturn = Hooks::PublishReturn(
      StructuredValue::List(std::move(PairElements)), Pair);
  Check(PairReturn.Accepted && PairReturn.PublishedCount == 2 &&
            PairReturn.RoundTripMatches,
        "a pair return publishes two ordered values");

  const TypeDescriptor Pack = Luna::Detail::ReturnPackTypeOf(
      {Fixed(FixedTypeKey::Boolean), Fixed(FixedTypeKey::Int32),
       Fixed(FixedTypeKey::Double)});
  std::vector<StructuredValue> PackElements;
  PackElements.push_back(Scalar(Value(true)));
  PackElements.push_back(Scalar(Value(2)));
  PackElements.push_back(Scalar(Value(0.5)));
  const auto PackReturn = Hooks::PublishReturn(
      StructuredValue::List(std::move(PackElements)), Pack);
  Check(PackReturn.Accepted && PackReturn.PublishedCount == 3 &&
            PackReturn.RoundTripMatches,
        "a return pack publishes its ordered elements");

  // One failing element exposes zero return values and one diagnostic.
  const TypeDescriptor Guarded = Luna::Detail::ReturnPackTypeOf(
      {Fixed(FixedTypeKey::Int32), Fixed(FixedTypeKey::CString)});
  std::vector<StructuredValue> Partial;
  Partial.push_back(Scalar(Value(1)));
  Partial.push_back(Scalar(Value(std::string("a\0b", 3))));
  const auto Refused =
      Hooks::PublishReturn(StructuredValue::List(std::move(Partial)), Guarded);
  Check(!Refused.Accepted && Refused.PublishedCount == 0 &&
            Refused.StackDepthDelta == 0,
        "a failed return element publishes nothing");
  Check(Refused.Diagnostic.Failure == StructuredFailure::EmbeddedNullByte &&
            Refused.Diagnostic.Position == 2,
        "the failed return element reports its one-based return position");

  // A nested pair inside an aggregate stays one table rather than two values.
  const TypeDescriptor NestedPairs = Luna::Detail::SequenceTypeOf(Pair);
  std::vector<StructuredValue> Rows;
  std::vector<StructuredValue> Row;
  Row.push_back(Scalar(Value(1)));
  Row.push_back(Scalar(Value(std::string("one"))));
  Rows.push_back(StructuredValue::List(std::move(Row)));
  const auto NestedReturn =
      Hooks::PublishReturn(StructuredValue::List(std::move(Rows)), NestedPairs);
  Check(NestedReturn.Accepted && NestedReturn.PublishedCount == 1 &&
            NestedReturn.RoundTripMatches,
        "a nested pair publishes one table inside its parent aggregate");

  // A failed nested element leaves no partial table behind.
  const TypeDescriptor Strings =
      Luna::Detail::SequenceTypeOf(Fixed(FixedTypeKey::CString));
  std::vector<StructuredValue> Texts;
  Texts.push_back(Scalar(Value(std::string("ok"))));
  Texts.push_back(Scalar(Value(std::string("a\0b", 3))));
  const auto NoPartialTable =
      Hooks::Write(StructuredValue::List(std::move(Texts)), Strings);
  Check(!NoPartialTable.Accepted && NoPartialTable.PublishedCount == 0 &&
            NoPartialTable.StackDepthDelta == 0 &&
            NoPartialTable.Diagnostic.Path == "[2]",
        "a failed aggregate element publishes no partial table");
}

// -- argument packs ---------------------------------------------------------

void CheckArgumentPacks() {
  const TypeDescriptor Homogeneous =
      Luna::Detail::ArgumentPackTypeOf({Fixed(FixedTypeKey::Int32)});

  const auto Accepted = Hooks::ReadPack(
      {ScriptValue::Number(1), ScriptValue::Number(2), ScriptValue::Number(3)},
      Homogeneous, 2);
  Check(Accepted.Accepted &&
            HasSameStructure(Accepted.ConvertedValue, IntegerList({1, 2, 3})),
        "a homogeneous argument pack accepts any supplied count");
  Check(Accepted.StackDepthDelta == 0,
        "reading an argument pack leaves the stack untouched");

  const auto None = Hooks::ReadPack({}, Homogeneous, 2);
  Check(None.Accepted && None.ConvertedValue.Size() == 0,
        "a homogeneous argument pack accepts zero arguments");

  const auto Failure =
      Hooks::ReadPack({ScriptValue::Number(1), ScriptValue::Text("two"),
                       ScriptValue::Number(3)},
                      Homogeneous, 2);
  Check(!Failure.Accepted && Failure.Diagnostic.Position == 3,
        "a variadic failure reports the one-based call argument position");
  Check(ArgumentMessage(Failure.Diagnostic, 1) ==
            "Callable 'Studio.Apply' argument 3 expected signed 32-bit integer "
            "but received string.",
        "the variadic position wins over the caller's position");

  const TypeDescriptor Positional = Luna::Detail::ArgumentPackTypeOf(
      {Fixed(FixedTypeKey::Int32), Fixed(FixedTypeKey::String)});
  const auto Arity = Hooks::ReadPack(
      {ScriptValue::Number(1), ScriptValue::Text("a"), ScriptValue::Number(2)},
      Positional, 1);
  Check(!Arity.Accepted &&
            Arity.Diagnostic.Failure == StructuredFailure::ElementCountMismatch,
        "a positional argument pack requires its declared element count");

  const auto Nested = Hooks::ReadPack(
      {ScriptValue::Array({ScriptValue::Number(1), ScriptValue::Text("x")})},
      Luna::Detail::ArgumentPackTypeOf(
          {Luna::Detail::SequenceTypeOf(Fixed(FixedTypeKey::Int32))}),
      4);
  Check(!Nested.Accepted && Nested.Diagnostic.Position == 4 &&
            Nested.Diagnostic.Path == "[2]",
        "a variadic failure reports both the call position and nested path");
}

// -- enumerations and registered classes ------------------------------------

void CheckEnumerationsAndClasses() {
  const StableTypeKey EnumKey("Studio.Alignment");
  const TypeDescriptor Enumeration = TypeDescriptor::ForEnumeration(EnumKey);
  std::vector<TypeRecord> EnumerationRecords;
  EnumerationRecords.push_back(
      Luna::Detail::DeclareEnumerationTypeRecord(EnumKey, "Studio.Alignment"));
  Check(EnumerationRecords.front().IsComplete(),
        "an enumeration declaration is complete");

  const auto ReadEnum =
      Hooks::Read(ScriptValue::Number(2), Enumeration, EnumerationRecords);
  Check(ReadEnum.Accepted && ReadEnum.ConvertedValue.ScalarValue() &&
            *ReadEnum.ConvertedValue.ScalarValue() == Value(2),
        "an enumeration reads its underlying integral value");

  const auto EnumMismatch =
      Hooks::Read(ScriptValue::Text("Left"), Enumeration, EnumerationRecords);
  Check(!EnumMismatch.Accepted &&
            ArgumentMessage(EnumMismatch.Diagnostic) ==
                "Callable 'Studio.Apply' argument 1 expected Studio.Alignment "
                "but received string.",
        "an enumeration mismatch names the enumeration");

  const auto EnumFractional =
      Hooks::Read(ScriptValue::Number(1.5), Enumeration, EnumerationRecords);
  Check(!EnumFractional.Accepted && EnumFractional.Diagnostic.Failure ==
                                        StructuredFailure::IntegerFractional,
        "an enumeration keeps the foundation integer classification");

  const auto WrittenEnum =
      Hooks::Write(Scalar(Value(3)), Enumeration, EnumerationRecords);
  Check(WrittenEnum.Accepted && WrittenEnum.PublishedCount == 1,
        "an enumeration publishes its underlying value");

  // An enumeration nests like any other leaf.
  std::vector<TypeRecord> NestedRecords = EnumerationRecords;
  const auto Sequence = Hooks::Read(
      ScriptValue::Array({ScriptValue::Number(1), ScriptValue::Text("two")}),
      Luna::Detail::SequenceTypeOf(Enumeration), NestedRecords);
  Check(!Sequence.Accepted && Sequence.Diagnostic.Path == "[2]",
        "an enumeration inside an aggregate reports its element path");

  const StableTypeKey ClassKey("Studio.Vector");
  const TypeDescriptor Class = TypeDescriptor::ForClass(ClassKey);
  std::vector<TypeRecord> ClassRecords;
  ClassRecords.push_back(
      Luna::Detail::DeclareClassTypeRecord(ClassKey, "Studio.Vector"));
  Check(ClassRecords.front().IsComplete() &&
            ClassRecords.front().Representation ==
                Luna::Detail::LuauRepresentation::Userdata,
        "a registered class is described as userdata");

  const std::shared_ptr<const TypeGeneration> WithClass =
      Hooks::GenerationFor(Luna::Detail::SequenceTypeOf(Class), ClassRecords);
  Check(WithClass != nullptr &&
            WithClass->IsAvailableForRead(Luna::Detail::SequenceTypeOf(Class)),
        "an aggregate over a registered class is describable");

  // A class handle is only readable through the State that registered the
  // class: the metatable identity and origin identity an access validates
  // against live there. A generation assembled without one refuses
  // deterministically instead of trusting the value it was handed.
  const auto ClassRead = Hooks::Read(ScriptValue::Nil(), Class, ClassRecords);
  Check(!ClassRead.Accepted &&
            ClassRead.Diagnostic.Failure == StructuredFailure::UnavailableType,
        "a class handle read without its registered class refuses "
        "deterministically");
}

// -- declaration closure ----------------------------------------------------

void CheckDeclarationClosure() {
  const std::shared_ptr<const TypeGeneration> BuiltIn =
      Luna::Detail::BuiltInTypeGeneration();
  Check(BuiltIn != nullptr && BuiltIn->Size() == 9,
        "the built-in generation adds float, string view, C string, and null");
  Check(BuiltIn != nullptr &&
            BuiltIn->IsAvailableForRead(Fixed(FixedTypeKey::Float)) &&
            BuiltIn->IsAvailableForWrite(Fixed(FixedTypeKey::Null)),
        "the planned built-in scalars are available in both directions");

  const TypeDescriptor Nested = Luna::Detail::MapTypeOf(
      Fixed(FixedTypeKey::String),
      Luna::Detail::SequenceTypeOf(
          Luna::Detail::OptionalTypeOf(Fixed(FixedTypeKey::Float))));

  std::vector<TypeRecord> Declared;
  TypeDescriptor Blocking;
  Check(Luna::Detail::DeclareStructuralTypes(*BuiltIn, Nested, Declared,
                                             Blocking) ==
            StructuralDeclarationStatus::Declared,
        "one call declares the whole closure of a nested type");
  Check(Declared.size() == 3,
        "only the types the registry did not already describe are declared");
  Check(!Declared.empty() && Declared.back().Descriptor == Nested,
        "children are declared before the parent that nests them");

  // The declarations enter an ordinary generation without conflicting.
  Luna::Detail::TypeDeclarationStatus Status =
      Luna::Detail::TypeDeclarationStatus::Acceptable;
  const std::shared_ptr<const TypeGeneration> Extended =
      TypeGeneration::Derive(*BuiltIn, Declared, Status);
  Check(Extended != nullptr &&
            Status == Luna::Detail::TypeDeclarationStatus::Acceptable,
        "a structural closure derives the next generation");
  Check(Extended != nullptr && Extended->IsAvailableForRead(Nested) &&
            Extended->IsAvailableForWrite(Nested),
        "the derived generation makes the nested type convertible");
  Check(Extended != nullptr && Extended->PublicNameOf(Nested) ==
                                   "map of string to sequence of optional "
                                   "single-precision number",
        "a structural public name is composed from its children");

  // An unregistered class leaf refuses instead of being invented.
  std::vector<TypeRecord> Refused;
  TypeDescriptor BlockingLeaf;
  Check(Luna::Detail::DeclareStructuralTypes(
            *BuiltIn,
            Luna::Detail::SequenceTypeOf(
                TypeDescriptor::ForClass(StableTypeKey("Studio.Unknown"))),
            Refused,
            BlockingLeaf) == StructuralDeclarationStatus::UnavailableLeaf,
        "an unregistered class leaf is refused");
  Check(Refused.empty(), "a refused closure declares nothing");

  TypeDescriptor BlockingUnsupported;
  std::vector<TypeRecord> None;
  Check(Luna::Detail::DeclareStructuralTypes(*BuiltIn,
                                             TypeDescriptor::Unsupported(),
                                             None, BlockingUnsupported) ==
            StructuralDeclarationStatus::UnsupportedDescriptor,
        "an unsupported descriptor is refused");
}

} // namespace

int RunStructuralConverterTests() {
  FailureCount = 0;
  CheckSinglePrecision();
  CheckTextTypes();
  CheckNullType();
  CheckOptional();
  CheckSequences();
  CheckFixedArrays();
  CheckPairsAndTuples();
  CheckMaps();
  CheckNestedAggregates();
  CheckReturnShapes();
  CheckArgumentPacks();
  CheckEnumerationsAndClasses();
  CheckDeclarationClosure();
  return FailureCount == 0 ? 0 : 1;
}
