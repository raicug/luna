// clang-format off
#include <luna/binding/argument_pack.hpp>
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/conversion.hpp>
#include <luna/binding/parameter_descriptor.hpp>
#include <luna/binding/return_pack.hpp>
#include <luna/binding/value.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/core/results/execution_result.hpp>
#include <luna/state/state.hpp>

#include "state/invocation/overload/instrumentation.hpp"
#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <rapidcheck.h>

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::ParameterArity;
using Luna::ParameterDescriptor;
using Luna::ParameterForm;
using Luna::ParameterShapeIssue;
using Luna::ParameterShapeStatus;
using Luna::ValueKind;
using FaultPoint = Luna::Detail::StateFaultPoint;

class ByteCursor final {
public:
  explicit ByteCursor(const std::vector<std::uint8_t> &Bytes) noexcept
      : BytesValue(&Bytes) {}

  [[nodiscard]] std::uint8_t Next() noexcept {
    const std::size_t Index = IndexValue++;
    if (BytesValue->empty())
      return static_cast<std::uint8_t>(Index * 29U + 7U);
    return (*BytesValue)[Index % BytesValue->size()];
  }

  [[nodiscard]] std::size_t Pick(std::size_t Count) noexcept {
    return Count == 0 ? 0 : static_cast<std::size_t>(Next()) % Count;
  }

private:
  const std::vector<std::uint8_t> *BytesValue;
  std::size_t IndexValue = 0;
};

[[nodiscard]] std::string IntegerText(int Value) {
  return std::to_string(Value);
}

[[nodiscard]] std::string NumberText(double Value) {
  std::ostringstream Stream;
  Stream << Value;
  return Stream.str();
}

[[nodiscard]] std::string FlagText(bool Value) {
  return Value ? "true" : "false";
}

[[nodiscard]] std::string DiagnosticNumberText(double Value) {
  std::ostringstream Stream;
  Stream << std::setprecision(std::numeric_limits<double>::max_digits10)
         << Value;
  return Stream.str();
}

[[nodiscard]] std::string_view PublicTypeName(ValueKind Kind) noexcept {
  switch (Kind) {
  case ValueKind::Boolean:
    return "boolean";
  case ValueKind::Integer:
    return "signed 32-bit integer";
  case ValueKind::Number:
    return "number";
  case ValueKind::String:
    return "string";
  }
  return "unknown";
}

[[nodiscard]] std::string ValueText(const Luna::Value &Source) {
  if (const bool *Flag = std::get_if<bool>(&Source))
    return "boolean " + FlagText(*Flag);
  if (const int *Whole = std::get_if<int>(&Source))
    return "integer " + IntegerText(*Whole);
  if (const double *Number = std::get_if<double>(&Source))
    return "number " + NumberText(*Number);
  if (const std::string *Text = std::get_if<std::string>(&Source))
    return "string " + *Text;
  return "unknown";
}

[[nodiscard]] std::string ElementText(const Luna::OwnedValue &Element) {
  switch (Element.Kind()) {
  case Luna::ValueCategory::Boolean:
    return "boolean " + FlagText(Element.ToBoolean().value_or(false));
  case Luna::ValueCategory::Number:
    return "number " + NumberText(Element.ToNumber().value_or(0.0));
  case Luna::ValueCategory::String:
    return "string " + Element.ToText().value_or(std::string());
  case Luna::ValueCategory::Table:
    return "table";
  default:
    break;
  }
  return "nil";
}

struct GeneratedParameter final {
  ParameterForm Form = ParameterForm::Required;
  ValueKind Kind = ValueKind::Integer;
  ValueKind DefaultKind = ValueKind::Integer;
  bool AcceptsNil = false;
  bool Retains = false;
};

[[nodiscard]] Luna::Value ValueOfKind(ValueKind Kind) {
  switch (Kind) {
  case ValueKind::Boolean:
    return Luna::Value(true);
  case ValueKind::Integer:
    return Luna::Value(3);
  case ValueKind::Number:
    return Luna::Value(1.5);
  case ValueKind::String:
    break;
  }
  return Luna::Value(std::string("d"));
}

[[nodiscard]] ParameterDescriptor DescriptorOf(const GeneratedParameter &From) {
  switch (From.Form) {
  case ParameterForm::Required:
    return ParameterDescriptor::ForRequired(From.Kind);
  case ParameterForm::Optional:
    return ParameterDescriptor::ForOptional(From.Kind);
  case ParameterForm::Defaulted:
    return ParameterDescriptor::ForDefaulted(
        From.Kind, ValueOfKind(From.DefaultKind), From.AcceptsNil);
  case ParameterForm::Variadic:
  case ParameterForm::Delegate:
  case ParameterForm::Converted:
    break;
  }
  return ParameterDescriptor::ForVariadic(From.Retains);
}

struct ReferenceShape final {
  ParameterShapeStatus Status = ParameterShapeStatus::Valid;
  std::size_t Position = 0;
};

[[nodiscard]] ReferenceShape
ReferenceValidate(const std::vector<GeneratedParameter> &Parameters) {
  bool SawRelaxed = false;
  for (std::size_t Index = 0; Index < Parameters.size(); ++Index) {
    const GeneratedParameter &Parameter = Parameters[Index];
    const std::size_t Position = Index + 1;

    if (Parameter.Form == ParameterForm::Variadic) {
      if (Position != Parameters.size())
        return ReferenceShape{ParameterShapeStatus::VariadicNotFinal, Position};
      continue;
    }

    if (Parameter.Form == ParameterForm::Required) {
      if (SawRelaxed)
        return ReferenceShape{ParameterShapeStatus::RequiredAfterRelaxed,
                              Position};
      continue;
    }

    if (Parameter.Form == ParameterForm::Defaulted &&
        Parameter.DefaultKind != Parameter.Kind)
      return ReferenceShape{ParameterShapeStatus::DefaultTypeMismatch,
                            Position};

    SawRelaxed = true;
  }
  return ReferenceShape();
}

