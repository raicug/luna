#pragma once

// The public custom conversion boundary.
//
// Everything a converter author touches lives here and is built from Luna-owned
// and standard-library types only: no virtual-machine type, header, pointer,
// stack index, registry reference, constant, or macro is reachable from this
// header, and none is required to write, compile, or link a converter.
//
// The boundary has three parts:
//
//   * `ValueView` is a transient token naming one value inside the conversion
//     frame Luna opened for the current callback. It exposes shape only. It
//     carries no native pointer and no stack index, and it becomes inert as
//     soon as the frame it belongs to ends, so retaining one can never reach
//     released virtual-machine storage. `ToOwned()` is the documented way to
//     keep a value: it copies into an owning `OwnedValue`.
//   * `OwnedValue` and `ValuePack` are owning Luna values. They outlive any
//     frame and are what a converter retains, stores, or publishes.
//     `Luna::Value` remains the pinned foundation variant; `OwnedValue`
//     converts to and from it and additionally represents nil and tables.
//   * `ConversionContext` is the transient conversion frame itself: it reports
//     the shape under conversion, the diagnostic position and nested path, and
//     - for a committing write only - the resource reservation and atomic
//     publication operations.
//
// Viability and rank probing is separated from committing conversion by the
// type system: `Probe` receives `const ConversionContext&`, and every operation
// that mutates, allocates, or publishes is non-const. Luna additionally records
// any violation that reaches it through a const cast, so probe purity is both
// structurally enforced and observably detectable.

// clang-format off
#include <luna/binding/value.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace Luna {

namespace Detail {
class ConversionFrame;
} // namespace Detail

class ConversionContext;
class OwnedValue;
class ValueView;

// Luau-free description of what one value looks like. It is deliberately
// distinct from the pinned foundation `ValueKind`, which cannot describe nil,
// tables, userdata, or functions.
enum class ValueCategory {
  None,
  Nil,
  Boolean,
  Number,
  String,
  Table,
  Userdata,
  Function
};

[[nodiscard]] constexpr std::string_view
ValueCategoryText(ValueCategory Category) noexcept {
  switch (Category) {
  case ValueCategory::None:
    return "none";
  case ValueCategory::Nil:
    return "nil";
  case ValueCategory::Boolean:
    return "boolean";
  case ValueCategory::Number:
    return "number";
  case ValueCategory::String:
    return "string";
  case ValueCategory::Table:
    return "table";
  case ValueCategory::Userdata:
    return "userdata";
  case ValueCategory::Function:
    return "function";
  }
  return "none";
}

// Ordered conversion rank categories. Ranks are compared as Pareto dimensions;
// they are never summed into a score.
enum class ConversionRank { Exact, SafeBuiltIn, User };

[[nodiscard]] constexpr std::string_view
ConversionRankText(ConversionRank Rank) noexcept {
  switch (Rank) {
  case ConversionRank::Exact:
    return "exact";
  case ConversionRank::SafeBuiltIn:
    return "safe_builtin";
  case ConversionRank::User:
    return "user";
  }
  return "user";
}

// Direction of the frame a context describes.
enum class ConversionDirection { Read, Write };

[[nodiscard]] constexpr std::string_view
ConversionDirectionText(ConversionDirection Direction) noexcept {
  switch (Direction) {
  case ConversionDirection::Read:
    return "read";
  case ConversionDirection::Write:
    return "write";
  }
  return "read";
}

// Outcome of one committing read.
enum class ConversionStatus {
  Success,
  InactiveContext,
  ProbeViolation,
  TypeMismatch,
  MissingElement,
  OutOfRange,
  PolicyExceeded,
  IncompleteAggregate,
  Rejected
};

[[nodiscard]] constexpr std::string_view
ConversionStatusText(ConversionStatus Status) noexcept {
  switch (Status) {
  case ConversionStatus::Success:
    return "success";
  case ConversionStatus::InactiveContext:
    return "inactive_context";
  case ConversionStatus::ProbeViolation:
    return "probe_violation";
  case ConversionStatus::TypeMismatch:
    return "type_mismatch";
  case ConversionStatus::MissingElement:
    return "missing_element";
  case ConversionStatus::OutOfRange:
    return "out_of_range";
  case ConversionStatus::PolicyExceeded:
    return "policy_exceeded";
  case ConversionStatus::IncompleteAggregate:
    return "incomplete_aggregate";
  case ConversionStatus::Rejected:
    return "rejected";
  }
  return "rejected";
}

