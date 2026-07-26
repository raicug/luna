// clang-format off
#include "state/type/structured_conversion.hpp"

#include <luna/binding/value.hpp>

#include "state/type/conversion_outcome.hpp"
#include "state/type/type_record.hpp"
#include "state/userdata/value_exposure.hpp"

#include <cmath>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] std::string PositionText(ConversionDirection Direction,
                                       std::size_t OneBasedPosition) {
  // The receiver owns rank position zero of its call, so it is named instead of
  // numbered; the single value a property or field carries has no call position
  // at all; every other direction keeps exactly the foundation's wording.
  if (Direction == ConversionDirection::Receiver)
    return " receiver";
  if (Direction == ConversionDirection::MemberValue)
    return " value";
  const std::string Label =
      Direction == ConversionDirection::Argument ? " argument " : " return ";
  return Label + std::to_string(OneBasedPosition);
}

[[nodiscard]] std::string SubjectText(const ConversionSubject &Subject) {
  return DescribeConversionSubject(Subject);
}

} // namespace

std::string DescribeConversionSubject(const ConversionSubject &Subject) {
  const std::string Label =
      Subject.Kind == ConversionSubjectKind::Callable ? "Callable" : "Member";
  return Label + " '" + Subject.Name + "'";
}

std::string DescribeConversionSubjectContext(const ConversionSubject &Subject) {
  const std::string Label =
      Subject.Kind == ConversionSubjectKind::Callable ? "callable" : "member";
  return Label + " '" + Subject.Name + "'";
}

ConversionSubject SubjectForCallable(std::string_view Name,
                                     bool IsInstanceMember) {
  ConversionSubject Subject;
  Subject.Kind = IsInstanceMember ? ConversionSubjectKind::Member
                                  : ConversionSubjectKind::Callable;
  Subject.Name = std::string(Name);
  return Subject;
}

std::string_view StructuredKindText(StructuredKind Kind) noexcept {
  switch (Kind) {
  case StructuredKind::Null:
    return "null";
  case StructuredKind::Scalar:
    return "scalar";
  case StructuredKind::List:
    return "list";
  case StructuredKind::Map:
    return "map";
  case StructuredKind::Handle:
    return "handle";
  }
  return "null";
}

StructuredValue StructuredValue::Null() { return StructuredValue(); }

StructuredValue StructuredValue::Scalar(Value Source) {
  StructuredValue Staged;
  Staged.KindValue = StructuredKind::Scalar;
  Staged.ScalarStorage = std::move(Source);
  Staged.HasScalar = true;
  return Staged;
}

StructuredValue StructuredValue::List(std::vector<StructuredValue> Items) {
  StructuredValue Staged;
  Staged.KindValue = StructuredKind::List;
  Staged.ElementValues = std::move(Items);
  return Staged;
}

StructuredValue StructuredValue::Map(std::vector<StructuredValue> Items) {
  StructuredValue Staged;
  Staged.KindValue = StructuredKind::Map;
  if (Items.size() % 2 == 0)
    Staged.ElementValues = std::move(Items);
  return Staged;
}

StructuredValue StructuredValue::Handle(void *Storage, bool PermitsMutation) {
  StructuredValue Staged;

  // A handle without a validated object is no handle at all, so it stays the
  // absent value instead of pretending native code may be reached.
  if (Storage == nullptr)
    return Staged;
  Staged.KindValue = StructuredKind::Handle;
  Staged.HandleStorageValue = Storage;
  Staged.HandleIsMutable = PermitsMutation;
  return Staged;
}