struct ReferenceArity final {
  std::size_t Minimum = 0;
  std::size_t FixedCount = 0;
  bool IsVariadic = false;
};

[[nodiscard]] ReferenceArity
ReferenceArityOf(const std::vector<GeneratedParameter> &Parameters) {
  ReferenceArity Arity;
  for (const GeneratedParameter &Parameter : Parameters) {
    if (Parameter.Form == ParameterForm::Variadic) {
      Arity.IsVariadic = true;
      continue;
    }
    ++Arity.FixedCount;
    if (Parameter.Form == ParameterForm::Required)
      ++Arity.Minimum;
  }
  return Arity;
}

[[nodiscard]] std::string ReferenceArityText(const ReferenceArity &Arity) {
  if (Arity.IsVariadic)
    return "at least " + std::to_string(Arity.Minimum) + " arguments";
  if (Arity.Minimum == Arity.FixedCount)
    return std::to_string(Arity.Minimum) + " arguments";
  return "between " + std::to_string(Arity.Minimum) + " and " +
         std::to_string(Arity.FixedCount) + " arguments";
}

[[nodiscard]] ValueKind GeneratedKind(std::size_t Choice) noexcept {
  switch (Choice % 4) {
  case 0:
    return ValueKind::Integer;
  case 1:
    return ValueKind::Number;
  case 2:
    return ValueKind::String;
  default:
    return ValueKind::Boolean;
  }
}

void VerifyDeclaredShapeModel(ByteCursor &Cursor) {
  const std::size_t Count = Cursor.Pick(5);

  std::vector<GeneratedParameter> Described;
  std::vector<ParameterDescriptor> Descriptors;
  for (std::size_t Index = 0; Index < Count; ++Index) {
    GeneratedParameter Parameter;
    switch (Cursor.Pick(4)) {
    case 0:
      Parameter.Form = ParameterForm::Required;
      break;
    case 1:
      Parameter.Form = ParameterForm::Optional;
      break;
    case 2:
      Parameter.Form = ParameterForm::Defaulted;
      break;
    default:
      Parameter.Form = ParameterForm::Variadic;
      break;
    }
    Parameter.Kind = GeneratedKind(Cursor.Pick(4));
    Parameter.DefaultKind =
        Cursor.Pick(3) == 0 ? GeneratedKind(Cursor.Pick(4)) : Parameter.Kind;
    Parameter.AcceptsNil = Cursor.Pick(2) == 0;
    Parameter.Retains = Cursor.Pick(2) == 0;
    Described.push_back(Parameter);
    Descriptors.push_back(DescriptorOf(Parameter));
  }

  const ReferenceShape Expected = ReferenceValidate(Described);
  const ParameterShapeIssue Observed =
      Luna::ValidateParameterShape(Descriptors);
  RC_TAG(std::string(Luna::ParameterShapeStatusText(Expected.Status)));
  RC_ASSERT(Observed.Status == Expected.Status);
  RC_ASSERT(Observed.Position == Expected.Position);
  RC_ASSERT(Observed.IsValid() ==
            (Expected.Status == ParameterShapeStatus::Valid));

  const ReferenceArity ExpectedArity = ReferenceArityOf(Described);
  const ParameterArity Arity = Luna::ArityOf(Descriptors);
  RC_ASSERT(Arity.Minimum == ExpectedArity.Minimum);
  RC_ASSERT(Arity.FixedCount == ExpectedArity.FixedCount);
  RC_ASSERT(Arity.IsVariadic == ExpectedArity.IsVariadic);
  RC_ASSERT(Arity.Maximum.has_value() != ExpectedArity.IsVariadic);
  if (!ExpectedArity.IsVariadic)
    RC_ASSERT(*Arity.Maximum == ExpectedArity.FixedCount);

  for (std::size_t Index = 0; Index < Count; ++Index) {
    const GeneratedParameter &Source = Described[Index];
    const ParameterDescriptor &Declared = Descriptors[Index];
    RC_ASSERT(Declared.Form() == Source.Form);
    RC_ASSERT(Declared.IsVariadic() ==
              (Source.Form == ParameterForm::Variadic));
    RC_ASSERT(Declared.IsOmittable() ==
              (Source.Form != ParameterForm::Required));
    RC_ASSERT(Declared.HasDefault() ==
              (Source.Form == ParameterForm::Defaulted));

    if (Source.Form == ParameterForm::Variadic) {
      RC_ASSERT(Declared.Kind() == nullptr);
      RC_ASSERT(Declared.Retains() == Source.Retains);
      continue;
    }

    RC_ASSERT(Declared.Kind() != nullptr);
    RC_ASSERT(*Declared.Kind() == Source.Kind);
    if (const Luna::Value *Default = Declared.Default())
      RC_ASSERT(*Default == ValueOfKind(Source.DefaultKind));

    const bool ExpectedNil =
        Source.Form == ParameterForm::Optional ||
        (Source.Form == ParameterForm::Defaulted && Source.AcceptsNil);
    RC_ASSERT(Declared.AcceptsNil() == ExpectedNil);
  }
}

} // namespace