// Outcome of one reservation or publication.
enum class WriteStatus {
  Success,
  InactiveContext,
  ProbeViolation,
  WrongDirection,
  ReservationMissing,
  ReservationExceeded,
  PolicyExceeded,
  AlreadyPublished,
  IncompleteAggregate
};

[[nodiscard]] constexpr std::string_view
WriteStatusText(WriteStatus Status) noexcept {
  switch (Status) {
  case WriteStatus::Success:
    return "success";
  case WriteStatus::InactiveContext:
    return "inactive_context";
  case WriteStatus::ProbeViolation:
    return "probe_violation";
  case WriteStatus::WrongDirection:
    return "wrong_direction";
  case WriteStatus::ReservationMissing:
    return "reservation_missing";
  case WriteStatus::ReservationExceeded:
    return "reservation_exceeded";
  case WriteStatus::PolicyExceeded:
    return "policy_exceeded";
  case WriteStatus::AlreadyPublished:
    return "already_published";
  case WriteStatus::IncompleteAggregate:
    return "incomplete_aggregate";
  }
  return "incomplete_aggregate";
}

// Explicit Luna-owned resource request. A writer states everything it needs
// before it publishes anything; publication is refused unless the complete
// value fits inside the reservation.
struct ValueReservation final {
  std::size_t ValueCount = 0;
  std::size_t ElementCount = 0;
  std::size_t FieldCount = 0;
  std::size_t TableCount = 0;
  std::size_t UserdataCount = 0;
  std::size_t ByteCount = 0;
};

struct WriteResult final {
  WriteStatus Status = WriteStatus::InactiveContext;
  std::string Diagnostic;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Status == WriteStatus::Success;
  }
};

// Result of one viability and rank probe. A probe never converts and never
// mutates; it reports whether conversion would be viable, at which rank, and
// otherwise why it is not.
struct ConversionProbe final {
  bool IsViable = false;
  ConversionRank Rank = ConversionRank::User;
  std::string Rejection;
};

[[nodiscard]] inline ConversionProbe ViableProbe(ConversionRank Rank) {
  ConversionProbe Probe;
  Probe.IsViable = true;
  Probe.Rank = Rank;
  return Probe;
}

[[nodiscard]] inline ConversionProbe RejectedProbe(std::string Rejection) {
  ConversionProbe Probe;
  Probe.IsViable = false;
  Probe.Rejection = std::move(Rejection);
  return Probe;
}

template <class Type> struct ConversionResult final {
  ConversionStatus Status = ConversionStatus::Rejected;
  std::optional<Type> ConvertedValue;
  std::string Diagnostic;

  [[nodiscard]] bool IsSuccess() const noexcept {
    return Status == ConversionStatus::Success && ConvertedValue.has_value();
  }
};

// The largest string, in bytes, any conversion accepts in either direction.
// This is the inherited foundation policy and the single source of truth every
// converter and diagnostic reads.
[[nodiscard]] std::size_t MaximumConversionStringBytes() noexcept;

// One owning Luna value. It holds nil, a boolean, a number, a string, or a
// table of ordered elements plus name-sorted fields, owns every byte it
// reports, and is safe to retain for as long as the consumer wants. Fields are
// kept in canonical name order so equality and enumeration never depend on
// insertion order.
class OwnedValue final {
public:
  OwnedValue() = default;

  [[nodiscard]] static OwnedValue Nil() { return OwnedValue(); }

  [[nodiscard]] static OwnedValue Boolean(bool Source) {
    OwnedValue Result;
    Result.CategoryValue = ValueCategory::Boolean;
    Result.BooleanValue = Source;
    return Result;
  }

  [[nodiscard]] static OwnedValue Number(double Source) {
    OwnedValue Result;
    Result.CategoryValue = ValueCategory::Number;
    Result.NumberValue = Source;
    return Result;
  }

  [[nodiscard]] static OwnedValue Text(std::string Source) {
    OwnedValue Result;
    Result.CategoryValue = ValueCategory::String;
    Result.TextValue = std::move(Source);
    return Result;
  }

  [[nodiscard]] static OwnedValue Table() {
    OwnedValue Result;
    Result.CategoryValue = ValueCategory::Table;
    return Result;
  }

