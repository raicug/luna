// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "interruption check failed: " << Description << '\n';
}

[[nodiscard]] bool Succeeds(Luna::State &Owner, std::string_view Source) {
  const Luna::ExecutionResult Result = Owner.Execute(Source);
  if (Result.IsSuccess())
    return true;
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    std::cerr << "interruption source failed: " << Diagnostic->Message()
              << '\n';
  return false;
}

void CheckPendingInterruptStopsAnEndlessChunk() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(!Owner.IsInterruptPending(), "no interrupt is pending at rest");
  Owner.RequestInterrupt("the user pressed stop");
  Check(Owner.IsInterruptPending(), "a request arms the interrupt");

  const Luna::ExecutionResult Stopped = Owner.Execute("while true do end");
  Check(!Stopped.IsSuccess(), "an interrupted chunk does not succeed");
  Check(Stopped.IsInterrupted(),
        "an interrupted chunk reports an interrupted result rather than a "
        "script error");
  Check(Stopped.Diagnostic() != nullptr && Stopped.Diagnostic()->Category() ==
                                               Luna::ErrorCategory::Interrupted,
        "the diagnostic carries the interrupted category");
  Check(Stopped.Diagnostic() != nullptr &&
            Stopped.Diagnostic()->Message().find("the user pressed stop") !=
                std::string::npos,
        "the diagnostic carries the reason the host stated");

  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "an interrupted chunk restores the entry stack depth");

  Owner.ClearInterrupt();
  Check(!Owner.IsInterruptPending(), "clearing disarms the interrupt");

  Check(Succeeds(Owner, "local Total = 0\n"
                        "for Index = 1, 1000 do Total = Total + Index end\n"
                        "assert(Total == 500500)"),
        "the State stays reusable after an interrupt");
}

void CheckPcallObservesTheInterruptAsAnOrdinaryError() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  std::thread Requester([&Owner]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Owner.RequestInterrupt("stop requested");
  });

  static_cast<void>(Owner.Execute("local Ok, Message = pcall(function()\n"
                                  "  while true do end\n"
                                  "end)\n"
                                  "Handled = Ok\n"
                                  "Reported = Message"));
  Requester.join();

  Owner.ClearInterrupt();
  Check(Succeeds(Owner,
                 "assert(Handled == false, 'pcall caught the interrupt')\n"
                 "assert(type(Reported) == 'string', 'a message was raised')\n"
                 "assert(string.find(Reported, 'interrupted') ~= nil)\n"
                 "assert(string.find(Reported, 'stop requested') ~= nil)"),
        "pcall observes a pending interrupt as an ordinary catchable error "
        "carrying the reason");
}

void CheckArmedInterruptPersistsUntilCleared() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");
  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Check(Succeeds(Owner, "Marker = 7"), "a chunk runs before any request");

  Owner.RequestInterrupt("late stop");
  Check(Owner.IsInterruptPending(),
        "requesting an interrupt with nothing executing arms the flag and "
        "raises nothing");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "a request with nothing executing leaves the stack alone");

  const Luna::ExecutionResult Armed =
      Owner.Execute("for Index = 1, 100 do Marker = Index end");
  Check(Armed.IsInterrupted(),
        "an armed interrupt stays armed, so the chunk that starts next is "
        "interrupted too");

  Owner.ClearInterrupt();
  Check(Succeeds(Owner, "assert(Marker == 7 or Marker >= 1)"),
        "the State's own model survives an interrupt request");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "every interrupted chunk restores the entry stack depth");
}

void CheckInterruptFromAnotherThreadStopsTheOwnerThread() {
  Luna::State Owner;
  Check(Owner.IsReady(), "the state is ready");

  std::thread Requester([&Owner]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Owner.RequestInterrupt("stopped from another thread");
  });

  const Luna::ExecutionResult Stopped =
      Owner.Execute("local Spin = 0\nwhile true do Spin = Spin + 1 end");
  Requester.join();

  Check(Stopped.IsInterrupted(),
        "a request raised on another thread stops the chunk running on the "
        "owner thread");
  Check(Stopped.Diagnostic() != nullptr &&
            Stopped.Diagnostic()->Message().find("another thread") !=
                std::string::npos,
        "the reason crosses the thread boundary intact");

  Owner.ClearInterrupt();
  Check(Succeeds(Owner, "assert(true)"), "the State stays reusable");
}

} // namespace

int RunExecutionInterruptionTests();

int RunExecutionInterruptionTests() {
  FailureCount = 0;
  CheckPendingInterruptStopsAnEndlessChunk();
  CheckPcallObservesTheInterruptAsAnOrdinaryError();
  CheckArmedInterruptPersistsUntilCleared();
  CheckInterruptFromAnotherThreadStopsTheOwnerThread();
  return FailureCount == 0 ? 0 : 1;
}