StructuredValue StructuredValue::ExposedHandle(
    void *Storage, bool PermitsMutation,
    std::shared_ptr<const ClassExposureIntent> Intent) {
  // An exposure without the ownership statement that says how the object will
  // be owned is no exposure at all. An exposure of an object that does not
  // exist yet is a different thing entirely: it is a construction, and the
  // semantic allocator protocol inside the statement is what creates the
  // object.
  if (!Intent)
    return StructuredValue();
  if (Storage == nullptr && !DeclaresObjectConstruction(*Intent))
    return StructuredValue();

  StructuredValue Staged;
  Staged.KindValue = StructuredKind::Handle;
  Staged.HandleStorageValue = Storage;
  Staged.HandleIsMutable = PermitsMutation;
  Staged.HandleIntent = std::move(Intent);
  return Staged;
}

const Value *StructuredValue::ScalarValue() const noexcept {
  return HasScalar ? &ScalarStorage : nullptr;
}

void *StructuredValue::HandleStorage() const noexcept {
  return KindValue == StructuredKind::Handle ? HandleStorageValue : nullptr;
}

const ClassExposureIntent *StructuredValue::HandleExposure() const noexcept {
  return KindValue == StructuredKind::Handle ? HandleIntent.get() : nullptr;
}

std::size_t StructuredValue::Size() const noexcept {
  switch (KindValue) {
  case StructuredKind::Null:
  case StructuredKind::Scalar:
  case StructuredKind::Handle:
    return 0;
  case StructuredKind::List:
    return ElementValues.size();
  case StructuredKind::Map:
    return ElementValues.size() / 2;
  }
  return 0;
}

const StructuredValue *
StructuredValue::ElementAt(std::size_t Index) const noexcept {
  if (KindValue != StructuredKind::List || Index >= ElementValues.size())
    return nullptr;
  return &ElementValues[Index];
}

const StructuredValue *
StructuredValue::KeyAt(std::size_t Index) const noexcept {
  if (KindValue != StructuredKind::Map || Index >= Size())
    return nullptr;
  return &ElementValues[Index * 2];
}

const StructuredValue *
StructuredValue::MappedAt(std::size_t Index) const noexcept {
  if (KindValue != StructuredKind::Map || Index >= Size())
    return nullptr;
  return &ElementValues[Index * 2 + 1];
}

bool HasSameStructure(const StructuredValue &Left,
                      const StructuredValue &Right) {
  if (Left.Kind() != Right.Kind())
    return false;
  switch (Left.Kind()) {
  case StructuredKind::Null:
    return true;
  case StructuredKind::Handle:
    return Left.HandleStorage() == Right.HandleStorage() &&
           Left.HandlePermitsMutation() == Right.HandlePermitsMutation();
  case StructuredKind::Scalar: {
    const Value *LeftScalar = Left.ScalarValue();
    const Value *RightScalar = Right.ScalarValue();
    return LeftScalar && RightScalar && *LeftScalar == *RightScalar;
  }
  case StructuredKind::List:
  case StructuredKind::Map: {
    const std::span<const StructuredValue> LeftItems = Left.Elements();
    const std::span<const StructuredValue> RightItems = Right.Elements();
    if (LeftItems.size() != RightItems.size())
      return false;
    for (std::size_t Index = 0; Index < LeftItems.size(); ++Index) {
      if (!HasSameStructure(LeftItems[Index], RightItems[Index]))
        return false;
    }
    return true;
  }
  }
  return false;
}