  // The pinned foundation variant converts into an owning value without loss.
  [[nodiscard]] static OwnedValue FromValue(const Value &Source) {
    if (const bool *SourceBoolean = std::get_if<bool>(&Source))
      return OwnedValue::Boolean(*SourceBoolean);
    if (const int *SourceInteger = std::get_if<int>(&Source))
      return OwnedValue::Number(static_cast<double>(*SourceInteger));
    if (const double *SourceNumber = std::get_if<double>(&Source))
      return OwnedValue::Number(*SourceNumber);
    if (const std::string *SourceText = std::get_if<std::string>(&Source))
      return OwnedValue::Text(*SourceText);
    return OwnedValue();
  }

  // The reverse mapping. Nil and tables have no foundation representation, and
  // a number that is not an exact signed 32-bit integer stays a number.
  [[nodiscard]] std::optional<Value> ToValue() const {
    switch (CategoryValue) {
    case ValueCategory::Boolean:
      return Value(BooleanValue);
    case ValueCategory::Number:
      return Value(NumberValue);
    case ValueCategory::String:
      return Value(TextValue);
    default:
      break;
    }
    return std::nullopt;
  }

  [[nodiscard]] ValueCategory Kind() const noexcept { return CategoryValue; }

  [[nodiscard]] bool IsNil() const noexcept {
    return CategoryValue == ValueCategory::Nil;
  }

  [[nodiscard]] bool IsTable() const noexcept {
    return CategoryValue == ValueCategory::Table;
  }

  [[nodiscard]] std::optional<bool> ToBoolean() const noexcept {
    if (CategoryValue != ValueCategory::Boolean)
      return std::nullopt;
    return BooleanValue;
  }

  [[nodiscard]] std::optional<double> ToNumber() const noexcept {
    if (CategoryValue != ValueCategory::Number)
      return std::nullopt;
    return NumberValue;
  }

  [[nodiscard]] std::optional<std::string> ToText() const {
    if (CategoryValue != ValueCategory::String)
      return std::nullopt;
    return TextValue;
  }

  [[nodiscard]] std::string_view TextBytes() const noexcept {
    return TextValue;
  }

  // Ordered elements. `Index` is zero-based; diagnostics print the one-based
  // Luau position.
  [[nodiscard]] std::size_t Size() const noexcept {
    return ElementsValue.size();
  }

  [[nodiscard]] OwnedValue Element(std::size_t Index) const {
    if (Index >= ElementsValue.size())
      return OwnedValue();
    return ElementsValue[Index];
  }

  void Append(OwnedValue Element) {
    CategoryValue = ValueCategory::Table;
    ElementsValue.push_back(std::move(Element));
  }

  [[nodiscard]] std::size_t FieldCount() const noexcept {
    return FieldNamesValue.size();
  }

  [[nodiscard]] std::string_view FieldName(std::size_t Index) const noexcept {
    if (Index >= FieldNamesValue.size())
      return {};
    return FieldNamesValue[Index];
  }

  [[nodiscard]] bool HasField(std::string_view Name) const {
    const std::optional<std::size_t> Found = FindField(Name);
    return Found.has_value();
  }

  [[nodiscard]] OwnedValue Field(std::string_view Name) const {
    const std::optional<std::size_t> Found = FindField(Name);
    if (!Found)
      return OwnedValue();
    return FieldValuesValue[*Found];
  }

  void SetField(std::string_view Name, OwnedValue Field) {
    CategoryValue = ValueCategory::Table;
    if (const std::optional<std::size_t> Found = FindField(Name)) {
      FieldValuesValue[*Found] = std::move(Field);
      return;
    }
    std::size_t Position = 0;
    while (Position < FieldNamesValue.size() &&
           FieldNamesValue[Position] < Name)
      ++Position;
    FieldNamesValue.insert(FieldNamesValue.begin() +
                               static_cast<std::ptrdiff_t>(Position),
                           std::string(Name));
    FieldValuesValue.insert(FieldValuesValue.begin() +
                                static_cast<std::ptrdiff_t>(Position),
                            std::move(Field));
  }

  // Recursive resource accounting. Writers reserve against exactly these
  // numbers, so a reservation can be validated before anything is published.
  [[nodiscard]] std::size_t TotalValueCount() const {
    std::size_t Total = 1;
    for (const OwnedValue &Element : ElementsValue)
      Total += Element.TotalValueCount();
    for (const OwnedValue &Field : FieldValuesValue)
      Total += Field.TotalValueCount();
    return Total;
  }

  [[nodiscard]] std::size_t TotalElementCount() const {
    std::size_t Total = ElementsValue.size();
    for (const OwnedValue &Element : ElementsValue)
      Total += Element.TotalElementCount();
    for (const OwnedValue &Field : FieldValuesValue)
      Total += Field.TotalElementCount();
    return Total;
  }

