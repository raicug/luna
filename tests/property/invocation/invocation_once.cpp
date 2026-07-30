// clang-format off
#include <luna/luna.hpp>

#include <rapidcheck.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>
// clang-format on

namespace {

[[nodiscard]] std::string BooleanLiteral(bool Value) {
  return Value ? "true" : "false";
}

[[nodiscard]] std::string NumberLiteral(double Value) {
  std::array<char, 64> Buffer{};
  const auto Result =
      std::to_chars(Buffer.data(), Buffer.data() + Buffer.size(), Value,
                    std::chars_format::general);
  if (Result.ec != std::errc{})
    return "0.0";
  return std::string(Buffer.data(), Result.ptr);
}

inline constexpr std::size_t MaximumGeneratedBytes = 64;

[[nodiscard]] std::size_t
GeneratedSize(const std::vector<std::uint8_t> &Bytes) noexcept {
  return Bytes.size() < MaximumGeneratedBytes ? Bytes.size()
                                              : MaximumGeneratedBytes;
}

[[nodiscard]] std::string
GeneratedText(const std::vector<std::uint8_t> &Bytes) {
  std::string Result;
  Result.reserve(GeneratedSize(Bytes));
  for (std::size_t Index = 0; Index < GeneratedSize(Bytes); ++Index)
    Result.push_back(static_cast<char>(Bytes[Index]));
  return Result;
}

[[nodiscard]] std::string ByteString(const std::vector<std::uint8_t> &Bytes) {
  constexpr char Hex[] = "0123456789abcdef";
  const std::size_t Size = GeneratedSize(Bytes);

  std::string Result;
  Result.reserve(2 + Size * 4);
  Result.push_back('"');
  for (std::size_t Index = 0; Index < Size; ++Index) {
    const std::uint8_t Byte = Bytes[Index];
    Result += "\\x";
    Result.push_back(Hex[Byte >> 4U]);
    Result.push_back(Hex[Byte & 0x0fU]);
  }
  Result.push_back('"');
  return Result;
}

} // namespace

