// clang-format off
#include <luna/luna.hpp>

#include <rapidcheck.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
// clang-format on

namespace {

constexpr std::string_view GlobalName = "IncompatibleArguments";
constexpr std::size_t Arity = 4;

enum class SignatureKind : std::uint8_t { First, Second, Third, Fourth, Count };
enum class ExpectedKind : std::uint8_t { Boolean, Integer, Number, String };
enum class ReceivedKind : std::uint8_t { Boolean, Number, String, Nil, Count };

[[nodiscard]] SignatureKind NormalizeSignature(std::uint8_t Value) noexcept {
  return static_cast<SignatureKind>(
      Value % static_cast<std::uint8_t>(SignatureKind::Count));
}

[[nodiscard]] std::array<ExpectedKind, Arity>
ExpectedTypes(SignatureKind Signature) noexcept {
  switch (Signature) {
  case SignatureKind::First:
    return {ExpectedKind::Boolean, ExpectedKind::Integer, ExpectedKind::Number,
            ExpectedKind::String};
  case SignatureKind::Second:
    return {ExpectedKind::String, ExpectedKind::Number, ExpectedKind::Boolean,
            ExpectedKind::Integer};
  case SignatureKind::Third:
    return {ExpectedKind::Integer, ExpectedKind::String, ExpectedKind::Number,
            ExpectedKind::Boolean};
  case SignatureKind::Fourth:
    return {ExpectedKind::Number, ExpectedKind::Boolean, ExpectedKind::String,
            ExpectedKind::Integer};
  case SignatureKind::Count:
    break;
  }
  return {};
}

[[nodiscard]] ReceivedKind CompatibleType(ExpectedKind Expected) noexcept {
  switch (Expected) {
  case ExpectedKind::Boolean:
    return ReceivedKind::Boolean;
  case ExpectedKind::Integer:
  case ExpectedKind::Number:
    return ReceivedKind::Number;
  case ExpectedKind::String:
    return ReceivedKind::String;
  }
  return ReceivedKind::Nil;
}
[[nodiscard]] bool IsCompatible(ExpectedKind Expected,
                                ReceivedKind Received) noexcept {
  return CompatibleType(Expected) == Received;
}

[[nodiscard]] ReceivedKind SelectMismatch(ExpectedKind Expected,
                                          std::uint8_t Generated) noexcept {
  auto Received = static_cast<ReceivedKind>(
      Generated % static_cast<std::uint8_t>(ReceivedKind::Count));
  while (IsCompatible(Expected, Received)) {
    Received = static_cast<ReceivedKind>(
        (static_cast<std::uint8_t>(Received) + 1U) %
        static_cast<std::uint8_t>(ReceivedKind::Count));
  }
  return Received;
}

[[nodiscard]] std::uint8_t MultipleMismatchMask(std::uint8_t Generated) {
  std::uint8_t Mask = Generated & 0x0fU;
  for (std::size_t Index = 0; std::popcount(Mask) < 2; ++Index)
    Mask |= static_cast<std::uint8_t>(1U << (Index % Arity));
  return Mask;
}

[[nodiscard]] std::string_view ExpectedTypeName(ExpectedKind Kind) noexcept {
  switch (Kind) {
  case ExpectedKind::Boolean:
    return "boolean";
  case ExpectedKind::Integer:
    return "signed 32-bit integer";
  case ExpectedKind::Number:
    return "number";
  case ExpectedKind::String:
    return "string";
  }
  return "unknown";
}

[[nodiscard]] std::string_view ReceivedTypeName(ReceivedKind Kind) noexcept {
  switch (Kind) {
  case ReceivedKind::Boolean:
    return "boolean";
  case ReceivedKind::Number:
    return "number";
  case ReceivedKind::String:
    return "string";
  case ReceivedKind::Nil:
    return "nil";
  case ReceivedKind::Count:
    break;
  }
  return "unknown";
}

[[nodiscard]] std::string_view SourceLiteral(ReceivedKind Kind) noexcept {
  switch (Kind) {
  case ReceivedKind::Boolean:
    return "true";
  case ReceivedKind::Number:
    return "17";
  case ReceivedKind::String:
    return "\"text\"";
  case ReceivedKind::Nil:
    return "nil";
  case ReceivedKind::Count:
    break;
  }
  return "nil";
}

[[nodiscard]] Luna::RegistrationResult
RegisterSignature(Luna::State &State, SignatureKind Signature, int &Calls) {
  switch (Signature) {
  case SignatureKind::First:
    return State.Bindings().Register(
        GlobalName, [&Calls](bool, int, double, std::string) { ++Calls; });
  case SignatureKind::Second:
    return State.Bindings().Register(
        GlobalName, [&Calls](std::string, double, bool, int) { ++Calls; });
  case SignatureKind::Third:
    return State.Bindings().Register(
        GlobalName, [&Calls](int, std::string, double, bool) { ++Calls; });
  case SignatureKind::Fourth:
    return State.Bindings().Register(
        GlobalName, [&Calls](double, bool, std::string, int) { ++Calls; });
  case SignatureKind::Count:
    break;
  }
  return State.Bindings().Register(GlobalName, [&Calls]() { ++Calls; });
}
[[nodiscard]] std::string
InvocationSource(const std::array<ReceivedKind, Arity> &Received) {
  std::string Source(GlobalName);
  Source.push_back('(');
  for (std::size_t Index = 0; Index < Received.size(); ++Index) {
    if (Index != 0)
      Source.push_back(',');
    Source += SourceLiteral(Received[Index]);
  }
  Source.push_back(')');
  return Source;
}

[[nodiscard]] bool Contains(std::string_view Text, std::string_view Context) {
  return Text.find(Context) != std::string_view::npos;
}

} // namespace