  [[nodiscard]] std::size_t TotalFieldCount() const {
    std::size_t Total = FieldNamesValue.size();
    for (const OwnedValue &Element : ElementsValue)
      Total += Element.TotalFieldCount();
    for (const OwnedValue &Field : FieldValuesValue)
      Total += Field.TotalFieldCount();
    return Total;
  }

  [[nodiscard]] std::size_t TotalTableCount() const {
    std::size_t Total = CategoryValue == ValueCategory::Table ? 1 : 0;
    for (const OwnedValue &Element : ElementsValue)
      Total += Element.TotalTableCount();
    for (const OwnedValue &Field : FieldValuesValue)
      Total += Field.TotalTableCount();
    return Total;
  }

  [[nodiscard]] std::size_t TotalByteCount() const {
    std::size_t Total = TextValue.size();
    for (const std::string &Name : FieldNamesValue)
      Total += Name.size();
    for (const OwnedValue &Element : ElementsValue)
      Total += Element.TotalByteCount();
    for (const OwnedValue &Field : FieldValuesValue)
      Total += Field.TotalByteCount();
    return Total;
  }

  // Largest single string anywhere in the value, which is what the inherited
  // per-string byte policy applies to.
  [[nodiscard]] std::size_t LargestStringByteCount() const {
    std::size_t Largest = TextValue.size();
    for (const std::string &Name : FieldNamesValue) {
      if (Name.size() > Largest)
        Largest = Name.size();
    }
    for (const OwnedValue &Element : ElementsValue) {
      const std::size_t Nested = Element.LargestStringByteCount();
      if (Nested > Largest)
        Largest = Nested;
    }
    for (const OwnedValue &Field : FieldValuesValue) {
      const std::size_t Nested = Field.LargestStringByteCount();
      if (Nested > Largest)
        Largest = Nested;
    }
    return Largest;
  }

  // The reservation one publication of this value needs, exactly.
  [[nodiscard]] ValueReservation RequiredReservation() const {
    ValueReservation Required;
    Required.ValueCount = TotalValueCount();
    Required.ElementCount = TotalElementCount();
    Required.FieldCount = TotalFieldCount();
    Required.TableCount = TotalTableCount();
    Required.ByteCount = TotalByteCount();
    return Required;
  }

  [[nodiscard]] friend bool operator==(const OwnedValue &Left,
                                       const OwnedValue &Right) {
    if (Left.CategoryValue != Right.CategoryValue)
      return false;
    switch (Left.CategoryValue) {
    case ValueCategory::Boolean:
      return Left.BooleanValue == Right.BooleanValue;
    case ValueCategory::Number:
      return Left.NumberValue == Right.NumberValue;
    case ValueCategory::String:
      return Left.TextValue == Right.TextValue;
    default:
      break;
    }
    return Left.ElementsValue == Right.ElementsValue &&
           Left.FieldNamesValue == Right.FieldNamesValue &&
           Left.FieldValuesValue == Right.FieldValuesValue;
  }

  [[nodiscard]] friend bool operator!=(const OwnedValue &Left,
                                       const OwnedValue &Right) {
    return !(Left == Right);
  }

private:
  [[nodiscard]] std::optional<std::size_t>
  FindField(std::string_view Name) const {
    for (std::size_t Index = 0; Index < FieldNamesValue.size(); ++Index) {
      if (FieldNamesValue[Index] == Name)
        return Index;
    }
    return std::nullopt;
  }

  ValueCategory CategoryValue = ValueCategory::Nil;
  bool BooleanValue = false;
  double NumberValue = 0.0;
  std::string TextValue;
  std::vector<OwnedValue> ElementsValue;
  std::vector<std::string> FieldNamesValue;
  std::vector<OwnedValue> FieldValuesValue;
};

// One owning ordered pack of values. Argument packs and return packs are both
// this shape, and it stays valid after the frame that produced it ends.
class ValuePack final {
public:
  ValuePack() = default;

  [[nodiscard]] std::size_t Size() const noexcept { return ValuesValue.size(); }

  [[nodiscard]] bool IsEmpty() const noexcept { return ValuesValue.empty(); }

  [[nodiscard]] OwnedValue At(std::size_t Index) const {
    if (Index >= ValuesValue.size())
      return OwnedValue();
    return ValuesValue[Index];
  }

  void Append(OwnedValue Value) { ValuesValue.push_back(std::move(Value)); }

  void Clear() noexcept { ValuesValue.clear(); }