std::string_view StructuredFailureText(StructuredFailure Failure) noexcept {
  switch (Failure) {
  case StructuredFailure::None:
    return "none";
  case StructuredFailure::UnavailableType:
    return "unavailable_type";
  case StructuredFailure::TypeMismatch:
    return "type_mismatch";
  case StructuredFailure::IntegerNonFinite:
    return "integer_non_finite";
  case StructuredFailure::IntegerOutOfRange:
    return "integer_out_of_range";
  case StructuredFailure::IntegerFractional:
    return "integer_fractional";
  case StructuredFailure::StringTooLong:
    return "string_too_long";
  case StructuredFailure::EmbeddedNullByte:
    return "embedded_null_byte";
  case StructuredFailure::SinglePrecisionRange:
    return "single_precision_range";
  case StructuredFailure::MissingElement:
    return "missing_element";
  case StructuredFailure::ForeignTableKey:
    return "foreign_table_key";
  case StructuredFailure::ElementCountMismatch:
    return "element_count_mismatch";
  case StructuredFailure::UnsupportedMapKey:
    return "unsupported_map_key";
  case StructuredFailure::DuplicateMapKey:
    return "duplicate_map_key";
  case StructuredFailure::UndeclaredEnumerator:
    return "undeclared_enumerator";
  case StructuredFailure::UnsupportedFlagBits:
    return "unsupported_flag_bits";
  case StructuredFailure::ForeignUserdata:
    return "foreign_userdata";
  case StructuredFailure::ForeignOriginState:
    return "foreign_origin_state";
  case StructuredFailure::MetatableMismatch:
    return "metatable_mismatch";
  case StructuredFailure::ExpiredUserdata:
    return "expired_userdata";
  case StructuredFailure::UserdataTypeMismatch:
    return "userdata_type_mismatch";
  case StructuredFailure::IncompatibleUserdataObject:
    return "incompatible_userdata_object";
  case StructuredFailure::ConstViolation:
    return "const_violation";
  case StructuredFailure::ConflictingOwnership:
    return "conflicting_ownership";
  case StructuredFailure::UnavailableConversion:
    return "unavailable_conversion";
  case StructuredFailure::StackUnavailable:
    return "stack_unavailable";
  case StructuredFailure::InternalFailure:
    return "internal_failure";
  }
  return "internal_failure";
}

bool IsInternalStructuredFailure(StructuredFailure Failure) {
  switch (Failure) {
  case StructuredFailure::UnavailableType:
  case StructuredFailure::UnavailableConversion:
  case StructuredFailure::StackUnavailable:
  case StructuredFailure::InternalFailure:
  case StructuredFailure::None:
    return true;
  default:
    return false;
  }
}

StructuredDiagnostic StructuredDiagnosticFrom(const ArgumentReadResult &Read,
                                              std::string ExpectedType,
                                              std::string Path) {
  StructuredDiagnostic Diagnostic;
  Diagnostic.Path = std::move(Path);
  Diagnostic.ExpectedType = std::move(ExpectedType);
  Diagnostic.ReceivedType = Read.ReceivedType;
  Diagnostic.ReceivedNumber = Read.ReceivedNumber;
  Diagnostic.ReceivedCount = Read.ReceivedByteCount;

  switch (Read.Status) {
  case ArgumentReadStatus::Success:
    Diagnostic.Failure = StructuredFailure::None;
    return Diagnostic;
  case ArgumentReadStatus::TypeMismatch:
    Diagnostic.Failure = StructuredFailure::TypeMismatch;
    return Diagnostic;
  case ArgumentReadStatus::IntegerNonFinite:
    Diagnostic.Failure = StructuredFailure::IntegerNonFinite;
    return Diagnostic;
  case ArgumentReadStatus::IntegerOutOfRange:
    Diagnostic.Failure = StructuredFailure::IntegerOutOfRange;
    return Diagnostic;
  case ArgumentReadStatus::IntegerFractional:
    Diagnostic.Failure = StructuredFailure::IntegerFractional;
    return Diagnostic;
  case ArgumentReadStatus::StringTooLong:
    Diagnostic.Failure = StructuredFailure::StringTooLong;
    Diagnostic.PermittedCount = MaximumInvocationStringBytes;
    return Diagnostic;
  case ArgumentReadStatus::InternalFailure:
    Diagnostic.Failure = StructuredFailure::InternalFailure;
    return Diagnostic;
  }
  Diagnostic.Failure = StructuredFailure::InternalFailure;
  return Diagnostic;
}

void ConversionScope::PushElement(std::size_t OneBasedPosition) {
  Segments.push_back("[" + std::to_string(OneBasedPosition) + "]");
}