int RunSuccessfulValidationExactlyOnceProperties() {

  const bool Passed = rc::check(

      "Successful validation invokes exactly once",
      [](bool GeneratedBoolean, std::int32_t GeneratedInteger,
         std::int32_t GeneratedNumberEighths,
         const std::vector<std::uint8_t> &GeneratedBytes) {
        const int Integer = static_cast<int>(GeneratedInteger);
        const double Number = static_cast<double>(GeneratedNumberEighths) / 8.0;
        const std::string Text = ByteString(GeneratedBytes);
        const std::string ExpectedText = GeneratedText(GeneratedBytes);

        int ZeroCalls = 0;
        int BooleanCalls = 0;
        int IntegerCalls = 0;
        int NumberCalls = 0;
        int StringCalls = 0;
        int PairCalls = 0;
        int TripleCalls = 0;
        int QuadCalls = 0;

        bool ObservedBoolean = !GeneratedBoolean;
        int ObservedInteger = 0;
        double ObservedNumber = 0.0;
        std::string ObservedString;
        bool ObservedPairBoolean = !GeneratedBoolean;
        int ObservedPairInteger = 0;
        double ObservedTripleNumber = 0.0;
        std::string ObservedTripleString;
        bool ObservedTripleBoolean = !GeneratedBoolean;
        bool ObservedQuadBoolean = !GeneratedBoolean;
        int ObservedQuadInteger = 0;
        double ObservedQuadNumber = 0.0;
        std::string ObservedQuadString;

        Luna::State State;
        RC_ASSERT(State.IsReady());
        RC_ASSERT(State.Bindings()
                      .Register("InvokeZero", [&ZeroCalls]() { ++ZeroCalls; })
                      .IsSuccess());
        RC_ASSERT(State.Bindings()
                      .Register("InvokeBoolean",
                                [&](bool Value) {
                                  ++BooleanCalls;
                                  ObservedBoolean = Value;
                                  return Value;
                                })
                      .IsSuccess());
        RC_ASSERT(State.Bindings()
                      .Register("InvokeInteger",
                                [&](int Value) {
                                  ++IntegerCalls;
                                  ObservedInteger = Value;
                                  return Value;
                                })
                      .IsSuccess());
        RC_ASSERT(State.Bindings()
                      .Register("InvokeNumber",
                                [&](double Value) {
                                  ++NumberCalls;
                                  ObservedNumber = Value;
                                  return Value;
                                })
                      .IsSuccess());
        RC_ASSERT(State.Bindings()
                      .Register("InvokeString",
                                [&](std::string Value) {
                                  ++StringCalls;
                                  ObservedString = Value;
                                  return Value;
                                })
                      .IsSuccess());
        RC_ASSERT(State.Bindings()
                      .Register("InvokePair",
                                [&](bool Flag, int Value) {
                                  ++PairCalls;
                                  ObservedPairBoolean = Flag;
                                  ObservedPairInteger = Value;
                                  return Flag ? static_cast<double>(Value)
                                              : 0.0;
                                })
                      .IsSuccess());
        RC_ASSERT(State.Bindings()
                      .Register("InvokeTriple",
                                [&](double Value, std::string StringValue,
                                    bool Flag) {
                                  ++TripleCalls;
                                  ObservedTripleNumber = Value;
                                  ObservedTripleString = StringValue;
                                  ObservedTripleBoolean = Flag;
                                  return StringValue;
                                })
                      .IsSuccess());
        RC_ASSERT(
            State.Bindings()
                .Register("InvokeQuad",
                          [&](bool Flag, int IntegerValue, double NumberValue,
                              std::string StringValue) {
                            ++QuadCalls;
                            ObservedQuadBoolean = Flag;
                            ObservedQuadInteger = IntegerValue;
                            ObservedQuadNumber = NumberValue;
                            ObservedQuadString = StringValue;
                          })
                .IsSuccess());

        const std::string Boolean = BooleanLiteral(GeneratedBoolean);
        const std::string IntegerSource = std::to_string(Integer);
        const std::string NumberSource = NumberLiteral(Number);
        const std::string Source =
            "InvokeZero()\nInvokeBoolean(" + Boolean + ")\nInvokeInteger(" +
            IntegerSource + ")\nInvokeNumber(" + NumberSource +
            ")\nInvokeString(" + Text + ")\nInvokePair(" + Boolean + "," +
            IntegerSource + ")\nInvokeTriple(" + NumberSource + "," + Text +
            "," + Boolean + ")\nInvokeQuad(" + Boolean + "," + IntegerSource +
            "," + NumberSource + "," + Text + ")";

        const auto Execution = State.Execute(Source);
        RC_ASSERT(Execution.IsSuccess());
        RC_ASSERT(ZeroCalls == 1);
        RC_ASSERT(BooleanCalls == 1);
        RC_ASSERT(IntegerCalls == 1);
        RC_ASSERT(NumberCalls == 1);
        RC_ASSERT(StringCalls == 1);
        RC_ASSERT(PairCalls == 1);
        RC_ASSERT(TripleCalls == 1);
        RC_ASSERT(QuadCalls == 1);
        RC_ASSERT(ObservedBoolean == GeneratedBoolean);
        RC_ASSERT(ObservedInteger == Integer);
        RC_ASSERT(ObservedNumber == Number);
        RC_ASSERT(ObservedString == ExpectedText);
        RC_ASSERT(ObservedPairBoolean == GeneratedBoolean);
        RC_ASSERT(ObservedPairInteger == Integer);
        RC_ASSERT(ObservedTripleNumber == Number);
        RC_ASSERT(ObservedTripleString == ExpectedText);
        RC_ASSERT(ObservedTripleBoolean == GeneratedBoolean);
        RC_ASSERT(ObservedQuadBoolean == GeneratedBoolean);
        RC_ASSERT(ObservedQuadInteger == Integer);
        RC_ASSERT(ObservedQuadNumber == Number);
        RC_ASSERT(ObservedQuadString == ExpectedText);
      });

  return Passed ? 0 : 1;
}
