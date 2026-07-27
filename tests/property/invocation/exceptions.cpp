// clang-format off
#include <luna/luna.hpp>

#include <rapidcheck.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace {

constexpr std::string_view InitialCharacters =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_";
constexpr std::string_view LaterCharacters =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_0123456789";
constexpr std::string_view MessageCharacters =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 _-.:/";

[[nodiscard]] unsigned char GeneratedByte(const std::vector<int> &Values,
                                          std::size_t Index) noexcept {
  if (Values.empty())
    return static_cast<unsigned char>(Index * 37U + 11U);
  return static_cast<unsigned char>(
      static_cast<std::uint32_t>(Values[Index % Values.size()]) & 0xFFU);
}

[[nodiscard]] char GeneratedCharacter(std::string_view Alphabet,
                                      const std::vector<int> &Values,
                                      std::size_t Index) noexcept {
  return Alphabet[GeneratedByte(Values, Index) % Alphabet.size()];
}

[[nodiscard]] std::string ValidGlobalName(const std::vector<int> &Values) {
  const std::size_t Length = 1U + (GeneratedByte(Values, 0) % 255U);
  std::string Name(Length, '_');
  Name.front() = GeneratedCharacter(InitialCharacters, Values, 1);
  for (std::size_t Index = 1; Index < Length; ++Index)
    Name[Index] = GeneratedCharacter(LaterCharacters, Values, Index + 1U);
  return Name;
}

[[nodiscard]] std::string ExceptionMessage(const std::vector<int> &Values) {
  const std::size_t Length = 1U + (GeneratedByte(Values, 0) % 128U);
  std::string Message(Length, 'x');
  for (std::size_t Index = 0; Index < Length; ++Index)
    Message[Index] = GeneratedCharacter(MessageCharacters, Values, Index + 1U);
  return Message;
}

[[nodiscard]] bool Contains(std::string_view Text, std::string_view Context) {
  return Text.find(Context) != std::string_view::npos;
}

} // namespace

int RunStandardExceptionTranslationProperties() {
  // clang-format off
  const bool Passed = rc::check(
      // clang-format on
      "Standard exceptions are translated with context",
      [](const std::vector<int> &GeneratedName,
         const std::vector<int> &GeneratedMessage) {
        const std::string GlobalName = ValidGlobalName(GeneratedName);
        const std::string Message = ExceptionMessage(GeneratedMessage);

        Luna::State State;
        RC_ASSERT(State.IsReady());

        int Calls = 0;
        const auto Registration = State.Bindings().Register(
            GlobalName,
            [&Calls, Message](bool, int, double, std::string) -> int {
              ++Calls;
              throw std::runtime_error(Message);
            });
        RC_ASSERT(Registration.IsSuccess());

        const auto Execution =
            State.Execute(GlobalName + "(true, -17, 2.5, 'valid')");
        const auto *Diagnostic = Execution.Diagnostic();
        RC_ASSERT(!Execution.IsSuccess());
        RC_ASSERT(Diagnostic != nullptr);
        RC_ASSERT(Diagnostic->Category() == Luna::ErrorCategory::Runtime);
        RC_ASSERT(Contains(Diagnostic->Message(), GlobalName));
        RC_ASSERT(Contains(Diagnostic->Message(), Message));
        RC_ASSERT(Calls == 1);
      });

  return Passed ? 0 : 1;
}