void ConversionScope::PushKey(std::string_view KeyText) {
  Segments.push_back("[" + std::string(KeyText) + "]");
}

void ConversionScope::PushField(std::string_view Name) {
  Segments.push_back("." + std::string(Name));
}

void ConversionScope::Pop() {
  if (!Segments.empty())
    Segments.pop_back();
}

std::string ConversionScope::Path() const {
  std::string Text;
  for (const std::string &Segment : Segments)
    Text += Segment;
  return Text;
}

StructuredDiagnostic ConversionScope::Reject(StructuredFailure Failure,
                                             const TypeRecord &Record) const {
  StructuredDiagnostic Diagnostic;
  Diagnostic.Failure = Failure;
  Diagnostic.Path = Path();
  Diagnostic.ExpectedType = Record.PublicName;
  if (Record.MaximumByteCount)
    Diagnostic.PermittedCount = *Record.MaximumByteCount;
  return Diagnostic;
}

StructuredReadResult StructuredReadResult::Accept(StructuredValue Converted) {
  StructuredReadResult Result;
  Result.Accepted = true;
  Result.ConvertedValue = std::move(Converted);
  Result.Diagnostic.Failure = StructuredFailure::None;
  return Result;
}

StructuredReadResult
StructuredReadResult::Reject(StructuredDiagnostic Failure) {
  StructuredReadResult Result;
  Result.Accepted = false;
  Result.Diagnostic = std::move(Failure);
  return Result;
}

StructuredWriteResult StructuredWriteResult::Accept(int PublishedCount) {
  StructuredWriteResult Result;
  Result.Accepted = true;
  Result.PublishedCount = PublishedCount;
  Result.Diagnostic.Failure = StructuredFailure::None;
  return Result;
}

StructuredWriteResult
StructuredWriteResult::Reject(StructuredDiagnostic Failure) {
  StructuredWriteResult Result;
  Result.Accepted = false;
  Result.Diagnostic = std::move(Failure);
  return Result;
}

std::string FormatConversionNumber(double Number) {
  if (std::isnan(Number))
    return "NaN";
  if (std::isinf(Number))
    return std::signbit(Number) ? "negative infinity" : "positive infinity";

  std::ostringstream Stream;
  Stream << std::setprecision(std::numeric_limits<double>::max_digits10)
         << Number;
  return Stream.str();
}