namespace {

constexpr std::size_t ShapeCount = 8;

[[nodiscard]] std::string_view ShapeName(std::size_t Shape) noexcept {
  switch (Shape) {
  case 0:
    return "Scale";
  case 1:
    return "Offset";
  case 2:
    return "Tag";
  case 3:
    return "Concat";
  case 4:
    return "Sum";
  case 5:
    return "Join";
  case 6:
    return "Mix";
  default:
    break;
  }
  return "Range";
}

struct ModelParameter final {
  ParameterForm Form = ParameterForm::Required;
  ValueKind Kind = ValueKind::Integer;
  bool AcceptsNil = false;
  std::optional<Luna::Value> Default;
};

[[nodiscard]] ModelParameter Required(ValueKind Kind) {
  return ModelParameter{ParameterForm::Required, Kind, false, std::nullopt};
}

[[nodiscard]] ModelParameter Optional(ValueKind Kind) {
  return ModelParameter{ParameterForm::Optional, Kind, true, std::nullopt};
}

[[nodiscard]] ModelParameter Defaulted(ValueKind Kind, Luna::Value Default,
                                       bool AcceptsNil) {
  return ModelParameter{ParameterForm::Defaulted, Kind, AcceptsNil,
                        std::move(Default)};
}

[[nodiscard]] ModelParameter Variadic() {
  return ModelParameter{ParameterForm::Variadic, ValueKind::Integer, false,
                        std::nullopt};
}

[[nodiscard]] const std::vector<ModelParameter> &
ShapeParameters(std::size_t Shape) {
  static const std::vector<std::vector<ModelParameter>> Shapes{
      {Required(ValueKind::Integer), Optional(ValueKind::Integer)},
      {Required(ValueKind::Integer),
       Defaulted(ValueKind::Integer, Luna::Value(5), false)},
      {Defaulted(ValueKind::Integer, Luna::Value(7), true)},
      {Required(ValueKind::Integer), Required(ValueKind::String)},
      {Variadic()},
      {Required(ValueKind::String), Variadic()},
      {Required(ValueKind::Integer), Optional(ValueKind::Integer), Variadic()},
      {Defaulted(ValueKind::Integer, Luna::Value(1), false),
       Defaulted(ValueKind::Number, Luna::Value(2.5), false)}};
  return Shapes[Shape];
}

[[nodiscard]] ReferenceArity ShapeArity(std::size_t Shape) {
  ReferenceArity Arity;
  for (const ModelParameter &Parameter : ShapeParameters(Shape)) {
    if (Parameter.Form == ParameterForm::Variadic) {
      Arity.IsVariadic = true;
      continue;
    }
    ++Arity.FixedCount;
    if (Parameter.Form == ParameterForm::Required)
      ++Arity.Minimum;
  }
  return Arity;
}

struct ShapeObservation final {
  std::size_t Calls = 0;
  std::vector<std::string> Slots;
  std::vector<std::string> Variadic;
};

struct CallObservation final {
  std::vector<ShapeObservation> Shapes =
      std::vector<ShapeObservation>(ShapeCount);
  std::size_t ReuseCalls = 0;
};

[[nodiscard]] int MarkerOf(std::size_t Shape, std::size_t PresentSlots,
                           std::size_t VariadicSize) {
  return static_cast<int>(1000U * (Shape + 1U) + 10U * PresentSlots +
                          VariadicSize);
}

[[nodiscard]] std::string SlotText(int Value) {
  return "integer " + IntegerText(Value);
}

[[nodiscard]] std::string SlotText(const std::optional<int> &Value) {
  return Value ? SlotText(*Value) : std::string("omitted");
}

template <class Arguments>
void RecordVariadic(ShapeObservation &Entry, const Arguments &Values) {
  for (std::size_t Index = 0; Index < Values.Size(); ++Index) {
    Entry.Variadic.push_back(std::to_string(Values.Position(Index)) + ":" +
                             ElementText(Values.At(Index)));
  }
}

[[nodiscard]] bool RegisterShapes(Luna::BindingRegistry &Registry,
                                  CallObservation &Observed) {
  bool Registered = true;

  Registered =
      Registered &&
      Registry
          .RegisterFunction(ShapeName(0),
                            [&Observed](int First, std::optional<int> Second) {
                              ShapeObservation &Entry = Observed.Shapes[0];
                              ++Entry.Calls;
                              Entry.Slots.push_back(SlotText(First));
                              Entry.Slots.push_back(SlotText(Second));
                              return MarkerOf(0, Second ? 2 : 1, 0);
                            })
          .IsSuccess();

  Registered =
      Registered &&
      Registry
          .RegisterFunction(ShapeName(1),
                            Luna::WithDefaults(
                                [&Observed](int First, int Second) {
                                  ShapeObservation &Entry = Observed.Shapes[1];
                                  ++Entry.Calls;
                                  Entry.Slots.push_back(SlotText(First));
                                  Entry.Slots.push_back(SlotText(Second));
                                  return MarkerOf(1, 2, 0);
                                },
                                5))
          .IsSuccess();

  Registered =
      Registered &&
      Registry
          .RegisterFunction(ShapeName(2),
                            Luna::WithDefaults(
                                [&Observed](std::optional<int> First) {
                                  ShapeObservation &Entry = Observed.Shapes[2];
                                  ++Entry.Calls;
                                  Entry.Slots.push_back(SlotText(First));
                                  return MarkerOf(2, First ? 1 : 0, 0);
                                },
                                7))
          .IsSuccess();

  Registered =
      Registered &&
      Registry
          .RegisterFunction(ShapeName(3),
                            [&Observed](int First, std::string Second) {
                              ShapeObservation &Entry = Observed.Shapes[3];
                              ++Entry.Calls;
                              Entry.Slots.push_back(SlotText(First));
                              Entry.Slots.push_back("string " + Second);
                              return MarkerOf(3, 2, 0);
                            })
          .IsSuccess();

  Registered = Registered &&
               Registry
                   .RegisterFunction(ShapeName(4),
                                     [&Observed](Luna::ArgumentView Arguments) {
                                       ShapeObservation &Entry =
                                           Observed.Shapes[4];
                                       ++Entry.Calls;
                                       RecordVariadic(Entry, Arguments);
                                       return MarkerOf(4, 0, Arguments.Size());
                                     })
                   .IsSuccess();

  Registered =
      Registered &&
      Registry
          .RegisterFunction(
              ShapeName(5),
              [&Observed](std::string First, Luna::ArgumentPack Arguments) {
                ShapeObservation &Entry = Observed.Shapes[5];
                ++Entry.Calls;
                Entry.Slots.push_back("string " + First);
                RecordVariadic(Entry, Arguments);
                return MarkerOf(5, 1, Arguments.Size());
              })
          .IsSuccess();

  Registered = Registered &&
               Registry
                   .RegisterFunction(
                       ShapeName(6),
                       [&Observed](int First, std::optional<int> Second,
                                   Luna::ArgumentView Arguments) {
                         ShapeObservation &Entry = Observed.Shapes[6];
                         ++Entry.Calls;
                         Entry.Slots.push_back(SlotText(First));
                         Entry.Slots.push_back(SlotText(Second));
                         RecordVariadic(Entry, Arguments);
                         return MarkerOf(6, Second ? 2 : 1, Arguments.Size());
                       })
                   .IsSuccess();

  Registered =
      Registered &&
      Registry
          .RegisterFunction(ShapeName(7),
                            Luna::WithDefaults(
                                [&Observed](int First, double Second) {
                                  ShapeObservation &Entry = Observed.Shapes[7];
                                  ++Entry.Calls;
                                  Entry.Slots.push_back(SlotText(First));
                                  Entry.Slots.push_back("number " +
                                                        NumberText(Second));
                                  return MarkerOf(7, 2, 0);
                                },
                                1, 2.5))
          .IsSuccess();

  Registered = Registered && Registry
                                 .RegisterFunction("Reuse",
                                                   [&Observed]() {
                                                     ++Observed.ReuseCalls;
                                                     return 77;
                                                   })
                                 .IsSuccess();
  return Registered;
}

enum class SampleKind {
  IntegralNumber,
  FractionalNumber,
  Text,
  Flag,
  Nil,
  Table
};

struct ArgumentSample final {
  std::string Literal;
  std::string LuauTypeName;
  SampleKind Kind = SampleKind::Nil;
  double Number = 0.0;
  std::string TextValue;
  bool Flag = false;
};

[[nodiscard]] const std::vector<ArgumentSample> &ArgumentPool() {
  static const std::vector<ArgumentSample> Pool{
      ArgumentSample{"7", "number", SampleKind::IntegralNumber, 7.0, {}, false},
      ArgumentSample{
          "-3", "number", SampleKind::IntegralNumber, -3.0, {}, false},
      ArgumentSample{
          "2.5", "number", SampleKind::FractionalNumber, 2.5, {}, false},
      ArgumentSample{"'abc'", "string", SampleKind::Text, 0.0, "abc", false},
      ArgumentSample{"''", "string", SampleKind::Text, 0.0, "", false},
      ArgumentSample{"true", "boolean", SampleKind::Flag, 0.0, {}, true},
      ArgumentSample{"false", "boolean", SampleKind::Flag, 0.0, {}, false},
      ArgumentSample{"nil", "nil", SampleKind::Nil, 0.0, {}, false},
      ArgumentSample{"{}", "table", SampleKind::Table, 0.0, {}, false}};
  return Pool;
}

[[nodiscard]] ArgumentSample MatchingSample(ValueKind Kind,
                                            ByteCursor &Cursor) {
  const std::vector<ArgumentSample> &Pool = ArgumentPool();
  switch (Kind) {
  case ValueKind::Integer:
    return Pool[Cursor.Pick(2)];
  case ValueKind::Number:
    return Pool[Cursor.Pick(3)];
  case ValueKind::String:
    return Pool[3 + Cursor.Pick(2)];
  case ValueKind::Boolean:
    return Pool[5 + Cursor.Pick(2)];
  }
  return Pool[7];
}

struct GeneratedCall final {
  std::size_t Shape = 0;
  std::vector<ArgumentSample> Arguments;
  std::string ArgumentList;
};

[[nodiscard]] GeneratedCall GenerateShapeCall(ByteCursor &Cursor) {
  GeneratedCall Call;
  Call.Shape = Cursor.Pick(ShapeCount);
  const std::vector<ModelParameter> &Parameters = ShapeParameters(Call.Shape);
  const ReferenceArity Arity = ShapeArity(Call.Shape);

  if (Cursor.Pick(3) != 0) {
    const std::size_t Supplied = Cursor.Pick(Arity.FixedCount + 1);
    for (std::size_t Index = 0; Index < Supplied; ++Index) {
      const ModelParameter &Parameter = Parameters[Index];
      if (Cursor.Pick(4) == 0)
        Call.Arguments.push_back(ArgumentPool()[7]);
      else
        Call.Arguments.push_back(MatchingSample(Parameter.Kind, Cursor));
    }
    if (Arity.IsVariadic && Supplied == Arity.FixedCount) {
      const std::size_t Extra = Cursor.Pick(4);
      for (std::size_t Index = 0; Index < Extra; ++Index) {
        Call.Arguments.push_back(Cursor.Pick(5) == 0
                                     ? ArgumentPool()[8]
                                     : ArgumentPool()[Cursor.Pick(8)]);
      }
    }
  } else {
    const std::size_t Count = Cursor.Pick(Arity.FixedCount + 3);
    for (std::size_t Index = 0; Index < Count; ++Index)
      Call.Arguments.push_back(ArgumentPool()[Cursor.Pick(9)]);
  }

  for (std::size_t Index = 0; Index < Call.Arguments.size(); ++Index) {
    if (Index != 0)
      Call.ArgumentList += ", ";
    Call.ArgumentList += Call.Arguments[Index].Literal;
  }
  return Call;
}

struct ModelCallOutcome final {
  bool Accepted = false;
  std::string Diagnostic;
  std::vector<std::string> Slots;
  std::vector<std::string> Variadic;
  std::size_t CommittingReads = 0;
  int Marker = 0;
};

[[nodiscard]] std::optional<std::string>
ModelRejection(ValueKind Kind, const ArgumentSample &Argument) {
  const std::string Expected(PublicTypeName(Kind));
  switch (Kind) {
  case ValueKind::Integer:
    if (Argument.Kind == SampleKind::IntegralNumber)
      return std::nullopt;
    if (Argument.Kind == SampleKind::FractionalNumber)
      return "expected an integral value but received " +
             DiagnosticNumberText(Argument.Number) + ".";
    break;
  case ValueKind::Number:
    if (Argument.Kind == SampleKind::IntegralNumber ||
        Argument.Kind == SampleKind::FractionalNumber)
      return std::nullopt;
    break;
  case ValueKind::String:
    if (Argument.Kind == SampleKind::Text)
      return std::nullopt;
    break;
  case ValueKind::Boolean:
    if (Argument.Kind == SampleKind::Flag)
      return std::nullopt;
    break;
  }
  return "expected " + Expected + " but received " + Argument.LuauTypeName +
         ".";
}

[[nodiscard]] std::string ModelSlotText(ValueKind Kind,
                                        const ArgumentSample &Argument) {
  switch (Kind) {
  case ValueKind::Integer:
    return "integer " + IntegerText(static_cast<int>(Argument.Number));
  case ValueKind::Number:
    return "number " + NumberText(Argument.Number);
  case ValueKind::String:
    return "string " + Argument.TextValue;
  case ValueKind::Boolean:
    break;
  }
  return "boolean " + FlagText(Argument.Flag);
}

[[nodiscard]] std::string ModelElementText(const ArgumentSample &Argument) {
  switch (Argument.Kind) {
  case SampleKind::IntegralNumber:
  case SampleKind::FractionalNumber:
    return "number " + NumberText(Argument.Number);
  case SampleKind::Text:
    return "string " + Argument.TextValue;
  case SampleKind::Flag:
    return "boolean " + FlagText(Argument.Flag);
  case SampleKind::Table:
    return "table";
  default:
    break;
  }
  return "nil";
}

[[nodiscard]] ModelCallOutcome ModelShapeCall(const GeneratedCall &Call) {
  ModelCallOutcome Outcome;
  const std::vector<ModelParameter> &Parameters = ShapeParameters(Call.Shape);
  const ReferenceArity Arity = ShapeArity(Call.Shape);
  const std::string Subject =
      "Callable '" + std::string(ShapeName(Call.Shape)) + "'";
  const std::size_t Received = Call.Arguments.size();

  if (Received < Arity.Minimum ||
      (!Arity.IsVariadic && Received > Arity.FixedCount)) {
    Outcome.Diagnostic = Subject + " expected " + ReferenceArityText(Arity) +
                         " but received " + std::to_string(Received) + ".";
    return Outcome;
  }

  std::vector<std::string> Slots(Arity.FixedCount, "omitted");
  for (std::size_t Index = 0; Index < Arity.FixedCount && Index < Received;
       ++Index) {
    const ModelParameter &Parameter = Parameters[Index];
    const ArgumentSample &Argument = Call.Arguments[Index];

    if (Parameter.AcceptsNil && Argument.Kind == SampleKind::Nil)
      continue;

    ++Outcome.CommittingReads;
    if (const std::optional<std::string> Rejection =
            ModelRejection(Parameter.Kind, Argument)) {
      Outcome.Diagnostic =
          Subject + " argument " + std::to_string(Index + 1) + " " + *Rejection;
      return Outcome;
    }
    Slots[Index] = ModelSlotText(Parameter.Kind, Argument);
  }

  if (Arity.IsVariadic) {
    for (std::size_t Position = Arity.FixedCount + 1; Position <= Received;
         ++Position) {
      const ArgumentSample &Argument = Call.Arguments[Position - 1];
      // A table variadic element is captured directly from the stack rather
      // than read through the scalar TypeRecord::Read path, so it never
      // records a committing argument read the way a scalar element does.
      if (Argument.Kind != SampleKind::Nil &&
          Argument.Kind != SampleKind::Table)
        ++Outcome.CommittingReads;
      Outcome.Variadic.push_back(std::to_string(Position) + ":" +
                                 ModelElementText(Argument));
    }
  }

  for (std::size_t Index = Received; Index < Arity.FixedCount; ++Index) {
    if (const std::optional<Luna::Value> &Default = Parameters[Index].Default)
      Slots[Index] = ValueText(*Default);
  }

  std::size_t Present = 0;
  for (const std::string &Slot : Slots) {
    if (Slot != "omitted")
      ++Present;
  }

  Outcome.Accepted = true;
  Outcome.Slots = std::move(Slots);
  Outcome.Marker = MarkerOf(Call.Shape, Present, Outcome.Variadic.size());
  return Outcome;
}

} // namespace