  [[nodiscard]] std::size_t LargestStringByteCount() const {
    std::size_t Largest = 0;
    for (const OwnedValue &Value : ValuesValue) {
      const std::size_t Nested = Value.LargestStringByteCount();
      if (Nested > Largest)
        Largest = Nested;
    }
    return Largest;
  }

  [[nodiscard]] ValueReservation RequiredReservation() const {
    ValueReservation Required;
    for (const OwnedValue &Value : ValuesValue) {
      const ValueReservation Nested = Value.RequiredReservation();
      Required.ValueCount += Nested.ValueCount;
      Required.ElementCount += Nested.ElementCount;
      Required.FieldCount += Nested.FieldCount;
      Required.TableCount += Nested.TableCount;
      Required.ByteCount += Nested.ByteCount;
    }
    return Required;
  }

  [[nodiscard]] friend bool operator==(const ValuePack &Left,
                                       const ValuePack &Right) {
    return Left.ValuesValue == Right.ValuesValue;
  }

  [[nodiscard]] friend bool operator!=(const ValuePack &Left,
                                       const ValuePack &Right) {
    return !(Left == Right);
  }

private:
  std::vector<OwnedValue> ValuesValue;
};

// A transient, non-owning token naming one value inside the conversion frame
// Luna opened for the current callback.
//
// The token is a Luna-owned opaque number. It is not a pointer, not a stack
// index, and not a registry reference, and no accessor can turn it into one. A
// view is valid only for the documented extent of the conversion callback it
// was handed to: once that frame ends, every copy of the view answers as an
// inert value and the attempt is recorded, so retaining a view can never reach
// released virtual-machine storage. `ToOwned()` is how a converter keeps a
// value beyond the callback.
class ValueView final {
public:
  ValueView() noexcept = default;

  // The view still names a live value in a live frame.
  [[nodiscard]] bool IsActive() const noexcept;

  [[nodiscard]] ValueCategory Kind() const noexcept;
  [[nodiscard]] bool IsNil() const noexcept;
  [[nodiscard]] bool IsTable() const noexcept;

  [[nodiscard]] std::optional<bool> ToBoolean() const noexcept;
  [[nodiscard]] std::optional<double> ToNumber() const noexcept;
  [[nodiscard]] std::optional<std::string> ToText() const;

  // Byte count of a string value, which is what the inherited per-string byte
  // policy is reported against.
  [[nodiscard]] std::size_t ByteCount() const noexcept;

  // Ordered elements. `Index` is zero-based; the nested path prints the
  // one-based Luau position.
  [[nodiscard]] std::size_t Size() const noexcept;
  [[nodiscard]] ValueView Element(std::size_t Index) const noexcept;

  [[nodiscard]] std::size_t FieldCount() const noexcept;
  [[nodiscard]] std::string_view FieldName(std::size_t Index) const noexcept;
  [[nodiscard]] bool HasField(std::string_view Name) const noexcept;
  [[nodiscard]] ValueView Field(std::string_view Name) const noexcept;

  // Complete nested path of this value, such as `argument 2[4].Key`.
  [[nodiscard]] std::string Path() const;

  // Copy out of the frame. This is the only supported way to retain a value.
  [[nodiscard]] OwnedValue ToOwned() const;

private:
  friend class ConversionContext;
  friend class Detail::ConversionFrame;

  ValueView(std::uint64_t FrameToken, std::uint32_t NodeIndex) noexcept
      : FrameTokenValue(FrameToken), NodeIndexValue(NodeIndex) {}

  std::uint64_t FrameTokenValue = 0;
  std::uint32_t NodeIndexValue = 0;
};

// The transient conversion frame a converter is invoked with.
//
// A context reports the shape under conversion and the diagnostic position and
// nested path. On a committing write frame it also reserves resources and
// publishes the finished value; every one of those operations is non-const, so
// a probe - which receives `const ConversionContext&` - cannot reach them at
// all. Like `ValueView` it holds only a Luna-owned opaque token and becomes
// inert when its frame ends, so retaining a context is harmless and detectable
// rather than dangerous.
class ConversionContext final {
public:
  ConversionContext() noexcept = default;

  [[nodiscard]] bool IsActive() const noexcept;
  [[nodiscard]] ConversionDirection Direction() const noexcept;

  // The frame is a viability and rank probe: nothing may be mutated,
  // allocated, published, or converted through it.
  [[nodiscard]] bool IsProbing() const noexcept;

