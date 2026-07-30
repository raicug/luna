// clang-format off
#include <luna/luna.hpp>

#include <rapidcheck.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace {

constexpr std::string_view InitialCharacters =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_";
constexpr std::string_view LaterCharacters =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_0123456789";

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

[[nodiscard]] std::string ValidName(const std::vector<int> &Values,
                                    std::size_t Length, std::size_t Salt) {
  std::string Name(Length, '_');
  Name.front() = GeneratedCharacter(InitialCharacters, Values, Salt);
  for (std::size_t Index = 1; Index < Length; ++Index)
    Name[Index] = GeneratedCharacter(LaterCharacters, Values, Salt + Index);
  return Name;
}

} // namespace

int RunInvocableValidRegistrationProperties() {

  const bool Passed = rc::check(

      "Valid registration installs an invocable global",
      [](const std::vector<int> &GeneratedValues) {
        const std::size_t GeneratedLength =
            1U + (GeneratedByte(GeneratedValues, 0) % 255U);
        const std::array<std::size_t, 3> Lengths = {1U, 255U, GeneratedLength};

        for (std::size_t Case = 0; Case < Lengths.size(); ++Case) {
          const std::string Name =
              ValidName(GeneratedValues, Lengths[Case], Case * 257U + 1U);
          const std::string PreservedName = Name == "PreservedGlobal"
                                                ? "OtherPreservedGlobal"
                                                : "PreservedGlobal";

          Luna::State State;
          RC_ASSERT(State.IsReady());

          int PreservedInvocations = 0;
          const auto PreservedRegistration = State.Bindings().Register(
              PreservedName,
              [&PreservedInvocations]() { ++PreservedInvocations; });
          RC_ASSERT(PreservedRegistration.IsSuccess());

          int InstalledInvocations = 0;
          const auto Registration = State.Bindings().Register(
              Name, [&InstalledInvocations]() { ++InstalledInvocations; });
          RC_ASSERT(Registration.IsSuccess());

          const std::string Source = Name + "()\n" + PreservedName + "()";
          const auto Execution = State.Execute(Source);
          RC_ASSERT(Execution.IsSuccess());
          RC_ASSERT(InstalledInvocations == 1);
          RC_ASSERT(PreservedInvocations == 1);
        }
      });

  return Passed ? 0 : 1;
}