namespace {

struct PickObservation final {
  std::size_t RichCalls = 0;
  std::vector<std::string> RichSlots;
  std::size_t TextCalls = 0;
};

[[nodiscard]] bool RegisterPickCandidates(Luna::BindingRegistry &Registry,
                                          PickObservation &Observed) {
  const bool Rich =
      Registry
          .RegisterFunction(
              "Pick", Luna::WithDefaults(
                          [&Observed](int First, int Second) {
                            ++Observed.RichCalls;
                            Observed.RichSlots.push_back(SlotText(First));
                            Observed.RichSlots.push_back(SlotText(Second));
                            return 1;
                          },
                          5))
          .IsSuccess();
  const bool Text = Registry
                        .RegisterFunction("Pick",
                                          [&Observed](std::string) {
                                            ++Observed.TextCalls;
                                            return 2;
                                          })
                        .IsSuccess();
  return Rich && Text;
}

void VerifyDefaultAfterSelection(Luna::State &Owner,
                                 const PickObservation &Observed,
                                 ByteCursor &Cursor) {
  std::string Arguments;
  std::size_t ExpectedRich = 0;
  std::size_t ExpectedText = 0;
  std::vector<std::string> ExpectedSlots;
  std::size_t ExpectedReads = 0;
  int ExpectedMarker = 0;

  switch (Cursor.Pick(3)) {
  case 0:
    Arguments = "3";
    ExpectedRich = 1;
    ExpectedSlots = {"integer 3", "integer 5"};
    ExpectedReads = 1;
    ExpectedMarker = 1;
    break;
  case 1:
    Arguments = "'t'";
    ExpectedText = 1;
    ExpectedReads = 1;
    ExpectedMarker = 2;
    break;
  default:
    Arguments = "3, 4";
    ExpectedRich = 1;
    ExpectedSlots = {"integer 3", "integer 4"};
    ExpectedReads = 2;
    ExpectedMarker = 1;
    break;
  }

  Luna::Detail::ResetOverloadInstrumentation();
  const Luna::ExecutionResult Result =
      Owner.Execute("Chosen = Pick(" + Arguments + ")");
  const Luna::Detail::OverloadInstrumentationCounts Counts =
      Luna::Detail::OverloadInstrumentationTotals();

  RC_ASSERT(Result.IsSuccess());
  RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "Chosen") ==
            std::optional<int>(ExpectedMarker));
  RC_ASSERT(Observed.RichCalls == ExpectedRich);
  RC_ASSERT(Observed.TextCalls == ExpectedText);
  RC_ASSERT(Observed.RichSlots == ExpectedSlots);

  RC_ASSERT(Counts.ArgumentProbes == 2);
  RC_ASSERT(Counts.CommittingArgumentReads == ExpectedReads);
}

