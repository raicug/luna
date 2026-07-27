// clang-format off
#include <luna/luna.hpp>

#include <rapidcheck.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
// clang-format on

namespace {

inline constexpr std::size_t MaximumStringBytes = 1'048'576;
inline constexpr std::uint64_t DoubleExponentMask = 0x7ff0000000000000ULL;

enum class DoubleCategory : std::uint8_t {
  Finite,
  PositiveZero,
  NegativeZero,
  PositiveInfinity,
  NegativeInfinity,
  NaN,
  NegativeNaN,
  Count
};

enum class StringCategory : std::uint8_t { Maximum, EmbeddedZero, Generated };

[[nodiscard]] DoubleCategory NormalizeDoubleCategory(std::uint8_t Value) {
  return static_cast<DoubleCategory>(
      Value % static_cast<std::uint8_t>(DoubleCategory::Count));
}

[[nodiscard]] StringCategory NormalizeStringCategory(std::uint8_t Value) {
  if (Value == 0)
    return StringCategory::Maximum;
  if (Value % 8 == 1)
    return StringCategory::EmbeddedZero;
  return StringCategory::Generated;
}

[[nodiscard]] double SelectDouble(std::uint64_t Bits,
                                  DoubleCategory Category) noexcept {
  switch (Category) {
  case DoubleCategory::Finite: {
    const std::uint64_t Exponent = (Bits & DoubleExponentMask) >> 52U;
    const std::uint64_t FiniteExponent = Exponent % 0x7ffULL;
    return std::bit_cast<double>((Bits & ~DoubleExponentMask) |
                                 (FiniteExponent << 52U));
  }
  case DoubleCategory::PositiveZero:
    return 0.0;
  case DoubleCategory::NegativeZero:
    return -0.0;
  case DoubleCategory::PositiveInfinity:
    return std::numeric_limits<double>::infinity();
  case DoubleCategory::NegativeInfinity:
    return -std::numeric_limits<double>::infinity();
  case DoubleCategory::NaN:
    return std::numeric_limits<double>::quiet_NaN();
  case DoubleCategory::NegativeNaN:
    return -std::numeric_limits<double>::quiet_NaN();
  case DoubleCategory::Count:
    break;
  }
  return 0.0;
}

[[nodiscard]] std::string
MakeGeneratedString(const std::vector<std::uint8_t> &Bytes) {
  std::string Result;
  Result.reserve(Bytes.size());
  for (const auto Byte : Bytes)
    Result.push_back(static_cast<char>(Byte));
  return Result;
}

[[nodiscard]] std::string SelectString(const std::vector<std::uint8_t> &Bytes,
                                       StringCategory Category) {
  if (Category == StringCategory::Maximum) {
    std::string Result(MaximumStringBytes, '\0');
    for (std::size_t Index = 0; Index < Result.size(); ++Index)
      Result[Index] = static_cast<char>(Index & 0xffU);
    return Result;
  }

  std::string Result = MakeGeneratedString(Bytes);
  if (Category == StringCategory::EmbeddedZero) {
    Result.insert(
        Result.begin() + static_cast<std::ptrdiff_t>(Result.size() / 2), '\0');
  }
  return Result;
}

[[nodiscard]] bool EquivalentDouble(double Expected, double Actual) noexcept {
  if (std::isnan(Expected))
    return std::isnan(Actual);
  if (std::isinf(Expected))
    return std::isinf(Actual) && std::signbit(Expected) == std::signbit(Actual);
  if (Expected == 0.0)
    return Actual == 0.0 && std::signbit(Expected) == std::signbit(Actual);
  return std::isfinite(Actual) && Actual == Expected;
}

} // namespace

