// clang-format off
#include <luna/binding/conversion.hpp>
#include <luna/binding/value.hpp>

#include "state/type/conversion_frame.hpp"

#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
// clang-format on

namespace {

using Luna::ConversionContext;
using Luna::ConversionDirection;
using Luna::ConversionProbe;
using Luna::ConversionRank;
using Luna::ConversionResult;
using Luna::ConversionStatus;
using Luna::OwnedValue;
using Luna::ValueCategory;
using Luna::ValuePack;
using Luna::ValueReservation;
using Luna::ValueView;
using Luna::WriteResult;
using Luna::WriteStatus;
using Frame = Luna::Detail::ConversionFrame;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "conversion boundary check failed: " << Description << '\n';
}

static_assert(std::is_trivially_copyable_v<ValueView>,
              "A transient view must remain a plain Luna-owned token.");
static_assert(std::is_trivially_copyable_v<ConversionContext>,
              "A conversion context must remain a plain Luna-owned token.");
static_assert(std::is_default_constructible_v<ValueView> &&
                  std::is_default_constructible_v<ConversionContext>,
              "An inert view and context must be ordinary default values.");

template <class Type>
concept ReservesThroughConstContext =
    requires(const Type &Context, const ValueReservation &Request) {
      Context.Reserve(Request);
    };

template <class Type>
concept PublishesThroughConstContext =
    requires(const Type &Context, const OwnedValue &Published) {
      Context.Publish(Published);
    };

template <class Type>
concept PublishesPackThroughConstContext =
    requires(const Type &Context, const ValuePack &Published) {
      Context.PublishPack(Published);
    };

static_assert(!ReservesThroughConstContext<ConversionContext>,
              "Reserving resources must be unavailable through a probe.");
static_assert(!PublishesThroughConstContext<ConversionContext>,
              "Publishing a value must be unavailable through a probe.");
static_assert(!PublishesPackThroughConstContext<ConversionContext>,
              "Publishing a pack must be unavailable through a probe.");

[[nodiscard]] OwnedValue NestedSource() {
  OwnedValue Leaf = OwnedValue::Table();
  Leaf.SetField("Key", OwnedValue::Text("text"));

  OwnedValue Source = OwnedValue::Table();
  Source.Append(OwnedValue::Number(1.0));
  Source.Append(OwnedValue::Boolean(true));
  Source.Append(OwnedValue::Text("three"));
  Source.Append(std::move(Leaf));
  Source.SetField("Name", OwnedValue::Text("nested"));
  return Source;
}

struct Point final {
  double X = 0.0;
  double Y = 0.0;
};

} // namespace

namespace Luna {

template <> class TypeConverter<Point> {
public:
  [[nodiscard]] ConversionProbe Probe(ValueView Source,
                                      const ConversionContext &Context) const {
    if (!Source.IsTable())
      return RejectedProbe(Context.Describe("expected a table of X and Y"));
    if (!Source.Field("X").ToNumber() || !Source.Field("Y").ToNumber())
      return RejectedProbe(Context.Describe("expected numeric X and Y fields"));
    return ViableProbe(ConversionRank::SafeBuiltIn);
  }

  [[nodiscard]] ConversionResult<Point> Read(ValueView Source,
                                             ConversionContext &Context) const {
    ConversionResult<Point> Result;
    const std::optional<double> X = Source.Field("X").ToNumber();
    const std::optional<double> Y = Source.Field("Y").ToNumber();
    if (!X || !Y) {
      Result.Status = ConversionStatus::TypeMismatch;
      Result.Diagnostic = Context.Describe("expected numeric X and Y fields");
      return Result;
    }
    Result.Status = ConversionStatus::Success;
    Result.ConvertedValue = Point{*X, *Y};
    return Result;
  }

  [[nodiscard]] WriteResult Write(const Point &Source,
                                  ConversionContext &Context) const {
    OwnedValue Published = OwnedValue::Table();
    Published.SetField("X", OwnedValue::Number(Source.X));
    Published.SetField("Y", OwnedValue::Number(Source.Y));
    if (const WriteResult Reserved =
            Context.Reserve(Published.RequiredReservation());
        !Reserved.IsSuccess())
      return Reserved;
    return Context.Publish(Published);
  }
};

} // namespace Luna

