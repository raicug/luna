// clang-format off
#include <luna/luna.hpp>
#include <rapidcheck.h>
#include <cstddef>
#include <string>
#include <vector>
// clang-format on

int RunStateRegistrationIsolationProperties() {
  // clang-format off
  const bool Passed = rc::check(
      // clang-format on
      "Registrations are isolated by State",
      [](const std::vector<int> &Values) {
        const auto Pick = [&](std::size_t Index) {
          return Values.empty()
                     ? Index
                     : static_cast<std::size_t>(Values[Index % Values.size()]);
        };
        const std::size_t OwnerCount = 1U + Pick(0) % 5U;
        const std::size_t OtherCount = 1U + Pick(1) % 5U;
        std::vector<int> OwnerCalls(OwnerCount);
        std::vector<int> IndependentCalls(OwnerCount);
        std::vector<int> OtherCalls(OtherCount);
        Luna::State Owner;
        Luna::State Other;
        RC_ASSERT(Owner.IsReady());
        RC_ASSERT(Other.IsReady());

        for (std::size_t Index = 0; Index < OwnerCount; ++Index) {
          const std::string Name = "Owned_" + std::to_string(Index);
          RC_ASSERT(Owner.Bindings()
                        .Register(Name, [&OwnerCalls,
                                         Index]() { ++OwnerCalls[Index]; })
                        .IsSuccess());
          if (Pick(Index + 2U) % 2U == 0U)
            RC_ASSERT(
                Other.Bindings()
                    .Register(Name, [&IndependentCalls,
                                     Index]() { ++IndependentCalls[Index]; })
                    .IsSuccess());
        }
        for (std::size_t Index = 0; Index < OtherCount; ++Index) {
          const std::string Name = "Other_" + std::to_string(Index);
          RC_ASSERT(Other.Bindings()
                        .Register(Name, [&OtherCalls,
                                         Index]() { ++OtherCalls[Index]; })
                        .IsSuccess());
        }

        for (std::size_t Index = 0; Index < OwnerCount; ++Index) {
          const std::string Name = "Owned_" + std::to_string(Index);
          RC_ASSERT(Owner.Execute(Name + "()").IsSuccess());
          RC_ASSERT(OwnerCalls[Index] == 1);
          const bool Independent = Pick(Index + 2U) % 2U == 0U;
          RC_ASSERT(Other.Execute(Name + "()").IsSuccess() == Independent);
          RC_ASSERT(IndependentCalls[Index] == (Independent ? 1 : 0));
        }
        for (std::size_t Index = 0; Index < OtherCount; ++Index) {
          const std::string Name = "Other_" + std::to_string(Index);
          RC_ASSERT(Other.Execute(Name + "()").IsSuccess());
          RC_ASSERT(OtherCalls[Index] == 1);
          RC_ASSERT(!Owner.Execute(Name + "()").IsSuccess());
        }
      });

  return Passed ? 0 : 1;
}
