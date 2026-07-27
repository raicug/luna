// clang-format off
#include "state/invocation/overload/probe.hpp"

#include <luna/binding/conversion.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/invocation/overload/instrumentation.hpp"
#include "state/type/conversion_outcome.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"

#include <lua.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] std::string FormatNumber(double Number) {
  if (std::isnan(Number))
    return "NaN";
  if (std::isinf(Number))
    return std::signbit(Number) ? "negative infinity" : "positive infinity";

  std::ostringstream Stream;
  Stream << std::setprecision(std::numeric_limits<double>::max_digits10)
         << Number;
  return Stream.str();
}

[[nodiscard]] ArgumentProbe Rejected(std::string Reason) {
  ArgumentProbe Probe;
  Probe.IsViable = false;
  Probe.Rejection = std::move(Reason);
  return Probe;
}

[[nodiscard]] ArgumentProbe Viable(ConversionRank Rank) {
  ArgumentProbe Probe;
  Probe.IsViable = true;
  Probe.Rank = Rank;
  return Probe;
}

[[nodiscard]] LuauRepresentation
ObservedRepresentation(lua_State *State, int StackIndex) noexcept {
  switch (lua_type(State, StackIndex)) {
  case LUA_TNIL:
    return LuauRepresentation::Nil;
  case LUA_TBOOLEAN:
    return LuauRepresentation::Boolean;
  case LUA_TNUMBER:
    return LuauRepresentation::Number;
  case LUA_TSTRING:
    return LuauRepresentation::String;
  case LUA_TTABLE:
    return LuauRepresentation::Table;
  case LUA_TUSERDATA:
  case LUA_TLIGHTUSERDATA:
    return LuauRepresentation::Userdata;
  case LUA_TFUNCTION:
    return LuauRepresentation::Function;
  default:
    return LuauRepresentation::None;
  }
}

[[nodiscard]] constexpr ConversionRank
DeclaredRank(ConversionRankCategory Category) noexcept {
  switch (Category) {
  case ConversionRankCategory::Exact:
    return ConversionRank::Exact;
  case ConversionRankCategory::SafeBuiltIn:
    return ConversionRank::SafeBuiltIn;
  case ConversionRankCategory::User:
    return ConversionRank::User;
  }
  return ConversionRank::User;
}

struct NumberDomain final {
  bool IsFinite = false;
  bool IsIntegral = false;
  bool FitsSigned32 = false;
  double Value = 0.0;
};

[[nodiscard]] NumberDomain ClassifyNumber(lua_State *State, int StackIndex) {
  NumberDomain Domain;
  Domain.Value = lua_tonumberx(State, StackIndex, nullptr);
  Domain.IsFinite = std::isfinite(Domain.Value);
  if (!Domain.IsFinite)
    return Domain;

  constexpr double Minimum =
      static_cast<double>(std::numeric_limits<std::int32_t>::min());
  constexpr double Maximum =
      static_cast<double>(std::numeric_limits<std::int32_t>::max());
  Domain.IsIntegral = std::trunc(Domain.Value) == Domain.Value;
  Domain.FitsSigned32 = Domain.Value >= Minimum && Domain.Value <= Maximum;
  return Domain;
}

