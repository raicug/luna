// clang-format off
#include <luna/luna.hpp>

#include <rapidcheck.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
// clang-format on

namespace {

constexpr std::string_view GlobalName = "WrongArgumentCount";

enum class SignatureKind : std::uint32_t {
  ZeroVoid,
  BooleanToBoolean,
  IntegerToInteger,
  NumberToNumber,
  StringToString,
  BooleanIntegerToNumber,
  NumberStringBooleanToString,
  AllToVoid,
  Count
};

[[nodiscard]] SignatureKind NormalizeSignature(int Value) noexcept {
  return static_cast<SignatureKind>(
      static_cast<std::uint32_t>(Value) %
      static_cast<std::uint32_t>(SignatureKind::Count));
}

[[nodiscard]] std::size_t ExpectedCount(SignatureKind Signature) noexcept {
  switch (Signature) {
  case SignatureKind::ZeroVoid:
    return 0;
  case SignatureKind::BooleanToBoolean:
  case SignatureKind::IntegerToInteger:
  case SignatureKind::NumberToNumber:
  case SignatureKind::StringToString:
    return 1;
  case SignatureKind::BooleanIntegerToNumber:
    return 2;
  case SignatureKind::NumberStringBooleanToString:
    return 3;
  case SignatureKind::AllToVoid:
    return 4;
  case SignatureKind::Count:
    break;
  }
  return 0;
}

[[nodiscard]] std::size_t
NormalizeReceivedCount(int Value, std::size_t Expected) noexcept {
  constexpr std::size_t ReceivedCountDomain = 7;
  std::size_t Received =
      static_cast<std::uint32_t>(Value) % ReceivedCountDomain;
  if (Received == Expected)
    Received = (Received + 1) % ReceivedCountDomain;
  return Received;
}

[[nodiscard]] Luna::RegistrationResult
RegisterSignature(Luna::State &State, SignatureKind Signature, int &Calls) {
  switch (Signature) {
  case SignatureKind::ZeroVoid:
    return State.Bindings().Register(GlobalName, [&Calls]() { ++Calls; });
  case SignatureKind::BooleanToBoolean:
    return State.Bindings().Register(GlobalName, [&Calls](bool Value) {
      ++Calls;
      return Value;
    });
  case SignatureKind::IntegerToInteger:
    return State.Bindings().Register(GlobalName, [&Calls](int Value) {
      ++Calls;
      return Value;
    });
  case SignatureKind::NumberToNumber:
    return State.Bindings().Register(GlobalName, [&Calls](double Value) {
      ++Calls;
      return Value;
    });
  case SignatureKind::StringToString:
    return State.Bindings().Register(GlobalName, [&Calls](std::string Value) {
      ++Calls;
      return Value;
    });
  case SignatureKind::BooleanIntegerToNumber:
    return State.Bindings().Register(
        GlobalName, [&Calls](bool Flag, int Value) {
          ++Calls;
          return Flag ? static_cast<double>(Value) : 0.0;
        });
  case SignatureKind::NumberStringBooleanToString:
    return State.Bindings().Register(GlobalName,
                                     [&Calls](double, std::string Value, bool) {
                                       ++Calls;
                                       return Value;
                                     });
  case SignatureKind::AllToVoid:
    return State.Bindings().Register(
        GlobalName, [&Calls](bool, int, double, std::string) { ++Calls; });
  case SignatureKind::Count:
    break;
  }
  return State.Bindings().Register(GlobalName, [&Calls]() { ++Calls; });
}

[[nodiscard]] std::string InvocationSource(std::size_t ReceivedCount) {
  std::string Source(GlobalName);
  Source.push_back('(');
  for (std::size_t Index = 0; Index < ReceivedCount; ++Index) {
    if (Index != 0)
      Source.push_back(',');
    Source += "nil";
  }
  Source.push_back(')');
  return Source;
}

[[nodiscard]] bool Contains(std::string_view Text, std::string_view Context) {
  return Text.find(Context) != std::string_view::npos;
}

} // namespace

int RunWrongArgumentCountProperties() {
  // clang-format off
  const bool Passed = rc::check(
      // clang-format on
      "Wrong argument counts produce contextual failures",
      [](int GeneratedSignature, int GeneratedReceivedCount) {
        Luna::State State;
        RC_ASSERT(State.IsReady());

        const auto Signature = NormalizeSignature(GeneratedSignature);
        const std::size_t Expected = ExpectedCount(Signature);
        const std::size_t Received =
            NormalizeReceivedCount(GeneratedReceivedCount, Expected);
        RC_ASSERT(Received != Expected);

        int Calls = 0;
        const auto Registration = RegisterSignature(State, Signature, Calls);
        RC_ASSERT(Registration.IsSuccess());

        const auto Execution = State.Execute(InvocationSource(Received));
        const auto *Diagnostic = Execution.Diagnostic();
        RC_ASSERT(!Execution.IsSuccess());
        RC_ASSERT(Diagnostic != nullptr);
        RC_ASSERT(Diagnostic->Category() == Luna::ErrorCategory::Runtime);

        const std::string_view Message = Diagnostic->Message();
        RC_ASSERT(Contains(Message, GlobalName));
        RC_ASSERT(Contains(Message, "expected " + std::to_string(Expected) +
                                        " arguments"));
        RC_ASSERT(Contains(Message, "received " + std::to_string(Received)));
        RC_ASSERT(!Contains(Message, " argument 1 "));
        RC_ASSERT(Calls == 0);
      });

  return Passed ? 0 : 1;
}