int RunIncompatibleArgumentTypesProperties() {
  // clang-format off
  const bool Passed = rc::check(
      // clang-format on
      "Incompatible argument types identify the first mismatch",
      [](std::uint8_t GeneratedSignature, std::uint8_t GeneratedMismatchMask,
         std::uint32_t GeneratedReceivedTypes) {
        const SignatureKind Signature = NormalizeSignature(GeneratedSignature);
        const auto Expected = ExpectedTypes(Signature);
        const std::uint8_t MismatchMask =
            MultipleMismatchMask(GeneratedMismatchMask);
        RC_ASSERT(std::popcount(MismatchMask) >= 2);

        std::array<ReceivedKind, Arity> Received{};
        for (std::size_t Index = 0; Index < Arity; ++Index) {
          if ((MismatchMask & (1U << Index)) != 0) {
            const auto Generated = static_cast<std::uint8_t>(
                GeneratedReceivedTypes >> (Index * 8U));
            Received[Index] = SelectMismatch(Expected[Index], Generated);
          } else {
            Received[Index] = CompatibleType(Expected[Index]);
          }
        }

        const std::size_t FirstMismatch =
            static_cast<std::size_t>(std::countr_zero(MismatchMask));
        RC_ASSERT(FirstMismatch < Arity);

        Luna::State State;
        RC_ASSERT(State.IsReady());
        int Calls = 0;
        RC_ASSERT(RegisterSignature(State, Signature, Calls).IsSuccess());

        const auto Execution = State.Execute(InvocationSource(Received));
        const auto *Diagnostic = Execution.Diagnostic();
        RC_ASSERT(!Execution.IsSuccess());
        RC_ASSERT(Diagnostic != nullptr);
        RC_ASSERT(Diagnostic->Category() == Luna::ErrorCategory::Runtime);

        const std::string_view Message = Diagnostic->Message();
        const std::size_t Position = FirstMismatch + 1;
        const std::string FirstContext =
            "argument " + std::to_string(Position) + " expected " +
            std::string(ExpectedTypeName(Expected[FirstMismatch])) +
            " but received " +
            std::string(ReceivedTypeName(Received[FirstMismatch]));
        RC_ASSERT(Contains(Message, GlobalName));
        RC_ASSERT(Contains(Message, FirstContext));

        for (std::size_t Index = FirstMismatch + 1; Index < Arity; ++Index) {
          if ((MismatchMask & (1U << Index)) != 0) {
            RC_ASSERT(!Contains(Message,
                                "argument " + std::to_string(Index + 1) + " "));
          }
        }
        RC_ASSERT(Calls == 0);
      });

  return Passed ? 0 : 1;
}