[[nodiscard]] std::string_view ReturnShapeName(std::size_t Shape) noexcept {
  switch (Shape) {
  case 0:
    return "Nothing";
  case 1:
    return "Twice";
  case 2:
    return "Split";
  case 3:
    return "Detail";
  case 4:
    return "Bundle";
  default:
    break;
  }
  return "Echo";
}

[[nodiscard]] bool RegisterReturnShapes(Luna::BindingRegistry &Registry) {
  const bool Zero =
      Registry.RegisterFunction(ReturnShapeName(0), [](int) {}).IsSuccess();
  const bool Scalar = Registry
                          .RegisterFunction(ReturnShapeName(1),
                                            [](int Value) { return Value * 2; })
                          .IsSuccess();
  const bool Pair = Registry
                        .RegisterFunction(ReturnShapeName(2),
                                          [](int Value) {
                                            return std::pair<int, std::string>(
                                                Value, std::to_string(Value));
                                          })
                        .IsSuccess();
  const bool Tuple =
      Registry
          .RegisterFunction(ReturnShapeName(3),
                            [](int Value) {
                              return std::tuple<bool, double, std::string>(
                                  Value > 0, static_cast<double>(Value) / 2.0,
                                  std::to_string(Value));
                            })
          .IsSuccess();

  const bool Dynamic =
      Registry
          .RegisterFunction(
              ReturnShapeName(4),
              [](int Count, int FailAt) {
                Luna::ReturnPack Pack;
                for (int Index = 1; Index <= Count; ++Index) {
                  if (Index == FailAt)
                    Pack.AppendText(std::string(
                        Luna::MaximumConversionStringBytes() + 1, 'x'));
                  else
                    Pack.AppendInteger(Index);
                }
                return Pack;
              })
          .IsSuccess();

  const bool Variadic =
      Registry
          .RegisterFunction(
              ReturnShapeName(5),
              [](Luna::ArgumentView Arguments) {
                Luna::ReturnPack Pack;
                for (std::size_t Index = 0; Index < Arguments.Size(); ++Index)
                  Pack.AppendInteger(
                      static_cast<int>(Arguments.Position(Index)));
                return Pack;
              })
          .IsSuccess();
  return Zero && Scalar && Pair && Tuple && Dynamic && Variadic;
}

