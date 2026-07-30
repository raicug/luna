// clang-format off
#include <luna/luna.hpp>

#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <rapidcheck.h>

#include <cstdint>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using FaultPoint = Luna::Detail::StateFaultPoint;

enum class RequestKind : std::uint32_t {
  Success,
  InvalidNameFailure,
  DuplicateFailure,
  InstallationFailure,
  Count
};

[[nodiscard]] RequestKind NormalizeRequest(int Value) noexcept {
  return static_cast<RequestKind>(
      static_cast<std::uint32_t>(Value) %
      static_cast<std::uint32_t>(RequestKind::Count));
}

[[nodiscard]] int NormalizeDepth(int Value) noexcept {
  constexpr std::uint32_t ValidDepthCount = 65;
  return static_cast<int>(static_cast<std::uint32_t>(Value) % ValidDepthCount);
}

} // namespace

int RunRegistrationStackBalanceProperties() {

  const bool Passed = rc::check(

      "Registration preserves stack depth",
      [](int GeneratedDepth, int GeneratedRequest) {
        Luna::State State;
        RC_ASSERT(State.IsReady());

        const auto Kind = NormalizeRequest(GeneratedRequest);

        if (Kind == RequestKind::DuplicateFailure) {
          const auto Setup =
              State.Bindings().Register("Duplicate", []() { return 1; });
          RC_ASSERT(Setup.IsSuccess());
        }

        const int SeedDepth = NormalizeDepth(GeneratedDepth);
        RC_ASSERT(Hooks::SetRootStackDepth(State, SeedDepth));
        const auto EntryDepth = Hooks::ObserveRootStackDepth(State);
        RC_ASSERT(EntryDepth.has_value());
        RC_ASSERT(*EntryDepth == SeedDepth);

        const auto Result = [&]() -> Luna::RegistrationResult {
          switch (Kind) {
          case RequestKind::Success:
            return State.Bindings().Register("Successful", []() { return 2; });
          case RequestKind::InvalidNameFailure:
            return State.Bindings().Register("invalid-name", []() {});
          case RequestKind::DuplicateFailure:
            return State.Bindings().Register("Duplicate", []() { return 3; });
          case RequestKind::InstallationFailure:
            Hooks::InjectFault(State, FaultPoint::BindingInstallation);
            return State.Bindings().Register("InstallationFailure", []() {});
          case RequestKind::Count:
            break;
          }
          return State.Bindings().Register("invalid-name", []() {});
        }();

        RC_ASSERT(Result.IsSuccess() == (Kind == RequestKind::Success));
        RC_ASSERT(Hooks::ObserveRootStackDepth(State) == EntryDepth);
      });

  return Passed ? 0 : 1;
}