int RunSupportedValueRoundTripProperties() {
  // clang-format off
  const bool Passed = rc::check(
      // clang-format on
      "Supported values round-trip equivalently",
      [](bool GeneratedBoolean, std::int32_t GeneratedInteger,
         std::uint64_t GeneratedDoubleBits,
         std::uint8_t GeneratedDoubleCategory,
         const std::vector<std::uint8_t> &GeneratedBytes,
         std::uint8_t GeneratedStringCategory) {
        static_assert(std::numeric_limits<int>::min() ==
                      std::numeric_limits<std::int32_t>::min());
        static_assert(std::numeric_limits<int>::max() ==
                      std::numeric_limits<std::int32_t>::max());

        const int ExpectedInteger = static_cast<int>(GeneratedInteger);
        const DoubleCategory NumberCategory =
            NormalizeDoubleCategory(GeneratedDoubleCategory);
        const double ExpectedDouble =
            SelectDouble(GeneratedDoubleBits, NumberCategory);
        const StringCategory TextCategory =
            NormalizeStringCategory(GeneratedStringCategory);
        const std::string ExpectedString =
            SelectString(GeneratedBytes, TextCategory);

        bool ObservedBoolean = !GeneratedBoolean;
        int ObservedInteger = 0;
        double ObservedDouble = 0.0;
        std::string ObservedString;

        Luna::State State;
        RC_ASSERT(State.IsReady());
        RC_ASSERT(
            State.Bindings()
                .Register("ProduceBoolean", [&]() { return GeneratedBoolean; })
                .IsSuccess());
        RC_ASSERT(
            State.Bindings()
                .Register("IdentityBoolean", [](bool Value) { return Value; })
                .IsSuccess());
        RC_ASSERT(State.Bindings()
                      .Register("ObserveBoolean",
                                [&](bool Value) { ObservedBoolean = Value; })
                      .IsSuccess());
        RC_ASSERT(
            State.Bindings()
                .Register("ProduceInteger", [&]() { return ExpectedInteger; })
                .IsSuccess());
        RC_ASSERT(
            State.Bindings()
                .Register("IdentityInteger", [](int Value) { return Value; })
                .IsSuccess());
        RC_ASSERT(State.Bindings()
                      .Register("ObserveInteger",
                                [&](int Value) { ObservedInteger = Value; })
                      .IsSuccess());
        RC_ASSERT(
            State.Bindings()
                .Register("ProduceDouble", [&]() { return ExpectedDouble; })
                .IsSuccess());
        RC_ASSERT(
            State.Bindings()
                .Register("IdentityDouble", [](double Value) { return Value; })
                .IsSuccess());
        RC_ASSERT(State.Bindings()
                      .Register("ObserveDouble",
                                [&](double Value) { ObservedDouble = Value; })
                      .IsSuccess());
        RC_ASSERT(
            State.Bindings()
                .Register("ProduceString", [&]() { return ExpectedString; })
                .IsSuccess());
        RC_ASSERT(State.Bindings()
                      .Register("IdentityString",
                                [](std::string Value) { return Value; })
                      .IsSuccess());
        RC_ASSERT(State.Bindings()
                      .Register("ObserveString",
                                [&](std::string Value) {
                                  ObservedString = std::move(Value);
                                })
                      .IsSuccess());

        const auto Execution =
            State.Execute("ObserveBoolean(IdentityBoolean(ProduceBoolean()))\n"
                          "ObserveInteger(IdentityInteger(ProduceInteger()))\n"
                          "ObserveDouble(IdentityDouble(ProduceDouble()))\n"
                          "ObserveString(IdentityString(ProduceString()))\n");

        RC_ASSERT(Execution.IsSuccess());
        RC_ASSERT(ObservedBoolean == GeneratedBoolean);
        RC_ASSERT(ObservedInteger == ExpectedInteger);
        RC_ASSERT(EquivalentDouble(ExpectedDouble, ObservedDouble));
        RC_ASSERT(ObservedString == ExpectedString);

        if (TextCategory == StringCategory::EmbeddedZero)
          RC_ASSERT(ObservedString.find('\0') != std::string::npos);
        if (TextCategory == StringCategory::Maximum)
          RC_ASSERT(ObservedString.size() == MaximumStringBytes);
      });

  return Passed ? 0 : 1;
}
