#pragma once

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
}

class ConversionContext;
class OwnedValue;
class ValueView;

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

[[nodiscard]] std::size_t MaximumConversionStringBytes() noexcept;

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

class ValueView final {
public:
  ValueView() noexcept = default;

  [[nodiscard]] bool IsActive() const noexcept;

  [[nodiscard]] ValueCategory Kind() const noexcept;
  [[nodiscard]] bool IsNil() const noexcept;
  [[nodiscard]] bool IsTable() const noexcept;

  [[nodiscard]] std::optional<bool> ToBoolean() const noexcept;
  [[nodiscard]] std::optional<double> ToNumber() const noexcept;
  [[nodiscard]] std::optional<std::string> ToText() const;

  [[nodiscard]] std::size_t ByteCount() const noexcept;

  [[nodiscard]] std::size_t Size() const noexcept;
  [[nodiscard]] ValueView Element(std::size_t Index) const noexcept;

  [[nodiscard]] std::size_t FieldCount() const noexcept;
  [[nodiscard]] std::string_view FieldName(std::size_t Index) const noexcept;
  [[nodiscard]] bool HasField(std::string_view Name) const noexcept;
  [[nodiscard]] ValueView Field(std::string_view Name) const noexcept;

  [[nodiscard]] std::string Path() const;

  [[nodiscard]] OwnedValue ToOwned() const;

private:
  friend class ConversionContext;
  friend class Detail::ConversionFrame;

  ValueView(std::uint64_t FrameToken, std::uint32_t NodeIndex) noexcept
      : FrameTokenValue(FrameToken), NodeIndexValue(NodeIndex) {}

  std::uint64_t FrameTokenValue = 0;
  std::uint32_t NodeIndexValue = 0;
};

class ConversionContext final {
public:
  ConversionContext() noexcept = default;

  [[nodiscard]] bool IsActive() const noexcept;
  [[nodiscard]] ConversionDirection Direction() const noexcept;

  [[nodiscard]] bool IsProbing() const noexcept;

  [[nodiscard]] ValueCategory Kind() const noexcept;
  [[nodiscard]] bool IsNil() const noexcept;
  [[nodiscard]] std::size_t Size() const noexcept;
  [[nodiscard]] ValueView Element(std::size_t Index) const noexcept;
  [[nodiscard]] ValueView Field(std::string_view Name) const noexcept;
  [[nodiscard]] ValueView Source() const noexcept;

  [[nodiscard]] std::string_view Callable() const noexcept;
  [[nodiscard]] std::size_t Position() const noexcept;
  [[nodiscard]] std::string Path() const;

  [[nodiscard]] std::string Describe(std::string_view Reason) const;

  [[nodiscard]] bool HasReservation() const noexcept;
  [[nodiscard]] bool IsPublished() const noexcept;
  [[nodiscard]] ValueReservation Reservation() const noexcept;

  [[nodiscard]] WriteResult Reserve(const ValueReservation &Request);
  [[nodiscard]] WriteResult Publish(const OwnedValue &Published);
  [[nodiscard]] WriteResult PublishPack(const ValuePack &Published);

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

template <class Type> class TypeConverter;

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