struct GeneratedReturn final {
  std::size_t Shape = 0;
  std::string ArgumentList;
  std::vector<Luna::Value> Expected;
  bool Fails = false;
  std::string Diagnostic;
  bool HasFault = false;
  bool ConsumesFault = false;
  FaultPoint Fault = FaultPoint::ReturnWrite;
};

[[nodiscard]] std::string LuaLiteral(const Luna::Value &Source) {
  if (const bool *Flag = std::get_if<bool>(&Source))
    return FlagText(*Flag);
  if (const int *Whole = std::get_if<int>(&Source))
    return IntegerText(*Whole);
  if (const double *Number = std::get_if<double>(&Source)) {
    std::ostringstream Stream;
    Stream << std::setprecision(17) << *Number;
    return Stream.str();
  }
  if (const std::string *Text = std::get_if<std::string>(&Source))
    return "'" + *Text + "'";
  return "nil";
}

[[nodiscard]] GeneratedReturn GenerateReturn(ByteCursor &Cursor) {
  GeneratedReturn Case;
  Case.Shape = Cursor.Pick(6);
  const std::string Name(ReturnShapeName(Case.Shape));
  const std::string Prefix = "Internal error for callable '" + Name + "': ";
  const int Seed = static_cast<int>(Cursor.Pick(5)) - 1;

  switch (Case.Shape) {
  case 0:
    Case.ArgumentList = IntegerText(Seed);
    break;
  case 1:
    Case.ArgumentList = IntegerText(Seed);
    Case.Expected.push_back(Luna::Value(Seed * 2));
    break;
  case 2:
    Case.ArgumentList = IntegerText(Seed);
    Case.Expected.push_back(Luna::Value(Seed));
    Case.Expected.push_back(Luna::Value(std::to_string(Seed)));
    break;
  case 3:
    Case.ArgumentList = IntegerText(Seed);
    Case.Expected.push_back(Luna::Value(Seed > 0));
    Case.Expected.push_back(Luna::Value(static_cast<double>(Seed) / 2.0));
    Case.Expected.push_back(Luna::Value(std::to_string(Seed)));
    break;
  case 4: {
    const std::size_t Count = Cursor.Pick(4);
    const std::size_t FailAt = Cursor.Pick(Count + 2);
    Case.ArgumentList = std::to_string(Count) + ", " + std::to_string(FailAt);
    if (FailAt >= 1 && FailAt <= Count) {
      Case.Fails = true;
      Case.Diagnostic = Prefix + "Return value " + std::to_string(FailAt) +
                        " exceeds the " +
                        std::to_string(Luna::MaximumConversionStringBytes()) +
                        "-byte maximum.";
    } else {
      for (std::size_t Index = 1; Index <= Count; ++Index)
        Case.Expected.push_back(Luna::Value(static_cast<int>(Index)));
    }
    break;
  }
  default: {
    const std::size_t Count = Cursor.Pick(4);
    for (std::size_t Index = 1; Index <= Count; ++Index) {
      if (Index != 1)
        Case.ArgumentList += ", ";
      Case.ArgumentList += "true";
      Case.Expected.push_back(Luna::Value(static_cast<int>(Index)));
    }
    break;
  }
  }

  const std::size_t Choice = Cursor.Pick(4);
  if (Choice == 0)
    return Case;

  Case.HasFault = true;
  Case.Fault = Choice == 1   ? FaultPoint::ReturnStackCapacity
               : Choice == 2 ? FaultPoint::ReturnWrite
                             : FaultPoint::VoidFinalization;

  const bool IsVoid = Case.Shape == 0;
  const bool Applies = IsVoid == (Case.Fault == FaultPoint::VoidFinalization);
  if (!Applies || Case.Fails)
    return Case;

  Case.Fails = true;
  Case.ConsumesFault = true;
  switch (Case.Fault) {
  case FaultPoint::ReturnStackCapacity:
    Case.Diagnostic =
        Case.Shape == 1
            ? Prefix + "Could not reserve stack capacity for return value."
            : Prefix + "Could not reserve stack capacity for " +
                  std::to_string(Case.Expected.size()) + " return values.";
    break;
  case FaultPoint::ReturnWrite:
    Case.Diagnostic = Prefix + "Injected internal return-writer failure.";
    break;
  default:
    Case.Diagnostic = Prefix + "Injected internal void-finalization failure.";
    break;
  }
  return Case;
}