std::string DescribeConversionFailure(const ConversionSubject &Subject,
                                      ConversionDirection Direction,
                                      std::size_t OneBasedPosition,
                                      const StructuredDiagnostic &Failure) {
  const std::size_t Position =
      Failure.Position != 0 ? Failure.Position : OneBasedPosition;
  const std::string Prefix = SubjectText(Subject) +
                             PositionText(Direction, Position) + Failure.Path +
                             " ";

  switch (Failure.Failure) {
  case StructuredFailure::None:
    return Prefix + "converted successfully.";
  case StructuredFailure::TypeMismatch:
    return Prefix + "expected " + Failure.ExpectedType + " but received " +
           Failure.ReceivedType + ".";
  case StructuredFailure::IntegerNonFinite:
    return Prefix + "expected a finite signed 32-bit integer but received " +
           FormatConversionNumber(Failure.ReceivedNumber) + ".";
  case StructuredFailure::IntegerOutOfRange:
    return Prefix +
           "expected signed 32-bit range [-2147483648, 2147483647] "
           "but received " +
           FormatConversionNumber(Failure.ReceivedNumber) + ".";
  case StructuredFailure::IntegerFractional:
    return Prefix + "expected an integral value but received " +
           FormatConversionNumber(Failure.ReceivedNumber) + ".";
  case StructuredFailure::StringTooLong:
    return Prefix + "received " + std::to_string(Failure.ReceivedCount) +
           " string bytes; maximum is " +
           std::to_string(Failure.PermittedCount) + ".";
  case StructuredFailure::EmbeddedNullByte:
    return Prefix + "expected " + Failure.ExpectedType +
           " without an embedded null byte but received " +
           std::to_string(Failure.ReceivedCount) + " string bytes.";
  case StructuredFailure::SinglePrecisionRange:
    return Prefix +
           "expected single-precision range "
           "[-3.4028234663852886e+38, 3.4028234663852886e+38] "
           "but received " +
           FormatConversionNumber(Failure.ReceivedNumber) + ".";
  case StructuredFailure::MissingElement:
    return Prefix + "expected " + Failure.ExpectedType +
           " but received no value.";
  case StructuredFailure::ForeignTableKey:
    return Prefix + "expected " + Failure.ExpectedType +
           " but received a table key outside its element positions.";
  case StructuredFailure::ElementCountMismatch:
    return Prefix + "received " + std::to_string(Failure.ReceivedCount) +
           " elements; " + Failure.ExpectedType + " permits " +
           std::to_string(Failure.PermittedCount) + ".";
  case StructuredFailure::UnsupportedMapKey:
    return Prefix +
           "expected a boolean, number, or string map key but received " +
           Failure.ReceivedType + ".";
  case StructuredFailure::DuplicateMapKey:
    return Prefix + "received two map keys that convert to one " +
           Failure.ExpectedType + " key.";
  case StructuredFailure::UndeclaredEnumerator:
    return Prefix + "expected one declared " + Failure.ExpectedType +
           " enumerator but received " +
           FormatConversionNumber(Failure.ReceivedNumber) + ".";
  case StructuredFailure::UnsupportedFlagBits:
    return Prefix + "expected only declared " + Failure.ExpectedType +
           " flag bits but received " +
           FormatConversionNumber(Failure.ReceivedNumber) +
           ", which carries an unsupported bit.";
  case StructuredFailure::ForeignUserdata:
    return Prefix + "expected " + Failure.ExpectedType + " but received " +
           Failure.ReceivedType + ", which is not a Luna userdata.";
  case StructuredFailure::ForeignOriginState:
    return Prefix + "received a " + Failure.ExpectedType +
           " that was exposed by a different Luna state.";
  case StructuredFailure::MetatableMismatch:
    return Prefix + "received a userdata that does not carry the " +
           Failure.ExpectedType + " metatable.";
  case StructuredFailure::ExpiredUserdata:
    return Prefix + "received a " + Failure.ExpectedType +
           " that is no longer accessible (" + Failure.ReceivedType + ").";
  case StructuredFailure::UserdataTypeMismatch:
    return Prefix + "expected " + Failure.ExpectedType +
           " but received a userdata of another registered class.";
  case StructuredFailure::IncompatibleUserdataObject:
    return Prefix + "expected " + Failure.ExpectedType +
           " but the registered safe cast refused this object as a value of "
           "that class.";
  case StructuredFailure::ConstViolation:
    return Prefix + "expected a mutable " + Failure.ExpectedType +
           " but received a const view.";
  case StructuredFailure::ConflictingOwnership:
    return Prefix + "cannot expose this " + Failure.ExpectedType +
           " again: the object is already exposed under a different ownership "
           "model or view (" +
           Failure.ReceivedType + ").";
  case StructuredFailure::UnavailableType:
    return "Internal error: " + Prefix +
           "has no available conversion in the captured type registry.";
  case StructuredFailure::UnavailableConversion:
    return "Internal error: " + Prefix + "cannot convert " +
           Failure.ExpectedType + " yet.";
  case StructuredFailure::StackUnavailable:
    return "Internal error: " + Prefix + "could not reserve " +
           std::to_string(Failure.ReceivedCount) + " stack slots.";
  case StructuredFailure::InternalFailure:
    return "Internal error while converting " + Prefix + "for " +
           Failure.ExpectedType + ".";
  }
  return "Internal error while converting " + Prefix + ".";
}

} // namespace Luna::Detail
