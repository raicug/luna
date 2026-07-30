// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <rapidcheck.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

enum class Settlement : std::uint32_t {
  CompletedBeforeReturning,
  CompletedFromWorker,
  Failed,
  Cancelled,
  NeverStarted,
  Count
};

[[nodiscard]] Settlement NormalizeSettlement(int Value) noexcept {
  return static_cast<Settlement>(static_cast<std::uint32_t>(Value) %
                                 static_cast<std::uint32_t>(Settlement::Count));
}

[[nodiscard]] int NormalizeArgument(int Value) noexcept {
  return static_cast<int>(static_cast<std::uint32_t>(Value) % 1000U);
}

struct ExpectedCall final {
  Settlement Chosen = Settlement::CompletedBeforeReturning;
  int Argument = 0;

  [[nodiscard]] bool Completes() const noexcept {
    return Chosen == Settlement::CompletedBeforeReturning ||
           Chosen == Settlement::CompletedFromWorker;
  }

  [[nodiscard]] int Published() const noexcept { return Argument * 2; }
};

struct Plan final {
  std::vector<ExpectedCall> Calls;
};

struct Harness final {
  Plan Planned;
  std::size_t Consumed = 0;
  std::vector<std::thread> Workers;
  std::vector<Luna::AsyncCompletionSource<int>> Sources;

  ~Harness() { Join(); }

  void Join() {
    for (std::thread &Worker : Workers) {
      if (Worker.joinable())
        Worker.join();
    }
    Workers.clear();
  }
};

Harness *Active = nullptr;

[[nodiscard]] Luna::AsyncTask<int> Suspending(int Value) {
  if (!Active || Active->Consumed >= Active->Planned.Calls.size())
    return Luna::AsyncTask<int>();

  const ExpectedCall Expected = Active->Planned.Calls[Active->Consumed];
  Active->Consumed += 1;

  if (Expected.Chosen == Settlement::NeverStarted)
    return Luna::AsyncTask<int>();

  Luna::AsyncCompletionSource<int> Source;
  Luna::AsyncTask<int> Pending = Source.Task();
  Active->Sources.push_back(Source);

  switch (Expected.Chosen) {
  case Settlement::CompletedBeforeReturning:
    static_cast<void>(Source.Complete(Value * 2));
    break;
  case Settlement::CompletedFromWorker:
    Active->Workers.emplace_back([Source, Value]() mutable {
      static_cast<void>(Source.Complete(Value * 2));
    });
    break;
  case Settlement::Failed:
    Active->Workers.emplace_back([Source]() mutable {
      static_cast<void>(Source.Fail("the generated work failed"));
    });
    break;
  case Settlement::Cancelled:
    Active->Workers.emplace_back([Source]() mutable {
      static_cast<void>(Source.Cancel("the generated work was cancelled"));
    });
    break;
  case Settlement::NeverStarted:
  case Settlement::Count:
    break;
  }
  return Pending;
}

[[nodiscard]] int Direct(int Value) { return Value + 1; }

} // namespace

int RunAsynchronousSettlementProperties() {

  const bool Passed = rc::check(

      "Suspended calls settle exactly once and resume through their retained "
      "generation",
      [](const std::vector<int> &GeneratedSettlements,
         const std::vector<int> &GeneratedArguments) {
        Harness Local;
        const std::size_t Count =
            GeneratedSettlements.size() < 4 ? GeneratedSettlements.size() : 4;
        for (std::size_t Index = 0; Index < Count; ++Index) {
          ExpectedCall Expected;
          Expected.Chosen = NormalizeSettlement(GeneratedSettlements[Index]);
          Expected.Argument = Index < GeneratedArguments.size()
                                  ? NormalizeArgument(GeneratedArguments[Index])
                                  : static_cast<int>(Index);
          Local.Planned.Calls.push_back(Expected);
        }
        Active = &Local;

        Luna::State Owner;
        Luna::BindingRegistry Registry = Owner.Bindings();
        RC_ASSERT(
            Registry.RegisterFunction("Suspending", &Suspending).IsSuccess());
        RC_ASSERT(Registry.RegisterFunction("Direct", &Direct).IsSuccess());

        const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
        const std::uint64_t EntryGeneration =
            Hooks::DispatchGenerationOf(Owner);
        RC_ASSERT(EntryDepth.has_value());

        std::size_t ExpectedSuspensions = 0;
        std::size_t ExpectedCompletions = 0;
        std::size_t ExpectedFailures = 0;
        std::size_t ExpectedCancellations = 0;

        for (const ExpectedCall &Expected : Local.Planned.Calls) {
          const std::string Source =
              "assert(Suspending(" + std::to_string(Expected.Argument) +
              ") == " + std::to_string(Expected.Published()) + ")";
          const auto Result = Owner.Execute(Source);

          ExpectedSuspensions += 1;
          if (Expected.Completes()) {
            ExpectedCompletions += 1;
            RC_ASSERT(Result.IsSuccess());
            RC_ASSERT(Result.Diagnostic() == nullptr);
          } else {
            RC_ASSERT(!Result.IsSuccess());
            RC_ASSERT(Result.Diagnostic() != nullptr);
            RC_ASSERT(Result.Diagnostic()->Category() ==
                      Luna::ErrorCategory::Runtime);
            if (Expected.Chosen == Settlement::Cancelled) {
              ExpectedCancellations += 1;
              RC_ASSERT(Result.Diagnostic()->Message().find(
                            "the generated work was cancelled") !=
                        std::string::npos);
            } else {
              ExpectedFailures += 1;
            }
          }

          RC_ASSERT(Hooks::PendingAsyncCallCount(Owner) == 0);
          RC_ASSERT(Hooks::ObserveRootStackDepth(Owner) == EntryDepth);
          RC_ASSERT(Hooks::DispatchGenerationOf(Owner) == EntryGeneration);
        }

        const Luna::Detail::AsyncCallCounters Counters =
            Hooks::AsyncCallCountersOf(Owner);
        RC_ASSERT(Counters.Suspensions == ExpectedSuspensions);
        RC_ASSERT(Counters.Completions == ExpectedCompletions);
        RC_ASSERT(Counters.Failures == ExpectedFailures);
        RC_ASSERT(Counters.Cancellations == ExpectedCancellations);
        RC_ASSERT(Counters.Refusals == 0);

        for (Luna::AsyncCompletionSource<int> &Settled : Local.Sources) {
          RC_ASSERT(Settled.Stage() != Luna::AsyncStage::Pending);
          RC_ASSERT(!Settled.Complete(0));
        }

        RC_ASSERT(Owner.Execute("assert(Direct(1) == 2)").IsSuccess());

        Local.Join();
        Active = nullptr;
        return true;
      });

  return Passed ? 0 : 1;
}