void VerifyReturnPublication(Luna::State &Owner, const GeneratedReturn &Case) {
  std::string Condition = "true";
  for (std::size_t Index = 0; Index < Case.Expected.size(); ++Index) {
    Condition += " and Packed[" + std::to_string(Index + 1) +
                 "] == " + LuaLiteral(Case.Expected[Index]);
  }

  const std::string Source =
      "local Packed = {" + std::string(ReturnShapeName(Case.Shape)) + "(" +
      Case.ArgumentList + ")}\nPublished = #Packed\nMatched = 0\nif " +
      Condition + " then Matched = 1 end\n";

  RC_ASSERT(Hooks::SetIntegerGlobal(Owner, "ReturnCanary", 23));
  const std::optional<int> EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  RC_ASSERT(EntryDepth.has_value());
  if (Case.HasFault)
    Hooks::InjectFault(Owner, Case.Fault, 1);

  const Luna::ExecutionResult Result = Owner.Execute(Source);
  RC_TAG(std::string(Case.Fails ? "return refused" : "return published"));

  if (!Case.Fails) {
    RC_ASSERT(Result.IsSuccess());
    RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "Published") ==
              std::optional<int>(static_cast<int>(Case.Expected.size())));
    RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "Matched") ==
              std::optional<int>(1));
  } else {
    RC_ASSERT(!Result.IsSuccess());
    const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
    RC_ASSERT(Diagnostic != nullptr);
    RC_ASSERT(Diagnostic->Message().find(Case.Diagnostic) != std::string::npos);
    RC_ASSERT(!Hooks::ObserveIntegerGlobal(Owner, "Published").has_value());
    RC_ASSERT(!Hooks::ObserveIntegerGlobal(Owner, "Matched").has_value());
    RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "ReturnCanary") ==
              std::optional<int>(23));

    const auto Restoration = Hooks::ObserveLastCallbackStackRestoration(Owner);
    RC_ASSERT(Restoration.has_value());
    RC_ASSERT(Restoration->EntryDepth == Restoration->RestoredDepth);
    RC_ASSERT(Restoration->ErrorDepth == Restoration->RestoredDepth + 1);
  }

  RC_ASSERT(Hooks::ObserveRootStackDepth(Owner) == EntryDepth);
  if (!Case.HasFault)
    return;

  RC_ASSERT(Hooks::PendingFaults(Owner, Case.Fault) ==
            (Case.ConsumesFault ? 0U : 1U));
  static_cast<void>(Hooks::ConsumeFault(Owner, Case.Fault));
}

} // namespace