[[nodiscard]] std::optional<std::string>
RejectNumberDomain(const TypeRecord &Record, lua_State *State, int StackIndex) {
  const bool WantsInteger = Record.ValueRepresentation.has_value() &&
                            *Record.ValueRepresentation == ValueKind::Integer;
  const bool WantsEnumerator = Record.Enumeration.has_value();
  if (!WantsInteger && !WantsEnumerator)
    return std::nullopt;

  const NumberDomain Domain = ClassifyNumber(State, StackIndex);
  if (!Domain.IsFinite)
    return "expected a finite signed 32-bit integer but received " +
           FormatNumber(Domain.Value);
  if (!Domain.FitsSigned32)
    return "expected signed 32-bit range [-2147483648, 2147483647] but "
           "received " +
           FormatNumber(Domain.Value);
  if (!Domain.IsIntegral)
    return "expected an integral value but received " +
           FormatNumber(Domain.Value);

  if (WantsEnumerator &&
      !Record.Enumeration->Accepts(static_cast<std::int64_t>(Domain.Value)))
    return "received the undeclared enumeration value " +
           FormatNumber(Domain.Value);
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string>
RejectStringDomain(const TypeRecord &Record, lua_State *State, int StackIndex) {
  if (!Record.MaximumByteCount)
    return std::nullopt;

  std::size_t Length = 0;
  static_cast<void>(lua_tolstring(State, StackIndex, &Length));
  if (Length <= *Record.MaximumByteCount)
    return std::nullopt;
  return "received " + std::to_string(Length) + " string bytes; maximum is " +
         std::to_string(*Record.MaximumByteCount);
}

} // namespace

std::string ReceivedTypeName(lua_State *State, int StackIndex) {
  if (!State)
    return "unknown";
  const char *Name = lua_typename(State, lua_type(State, StackIndex));
  return Name ? Name : "unknown";
}

TypeDescriptor CanonicalReceivedType(lua_State *State, int StackIndex) {
  if (!State)
    return TypeDescriptor::Unsupported();

  switch (ObservedRepresentation(State, StackIndex)) {
  case LuauRepresentation::Nil:
    return TypeDescriptor::ForFixed(FixedTypeKey::Null);
  case LuauRepresentation::Boolean:
    return TypeDescriptor::ForFixed(FixedTypeKey::Boolean);
  case LuauRepresentation::String:
    return TypeDescriptor::ForFixed(FixedTypeKey::String);
  case LuauRepresentation::Number: {
    const NumberDomain Domain = ClassifyNumber(State, StackIndex);
    if (Domain.IsFinite && Domain.IsIntegral && Domain.FitsSigned32)
      return TypeDescriptor::ForFixed(FixedTypeKey::Int32);
    return TypeDescriptor::ForFixed(FixedTypeKey::Double);
  }
  default:
    return TypeDescriptor::Unsupported();
  }
}

ArgumentProbe ProbeArgument(const TypeGeneration &Types, lua_State *State,
                            int StackIndex, const TypeDescriptor &Target) {
  RecordArgumentProbe();

  if (!State)
    return Rejected("could not be inspected");

  const TypeRecord *Record = Types.Find(Target);
  if (!Record || Record->IsVoid() || !Record->IsReadable ||
      (!Record->Read && !Record->StructuredRead))
    return Rejected("has no available conversion to " +
                    CanonicalTypeText(Target));

  const std::string Expected = Record->PublicName.empty()
                                   ? CanonicalTypeText(Target)
                                   : Record->PublicName;
  const LuauRepresentation Observed = ObservedRepresentation(State, StackIndex);

  if (Observed == LuauRepresentation::Nil &&
      Record->Representation != LuauRepresentation::Nil) {
    if (!Record->IsNullable)
      return Rejected("expected " + Expected + " but received nil");
  } else if (Observed != Record->Representation) {
    return Rejected("expected " + Expected + " but received " +
                    ReceivedTypeName(State, StackIndex));
  }

  if (Observed == LuauRepresentation::Number) {
    if (auto Rejection = RejectNumberDomain(*Record, State, StackIndex))
      return Rejected(std::move(*Rejection));
  } else if (Observed == LuauRepresentation::String) {
    if (auto Rejection = RejectStringDomain(*Record, State, StackIndex))
      return Rejected(std::move(*Rejection));
  }

  const TypeDescriptor Natural = CanonicalReceivedType(State, StackIndex);
  if (Natural.IsValid() && Natural == Target)
    return Viable(ConversionRank::Exact);
  if (!Natural.IsValid())
    return Viable(DeclaredRank(Record->Rank));
  if (Record->Rank == ConversionRankCategory::User)
    return Viable(ConversionRank::User);
  return Viable(ConversionRank::SafeBuiltIn);
}

} // namespace Luna::Detail