namespace {

static_assert(Luna::ConversionCapable<Point>,
              "A converter supplying separated probe, read, and write "
              "operations must satisfy the boundary concept.");

void CheckTransientViewsExposeShapeOnly() {
  const OwnedValue Source = NestedSource();
  Frame Reading(ConversionDirection::Read, "Studio.Draw", 2);
  const ValueView Root = Reading.Open(Source);

  Check(Root.IsActive(), "an open frame hands out an active view");
  Check(Root.Kind() == ValueCategory::Table, "the root view reports a table");
  Check(Root.Size() == 4, "the root view reports its element count");
  Check(Root.FieldCount() == 1, "the root view reports its field count");
  Check(Root.Element(0).ToNumber() == 1.0, "an element view reads its number");
  Check(Root.Element(1).ToBoolean() == true,
        "an element view reads its boolean");
  Check(Root.Element(2).ToText() == std::string("three"),
        "an element view reads its text");
  Check(Root.Element(2).ByteCount() == 5,
        "a string view reports its byte count");
  Check(Root.HasField("Name") && !Root.HasField("Missing"),
        "field presence is answered from the frame copy");
  Check(Root.FieldName(0) == std::string_view("Name"),
        "field names stay in canonical order");

  const ValueView Nested = Root.Element(3).Field("Key");
  Check(Nested.ToText() == std::string("text"),
        "a nested field view reads its text");
  Check(Nested.Path() == std::string("argument 2[4].Key"),
        "a nested view reports its complete one-based path");
  Check(Reading.Describe(0, "expected a table") ==
            std::string("Callable 'Studio.Draw' argument 2 expected a table."),
        "a diagnostic names the callable, the position, and the reason");

  Check(!Root.Element(4).IsActive() && Root.Element(4).IsNil() == false,
        "an out-of-range element view is inert");
  Check(!Root.Field("Missing").IsActive(), "an unknown field view is inert");

  Check(Root.ToOwned() == Source,
        "copying a view out of the frame reproduces the value");
}

void CheckRetainedViewsBecomeInert() {
  ValueView Retained;
  ConversionContext RetainedContext;
  {
    Frame Ending(ConversionDirection::Write, "Studio.Make", 1);
    Retained = Ending.Open(OwnedValue::Text("value"));
    RetainedContext = Ending.CommitContext();
    Check(Retained.IsActive() && RetainedContext.IsActive(),
          "a live frame answers its own tokens");
  }

  Luna::Detail::ResetConversionBoundaryDiagnostics();
  Check(!Retained.IsActive(), "a retained view stops being active");
  Check(Retained.Kind() == ValueCategory::None,
        "a retained view reports no category");
  Check(!Retained.ToText().has_value(), "a retained view reads no text");
  Check(Retained.Size() == 0 && Retained.FieldCount() == 0,
        "a retained view reports no children");
  Check(Retained.ToOwned() == OwnedValue::Nil(),
        "copying a retained view produces nil");
  Check(Retained.Path().empty(), "a retained view reports no path");
  Check(!RetainedContext.IsActive(), "a retained context stops being active");

  const WriteResult Late = RetainedContext.Publish(OwnedValue::Number(1.0));
  Check(Late.Status == WriteStatus::InactiveContext,
        "publishing through a retained context is refused");
  Check(Luna::Detail::ExpiredConversionAccessCount() > 0,
        "every access through an ended frame is recorded");
}

void CheckProbesCannotCommitOrMutate() {
  Frame Writing(ConversionDirection::Write, "Studio.Make", 1);
  const ValueView Root = Writing.Open(OwnedValue::Table());
  Luna::Detail::ResetConversionBoundaryDiagnostics();

  const ConversionContext Probing = Writing.ProbeContext();
  Check(Probing.IsProbing(), "a probing context identifies itself");
  Check(!Writing.CommitContext().IsProbing(),
        "a committing context is separated from probing");

  ConversionContext Escaped = Probing;
  const WriteResult Reserved = Escaped.Reserve(ValueReservation{});
  Check(Reserved.Status == WriteStatus::ProbeViolation,
        "a probe cannot reserve writer resources");
  Check(!Writing.HasReservation(),
        "a refused probe reservation changes no frame state");

  const WriteResult Written = Escaped.Publish(OwnedValue::Number(1.0));
  Check(Written.Status == WriteStatus::ProbeViolation,
        "a probe cannot publish a value");
  const WriteResult PackWritten = Escaped.PublishPack(ValuePack());
  Check(PackWritten.Status == WriteStatus::ProbeViolation,
        "a probe cannot publish a pack");
  Check(!Writing.IsPublished(), "a refused probe publishes nothing");

  const ConversionResult<Point> Read = Luna::ReadValue<Point>(Root, Escaped);
  Check(Read.Status == ConversionStatus::ProbeViolation,
        "a probe cannot invoke a committing read");
  const WriteResult Converted =
      Luna::WriteValue<Point>(Point{1.0, 2.0}, Escaped);
  Check(Converted.Status == WriteStatus::ProbeViolation,
        "a probe cannot invoke a committing write");
  Check(!Writing.IsPublished() && !Writing.HasReservation(),
        "an attempted probe conversion leaves the frame untouched");
  Check(Writing.ProbeViolations().size() == 5,
        "every attempted probe violation is recorded on its frame");
  Check(Luna::Detail::ProbeViolationCount() == 5,
        "every attempted probe violation is recorded for the boundary");
}

void CheckProbeRankingIsSeparateAndDeterministic() {
  OwnedValue Table = OwnedValue::Table();
  Table.SetField("X", OwnedValue::Number(3.0));
  Table.SetField("Y", OwnedValue::Number(4.0));

  Frame Reading(ConversionDirection::Read, "Studio.Draw", 1);
  const ValueView Root = Reading.Open(Table);
  const ConversionContext Probing = Reading.ProbeContext();

  const ConversionProbe First = Luna::ProbeValue<Point>(Root, Probing);
  const ConversionProbe Second = Luna::ProbeValue<Point>(Root, Probing);
  Check(First.IsViable && First.Rank == ConversionRank::SafeBuiltIn,
        "a viable probe reports its rank category");
  Check(First.IsViable == Second.IsViable && First.Rank == Second.Rank,
        "probing the same value twice is deterministic");
  Check(Reading.ProbeViolations().empty(),
        "a well-behaved probe records no violation");

  Frame Mismatched(ConversionDirection::Read, "Studio.Draw", 1);
  const ValueView Number = Mismatched.Open(OwnedValue::Number(7.0));
  const ConversionProbe Rejected =
      Luna::ProbeValue<Point>(Number, Mismatched.ProbeContext());
  Check(!Rejected.IsViable, "an unsuitable value is not viable");
  Check(Rejected.Rejection ==
            std::string("Callable 'Studio.Draw' argument 1 expected a table of "
                        "X and Y."),
        "a rejection names the callable, the position, and the reason");

  ConversionContext Committing = Reading.CommitContext();
  const ConversionResult<Point> Result =
      Luna::ReadValue<Point>(Root, Committing);
  Check(Result.IsSuccess() && Result.ConvertedValue->X == 3.0 &&
            Result.ConvertedValue->Y == 4.0,
        "the committing read converts the probed value");
}

void CheckWritersReserveBeforePublishing() {
  OwnedValue Published = OwnedValue::Table();
  Published.Append(OwnedValue::Text("first"));
  Published.SetField("Name", OwnedValue::Text("second"));

  Frame Writing(ConversionDirection::Write, "Studio.Make", 1);
  static_cast<void>(Writing.Open(OwnedValue::Nil()));
  ConversionContext Committing = Writing.CommitContext();
  Check(!Committing.HasReservation() && !Committing.IsPublished(),
        "a fresh write frame has reserved and published nothing");

  const WriteResult Unreserved = Committing.Publish(Published);
  Check(Unreserved.Status == WriteStatus::ReservationMissing,
        "publishing without a reservation is refused");
  Check(!Writing.IsPublished() && !Writing.PublishedResult().has_value(),
        "a refused publication publishes nothing");

  ValueReservation Insufficient;
  Insufficient.ValueCount = 1;
  Insufficient.ByteCount = 1;
  const WriteResult Small = Committing.Reserve(Insufficient);
  Check(Small.IsSuccess(), "a writer may state its reservation");
  Check(Committing.HasReservation() && Committing.Reservation().ValueCount == 1,
        "the frame reports the stated reservation");

  const WriteResult Exceeded = Committing.Publish(Published);
  Check(Exceeded.Status == WriteStatus::ReservationExceeded,
        "publishing more than was reserved is refused");
  Check(!Writing.IsPublished() && !Writing.PublishedResult().has_value(),
        "an exceeded reservation publishes nothing");
  Check(Exceeded.Diagnostic.find("reserved") != std::string::npos,
        "an exceeded reservation reports what was needed and reserved");

  const WriteResult Adequate =
      Committing.Reserve(Published.RequiredReservation());
  Check(Adequate.IsSuccess(), "a writer may restate a complete reservation");
  const WriteResult Success = Committing.Publish(Published);
  Check(Success.IsSuccess(), "a fully reserved value publishes");
  Check(Writing.IsPublished() && Writing.PublishedResult() == Published,
        "publication is exactly the validated value");

  const WriteResult Again = Committing.Publish(OwnedValue::Number(1.0));
  Check(Again.Status == WriteStatus::AlreadyPublished,
        "a frame publishes at most once");
  Check(Writing.PublishedResult() == Published,
        "a refused second publication leaves the published value unchanged");
}

void CheckWriterPolicyAndShapeValidation() {
  {
    Frame Reading(ConversionDirection::Read, "Studio.Draw", 1);
    static_cast<void>(Reading.Open(OwnedValue::Nil()));
    ConversionContext Committing = Reading.CommitContext();
    const WriteResult Wrong = Committing.Publish(OwnedValue::Number(1.0));
    Check(Wrong.Status == WriteStatus::WrongDirection,
          "a reading frame refuses publication");
  }

  {
    OwnedValue Incomplete = OwnedValue::Table();
    Incomplete.SetField("", OwnedValue::Number(1.0));

    Frame Writing(ConversionDirection::Write, "Studio.Make", 1);
    static_cast<void>(Writing.Open(OwnedValue::Nil()));
    ConversionContext Committing = Writing.CommitContext();
    Check(Committing.Reserve(Incomplete.RequiredReservation()).IsSuccess(),
          "an incomplete aggregate can still be reserved for");
    const WriteResult Refused = Committing.Publish(Incomplete);
    Check(Refused.Status == WriteStatus::IncompleteAggregate,
          "an aggregate with an unnamed field is refused");
    Check(!Writing.IsPublished(),
          "a refused aggregate publishes no partial table");
  }

  {
    const std::size_t Permitted = Luna::MaximumConversionStringBytes();
    Check(Permitted == 1'048'576,
          "the boundary reports the inherited string byte policy");

    Frame Writing(ConversionDirection::Write, "Studio.Make", 1);
    static_cast<void>(Writing.Open(OwnedValue::Nil()));
    ConversionContext Committing = Writing.CommitContext();

    ValueReservation TooLarge;
    TooLarge.ValueCount = 1;
    TooLarge.ByteCount = Permitted + 1;
    const WriteResult Rejected = Committing.Reserve(TooLarge);
    Check(Rejected.Status == WriteStatus::PolicyExceeded,
          "reserving beyond the string policy is refused");
    Check(!Writing.HasReservation(),
          "a refused reservation records no resources");

    ValueReservation Allowed;
    Allowed.ValueCount = 1;
    Allowed.ByteCount = Permitted;
    Check(Committing.Reserve(Allowed).IsSuccess(),
          "reserving at the policy boundary is accepted");

    const OwnedValue Oversized =
        OwnedValue::Text(std::string(Permitted + 1, 'a'));
    const WriteResult Refused = Committing.Publish(Oversized);
    Check(Refused.Status == WriteStatus::PolicyExceeded,
          "publishing beyond the string policy is refused");
    Check(Refused.Diagnostic.find(std::to_string(Permitted + 1)) !=
                  std::string::npos &&
              Refused.Diagnostic.find(std::to_string(Permitted)) !=
                  std::string::npos,
          "the policy diagnostic reports received and permitted bytes");
    Check(!Writing.IsPublished(), "an oversized string publishes nothing");
  }
}

void CheckReturnPacksPublishAtomically() {
  ValuePack Pack;
  Pack.Append(OwnedValue::Number(1.0));
  Pack.Append(OwnedValue::Text("second"));
  Pack.Append(OwnedValue::Boolean(false));

  Frame Writing(ConversionDirection::Write, "Studio.Make", 1);
  static_cast<void>(Writing.Open(OwnedValue::Nil()));
  ConversionContext Committing = Writing.CommitContext();

  ValueReservation Insufficient;
  Insufficient.ValueCount = 2;
  Insufficient.ByteCount = 6;
  Check(Committing.Reserve(Insufficient).IsSuccess(),
        "a pack writer states its reservation");
  const WriteResult Exceeded = Committing.PublishPack(Pack);
  Check(Exceeded.Status == WriteStatus::ReservationExceeded,
        "a pack larger than its reservation is refused");
  Check(!Writing.PublishedPack().has_value(),
        "a refused pack publishes no partial return values");

  Check(Committing.Reserve(Pack.RequiredReservation()).IsSuccess(),
        "a complete pack reservation is accepted");
  Check(Committing.PublishPack(Pack).IsSuccess(),
        "a fully reserved pack publishes");
  Check(Writing.PublishedPack().has_value() && *Writing.PublishedPack() == Pack,
        "pack publication preserves ordered multiple values");
  Check(Committing.PublishPack(Pack).Status == WriteStatus::AlreadyPublished,
        "a frame publishes at most one pack");
}

void CheckOwningValuesRetainEverything() {
  Check(OwnedValue::FromValue(Luna::Value(true)).ToBoolean() == true,
        "the foundation boolean converts into an owning value");
  Check(OwnedValue::FromValue(Luna::Value(7)).ToNumber() == 7.0,
        "the foundation integer converts into an owning value");
  Check(OwnedValue::FromValue(Luna::Value(2.5)).ToNumber() == 2.5,
        "the foundation number converts into an owning value");
  Check(OwnedValue::FromValue(Luna::Value(std::string("text"))).ToText() ==
            std::string("text"),
        "the foundation string converts into an owning value");
  Check(!OwnedValue::Nil().ToValue().has_value() &&
            !OwnedValue::Table().ToValue().has_value(),
        "nil and tables have no foundation representation");

  OwnedValue Fields = OwnedValue::Table();
  Fields.SetField("Second", OwnedValue::Number(2.0));
  Fields.SetField("First", OwnedValue::Number(1.0));
  Check(Fields.FieldName(0) == std::string_view("First") &&
            Fields.FieldName(1) == std::string_view("Second"),
        "fields are kept in canonical name order");
  Fields.SetField("First", OwnedValue::Number(3.0));
  Check(Fields.FieldCount() == 2 && Fields.Field("First").ToNumber() == 3.0,
        "setting a known field replaces it in place");

  OwnedValue Nested = OwnedValue::Table();
  Nested.Append(OwnedValue::Text("ab"));
  Nested.SetField("Key", std::move(Fields));
  const ValueReservation Required = Nested.RequiredReservation();
  Check(Required.ValueCount == 5,
        "a reservation counts every nested value once");
  Check(Required.ElementCount == 1 && Required.FieldCount == 3,
        "a reservation counts nested elements and fields");
  Check(Required.TableCount == 2, "a reservation counts nested tables");
  Check(Required.ByteCount == 2 + 3 + 5 + 6,
        "a reservation counts value and field-name bytes");
  Check(Nested.LargestStringByteCount() == 6,
        "the largest single string is what the byte policy applies to");
}

} // namespace

int RunConversionBoundaryTests() {
  FailureCount = 0;
  Luna::Detail::ResetConversionBoundaryDiagnostics();

  CheckTransientViewsExposeShapeOnly();
  CheckRetainedViewsBecomeInert();
  CheckProbesCannotCommitOrMutate();
  CheckProbeRankingIsSeparateAndDeterministic();
  CheckWritersReserveBeforePublishing();
  CheckWriterPolicyAndShapeValidation();
  CheckReturnPacksPublishAtomically();
  CheckOwningValuesRetainEverything();

  Luna::Detail::ResetConversionBoundaryDiagnostics();
  return FailureCount == 0 ? 0 : 1;
}