namespace {

void VerifyShapeCall(Luna::State &Owner, const CallObservation &Observed,
                     const GeneratedCall &Call, const ModelCallOutcome &Model) {
  RC_ASSERT(Hooks::SetIntegerGlobal(Owner, "CallCanary", 11));
  const std::optional<int> EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  RC_ASSERT(EntryDepth.has_value());

  Luna::Detail::ResetOverloadInstrumentation();
  const Luna::ExecutionResult Result =
      Owner.Execute("Marker = " + std::string(ShapeName(Call.Shape)) + "(" +
                    Call.ArgumentList + ")");
  const Luna::Detail::OverloadInstrumentationCounts Counts =
      Luna::Detail::OverloadInstrumentationTotals();
  RC_TAG(std::string(Model.Accepted ? "call accepted" : "call refused"));

  RC_ASSERT(Counts.ArgumentProbes == 0);
  RC_ASSERT(Counts.CommittingArgumentReads == Model.CommittingReads);

  const ShapeObservation &Entry = Observed.Shapes[Call.Shape];
  if (Model.Accepted) {
    RC_ASSERT(Result.IsSuccess());
    RC_ASSERT(Entry.Calls == 1);

    RC_ASSERT(Entry.Slots == Model.Slots);

    RC_ASSERT(Entry.Variadic == Model.Variadic);
    RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "Marker") ==
              std::optional<int>(Model.Marker));
  } else {
    RC_ASSERT(!Result.IsSuccess());
    const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic();
    RC_ASSERT(Diagnostic != nullptr);
    RC_ASSERT(Diagnostic->Message().find(Model.Diagnostic) !=
              std::string::npos);
    RC_ASSERT(Entry.Calls == 0);
    RC_ASSERT(Entry.Slots.empty());
    RC_ASSERT(Entry.Variadic.empty());
    RC_ASSERT(!Hooks::ObserveIntegerGlobal(Owner, "Marker").has_value());
    RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "CallCanary") ==
              std::optional<int>(11));

    const auto Restoration = Hooks::ObserveLastCallbackStackRestoration(Owner);
    RC_ASSERT(Restoration.has_value());
    RC_ASSERT(Restoration->EntryDepth == Restoration->RestoredDepth);
    RC_ASSERT(Restoration->ErrorDepth == Restoration->RestoredDepth + 1);
  }

  for (std::size_t Shape = 0; Shape < ShapeCount; ++Shape) {
    if (Shape == Call.Shape)
      continue;
    RC_ASSERT(Observed.Shapes[Shape].Calls == 0);
    RC_ASSERT(Observed.Shapes[Shape].Slots.empty());
  }
  RC_ASSERT(Observed.ReuseCalls == 0);
  RC_ASSERT(Hooks::ObserveRootStackDepth(Owner) == EntryDepth);
}

void VerifyGeneratedInvocation(ByteCursor &Calls, ByteCursor &Returns) {
  const GeneratedCall Call = GenerateShapeCall(Calls);
  const ModelCallOutcome Model = ModelShapeCall(Call);
  const GeneratedReturn ReturnCase = GenerateReturn(Returns);

  CallObservation Observed;
  PickObservation Picked;

  Luna::State Owner;
  RC_ASSERT(Owner.IsReady());
  Luna::BindingRegistry Registry = Owner.Bindings();
  RC_ASSERT(RegisterShapes(Registry, Observed));
  RC_ASSERT(RegisterPickCandidates(Registry, Picked));
  RC_ASSERT(RegisterReturnShapes(Registry));

  VerifyShapeCall(Owner, Observed, Call, Model);
  VerifyDefaultAfterSelection(Owner, Picked, Calls);
  VerifyReturnPublication(Owner, ReturnCase);

  RC_ASSERT(Owner.Execute("Recovered = Reuse()").IsSuccess());
  RC_ASSERT(Observed.ReuseCalls == 1);
  RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "Recovered") ==
            std::optional<int>(77));

  Luna::BindingRegistry Later = Owner.Bindings();
  RC_ASSERT(Later.RegisterFunction("Later", [](int Value) { return Value + 1; })
                .IsSuccess());
  RC_ASSERT(Owner.Execute("assert(Later(1) == 2, 'reuse')").IsSuccess());
}

} // namespace

int RunRichSignatureShapeProperties() {
  // clang-format off
  // Feature: reflection-driven-binding-system, Property 25: Optional, defaulted, variadic, and multiple-value calls follow their reflected shapes
  const bool Passed = rc::check(
      // clang-format on
      "Optional, defaulted, variadic, and multiple-value calls follow their "
      "reflected shapes",
      [](const std::vector<std::uint8_t> &ShapeBytes,
         const std::vector<std::uint8_t> &CallBytes,
         const std::vector<std::uint8_t> &ReturnBytes) {
        ByteCursor Shapes(ShapeBytes);
        VerifyDeclaredShapeModel(Shapes);

        ByteCursor Calls(CallBytes);
        ByteCursor Returns(ReturnBytes);
        VerifyGeneratedInvocation(Calls, Returns);
      });
  return Passed ? 0 : 1;
}
