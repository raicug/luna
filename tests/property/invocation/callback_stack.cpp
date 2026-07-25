// clang-format off
#include <luna/luna.hpp>

#include "state/testing/fault_point.hpp"
#include "state/testing/test_hooks.hpp"

#include <rapidcheck.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
// clang-format on

namespace {

using FaultPoint = Luna::Detail::StateFaultPoint;
using Hooks = Luna::Detail::StateTestHooks;
using Invocation = Luna::Detail::NativeInvocationObservation;

constexpr int MaximumCallbackDepth = 8;

template <std::size_t... Indices>
[[nodiscard]] Luna::RegistrationResult
RegisterAtDepth(Luna::State &State, std::string_view Name, int &Calls,
                bool Throws, std::index_sequence<Indices...>) {
  return State.Bindings().Register(
      Name,
      [&Calls,
       Throws](std::conditional_t<
               true, int,
               std::integral_constant<std::size_t, Indices>>... Values) -> int {
        ++Calls;
        if (Throws)
          throw std::runtime_error("generated native failure");
        return (0 + ... + Values);
      });
}

template <std::size_t Depth = 0>
[[nodiscard]] Luna::RegistrationResult
RegisterAtGeneratedDepth(Luna::State &State, std::string_view Name,
                         int RequestedDepth, int &Calls, bool Throws = false) {
  if (RequestedDepth == static_cast<int>(Depth))
    return RegisterAtDepth(State, Name, Calls, Throws,
                           std::make_index_sequence<Depth>{});
  if constexpr (Depth < static_cast<std::size_t>(MaximumCallbackDepth))
    return RegisterAtGeneratedDepth<Depth + 1>(State, Name, RequestedDepth,
                                               Calls, Throws);
  return RegisterAtDepth(State, Name, Calls, Throws, std::index_sequence<>{});
}

[[nodiscard]] int NormalizeDepth(int Value) noexcept {
  return static_cast<int>(static_cast<std::uint32_t>(Value) %
                          (MaximumCallbackDepth + 1U));
}

[[nodiscard]] bool RestoredBeforeError(const Luna::State &State,
                                       const Invocation &Result,
                                       int CallbackDepth) {
  const auto Stack = Hooks::ObserveLastCallbackStackRestoration(State);
  return !Result.Succeeded && Result.ReturnCount == 0 &&
         !Result.ErrorMessage.empty() && Stack.has_value() &&
         Stack->EntryDepth == CallbackDepth &&
         Stack->RestoredDepth == Stack->EntryDepth &&
         Stack->ErrorDepth == Stack->EntryDepth + 1 &&
         Result.CompletionStackDepth == Result.EntryStackDepth + 1 &&
         Result.FinalStackDepth == Result.EntryStackDepth;
}

} // namespace

int RunNativeFailureCallbackStackProperties() {
  // **Validates: Requirements 7.6, 7.7, 7.8**
  // clang-format off
  // Feature: luau-binding-foundation, Property 16: Native failures restore the callback checkpoint
  const bool Passed = rc::check(
      // clang-format on
      "Native failures restore the callback checkpoint",
      [](int GeneratedDepth, bool InjectInspectionFault) {
        const int CallbackDepth = NormalizeDepth(GeneratedDepth);
        const std::vector<Luna::Value> Arguments(
            static_cast<std::size_t>(CallbackDepth), Luna::Value{1});

        Luna::State State;
        RC_ASSERT(State.IsReady());

        int CallerCalls = 0;
        const int CallerArity = CallbackDepth == 0 ? 1 : 0;
        RC_ASSERT(RegisterAtGeneratedDepth(State, "CallerFailure", CallerArity,
                                           CallerCalls)
                      .IsSuccess());
        const auto Caller =
            Hooks::InvokeBinding(State, "CallerFailure", Arguments);
        RC_ASSERT(RestoredBeforeError(State, Caller, CallbackDepth));
        RC_ASSERT(CallerCalls == 0);

        int InternalCalls = 0;
        RC_ASSERT(RegisterAtGeneratedDepth(State, "InternalFailure",
                                           CallbackDepth, InternalCalls)
                      .IsSuccess());
        const auto InternalFault = InjectInspectionFault && CallbackDepth > 0
                                       ? FaultPoint::ArgumentInspection
                                       : FaultPoint::MissingMetadata;
        Hooks::InjectFault(State, InternalFault);
        const auto Internal =
            Hooks::InvokeBinding(State, "InternalFailure", Arguments);
        RC_ASSERT(RestoredBeforeError(State, Internal, CallbackDepth));
        RC_ASSERT(InternalCalls == 0);
        RC_ASSERT(Hooks::PendingFaults(State, InternalFault) == 0);

        int StandardCalls = 0;
        RC_ASSERT(RegisterAtGeneratedDepth(State, "StandardFailure",
                                           CallbackDepth, StandardCalls, true)
                      .IsSuccess());
        const auto Standard =
            Hooks::InvokeBinding(State, "StandardFailure", Arguments);
        RC_ASSERT(RestoredBeforeError(State, Standard, CallbackDepth));
        RC_ASSERT(StandardCalls == 1);
      });

  return Passed ? 0 : 1;
}