  // Shape of the value under conversion.
  [[nodiscard]] ValueCategory Kind() const noexcept;
  [[nodiscard]] bool IsNil() const noexcept;
  [[nodiscard]] std::size_t Size() const noexcept;
  [[nodiscard]] ValueView Element(std::size_t Index) const noexcept;
  [[nodiscard]] ValueView Field(std::string_view Name) const noexcept;
  [[nodiscard]] ValueView Source() const noexcept;

  // Diagnostic identity: the callable or member, the one-based argument or
  // return position, and the complete nested path.
  [[nodiscard]] std::string_view Callable() const noexcept;
  [[nodiscard]] std::size_t Position() const noexcept;
  [[nodiscard]] std::string Path() const;

  // One deterministic diagnostic naming the callable, the position, the
  // complete nested path, and the reason.
  [[nodiscard]] std::string Describe(std::string_view Reason) const;

  // Writer side. Resources are requested and validated first; publication is
  // atomic and happens at most once.
  [[nodiscard]] bool HasReservation() const noexcept;
  [[nodiscard]] bool IsPublished() const noexcept;
  [[nodiscard]] ValueReservation Reservation() const noexcept;

  [[nodiscard]] WriteResult Reserve(const ValueReservation &Request);
  [[nodiscard]] WriteResult Publish(const OwnedValue &Published);
  [[nodiscard]] WriteResult PublishPack(const ValuePack &Published);

  // Luna's own channel for recording an attempted probe violation. It is
  // const because the violation is recorded in Luna-owned diagnostics and
  // never in the conversion outcome.
  void ReportProbeViolation(std::string_view Reason) const;

private:
  friend class Detail::ConversionFrame;

  ConversionContext(std::uint64_t FrameToken, std::uint32_t NodeIndex,
                    bool Probing) noexcept
      : FrameTokenValue(FrameToken), NodeIndexValue(NodeIndex),
        ProbingValue(Probing) {}

  std::uint64_t FrameTokenValue = 0;
  std::uint32_t NodeIndexValue = 0;
  bool ProbingValue = false;
};

// The type a converter author specializes. A specialization supplies exactly
// three operations, and nothing in their signatures names a virtual machine:
//
//   template <> class TypeConverter<MyType> {
//   public:
//     ConversionProbe Probe(ValueView Source,
//                           const ConversionContext& Context) const;
//     ConversionResult<MyType> Read(ValueView Source,
//                                   ConversionContext& Context) const;
//     WriteResult Write(const MyType& Source, ConversionContext& Context)
//     const;
//   };
template <class Type> class TypeConverter;

// A type participates in the boundary when its converter supplies the three
// separated operations with exactly these shapes.
template <class Type>
concept ConversionCapable =
    requires(const TypeConverter<Type> &Converter, ValueView Source,
             const ConversionContext &Probing, ConversionContext &Committing,
             const Type &Written) {
      { Converter.Probe(Source, Probing) } -> std::same_as<ConversionProbe>;
      {
        Converter.Read(Source, Committing)
      } -> std::same_as<ConversionResult<Type>>;
      { Converter.Write(Written, Committing) } -> std::same_as<WriteResult>;
    };

// Boundary entry points. Every read and write goes through these so probing
// stays separated from committing conversion in one place.
template <class Type>
[[nodiscard]] ConversionProbe ProbeValue(ValueView Source,
                                         const ConversionContext &Context) {
  const TypeConverter<Type> Converter;
  return Converter.Probe(Source, Context);
}

template <class Type>
[[nodiscard]] ConversionResult<Type> ReadValue(ValueView Source,
                                               ConversionContext &Context) {
  if (Context.IsProbing()) {
    Context.ReportProbeViolation("a probe invoked a committing read");
    ConversionResult<Type> Result;
    Result.Status = ConversionStatus::ProbeViolation;
    Result.Diagnostic =
        Context.Describe("a viability probe cannot invoke conversion");
    return Result;
  }
  const TypeConverter<Type> Converter;
  return Converter.Read(Source, Context);
}

template <class Type>
[[nodiscard]] WriteResult WriteValue(const Type &Source,
                                     ConversionContext &Context) {
  if (Context.IsProbing()) {
    Context.ReportProbeViolation("a probe invoked a committing write");
    WriteResult Result;
    Result.Status = WriteStatus::ProbeViolation;
    Result.Diagnostic =
        Context.Describe("a viability probe cannot invoke conversion");
    return Result;
  }
  const TypeConverter<Type> Converter;
  return Converter.Write(Source, Context);
}

} // namespace Luna
