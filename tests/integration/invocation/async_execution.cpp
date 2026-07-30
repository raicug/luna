// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "asynchronous execution integration check failed: "
            << Description << '\n';
}

[[nodiscard]] int StackDepth(const Luna::State &Owner) {
  const std::optional<int> Depth = Hooks::ObserveRootStackDepth(Owner);
  return Depth ? *Depth : -1;
}

[[nodiscard]] std::string DiagnosticOf(const Luna::ExecutionResult &Result) {
  return Result.Diagnostic() == nullptr
             ? std::string()
             : std::string(Result.Diagnostic()->Message());
}

struct Host final {
  std::vector<std::thread> Workers;
  std::vector<Luna::AsyncCompletionSource<Luna::ReturnPack>> Packs;
  Luna::State *Owner = nullptr;
  bool RetiredDuringCall = false;
  std::size_t RetainersDuringCall = 0;

  ~Host() { Join(); }

  void Join() {
    for (std::thread &Worker : Workers) {
      if (Worker.joinable())
        Worker.join();
    }
    Workers.clear();
  }
};

Host *Live = nullptr;

[[nodiscard]] Luna::AsyncTask<Luna::ReturnPack> DescribeLater(std::string Name,
                                                              int Count) {
  Luna::AsyncCompletionSource<Luna::ReturnPack> Source;
  Luna::AsyncTask<Luna::ReturnPack> Pending = Source.Task();
  Live->Packs.push_back(Source);
  Live->Workers.emplace_back([Source, Name, Count]() mutable {
    Luna::ReturnPack Produced;
    Produced.AppendText(Name).AppendInteger(Count * 2);
    static_cast<void>(Source.Complete(Produced));
  });
  return Pending;
}

[[nodiscard]] Luna::AsyncTask<int> ScaleLater(int Value) {
  Luna::AsyncCompletionSource<int> Source;
  Luna::AsyncTask<int> Pending = Source.Task();
  Live->Workers.emplace_back([Source, Value]() mutable {
    static_cast<void>(Source.Complete(Value * 2));
  });
  return Pending;
}

[[nodiscard]] Luna::AsyncTask<int> FailLater(int Value) {
  Luna::AsyncCompletionSource<int> Source;
  Luna::AsyncTask<int> Pending = Source.Task();
  Live->Workers.emplace_back([Source]() mutable {
    static_cast<void>(Source.Fail("the generated archive is unavailable"));
  });
  static_cast<void>(Value);
  return Pending;
}

[[nodiscard]] Luna::AsyncTask<int> RetiringScaleLater(int Value) {
  Luna::AsyncCompletionSource<int> Source;
  Luna::AsyncTask<int> Pending = Source.Task();
  if (Live->Owner && !Live->RetiredDuringCall) {
    Live->RetiredDuringCall =
        Hooks::RetireDispatchSlot(*Live->Owner, "RetiringScaleLater");
    Live->RetainersDuringCall =
        Hooks::DispatchInvocationRetainerCount(*Live->Owner);
  }
  Live->Workers.emplace_back([Source, Value]() mutable {
    static_cast<void>(Source.Complete(Value * 2));
  });
  return Pending;
}

[[nodiscard]] int Direct(int Value) { return Value + 1; }

void CheckScriptsAwaitValuesThroughTheRealMachine() {
  Host Local;
  Live = &Local;

  Luna::State Owner;
  Check(Owner.IsReady(), "the host State is ready");
  const int EntryDepth = StackDepth(Owner);

  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Scope = Registry.RegisterNamespace("Catalog");
  Scope.RegisterFunction("DescribeLater", &DescribeLater);
  Scope.RegisterFunction("ScaleLater", &ScaleLater);
  Check(Scope.Commit().IsSuccess(),
        "a scoped asynchronous surface commits in one transaction");

  const auto Result = Owner.Execute(
      "local name, size = Catalog.DescribeLater('archive', 3)\n"
      "assert(name == 'archive' and size == 6)\n"
      "local total = 0\n"
      "for index = 1, 4 do\n"
      "  total = total + Catalog.ScaleLater(index)\n"
      "end\n"
      "assert(total == 20)\n"
      "local function nested(value)\n"
      "  return Catalog.ScaleLater(value) + Catalog.ScaleLater(value)\n"
      "end\n"
      "assert(nested(5) == 20)");
  Check(Result.IsSuccess(),
        "compiled Luau awaits every value through the real machine: " +
            DiagnosticOf(Result));

  const Luna::Detail::AsyncCallCounters Counters =
      Hooks::AsyncCallCountersOf(Owner);
  Check(Counters.Suspensions == 7 && Counters.Completions == 7 &&
            Counters.Failures == 0 && Counters.Cancellations == 0,
        "each awaited call suspended and resumed exactly once");
  Check(Hooks::PendingAsyncCallCount(Owner) == 0 &&
            StackDepth(Owner) == EntryDepth,
        "execution retains no suspended call and restores the stack");
  Check(Hooks::DispatchInvocationRetainerCount(Owner) == 0,
        "every retained dispatch generation is released after resumption");

  Local.Join();
  Live = nullptr;
}

void CheckSuspendedCallsResumeThroughTheirRetainedGeneration() {
  Host Local;
  Live = &Local;

  Luna::State Owner;
  Local.Owner = &Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.RegisterFunction("RetiringScaleLater", &RetiringScaleLater)
                .IsSuccess() &&
            Registry.RegisterFunction("Direct", &Direct).IsSuccess(),
        "the retiring asynchronous surface registers");

  const auto Resumed = Owner.Execute("assert(RetiringScaleLater(21) == 42)");
  Check(Local.RetiredDuringCall,
        "the call retired its own dispatch slot before suspending");
  Check(Local.RetainersDuringCall >= 1,
        "the suspending call retains its dispatch generation");
  Check(Resumed.IsSuccess(),
        "a suspended call resumes through the generation it retained: " +
            DiagnosticOf(Resumed));

  const auto Later = Owner.Execute("RetiringScaleLater(21)");
  Check(!Later.IsSuccess() && DiagnosticOf(Later).find("Unavailable binding") !=
                                  std::string::npos,
        "a later call through the retired slot fails deterministically");
  Check(Owner.Execute("assert(Direct(1) == 2)").IsSuccess(),
        "the State stays reusable after the retirement");

  Local.Join();
  Live = nullptr;
}

void CheckProtectedExecutionSurvivesAsynchronousFailure() {
  Host Local;
  Live = &Local;

  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.RegisterFunction("FailLater", &FailLater).IsSuccess() &&
            Registry.RegisterFunction("ScaleLater", &ScaleLater).IsSuccess() &&
            Registry.RegisterFunction("Direct", &Direct).IsSuccess(),
        "the failing asynchronous surface registers");

  const int EntryDepth = StackDepth(Owner);
  const auto Unprotected = Owner.Execute("FailLater(1)");
  Check(!Unprotected.IsSuccess() && Unprotected.Diagnostic() != nullptr &&
            Unprotected.Diagnostic()->Category() ==
                Luna::ErrorCategory::Runtime &&
            DiagnosticOf(Unprotected)
                    .find("the generated archive is "
                          "unavailable") != std::string::npos,
        "an asynchronous failure reaches the execution diagnostic");

  const auto Protected = Owner.Execute(
      "local ok, message = pcall(function() return FailLater(1) end)\n"
      "assert(not ok and type(message) == 'string')\n"
      "local awaited, value = pcall(function() return ScaleLater(3) end)\n"
      "assert(awaited and value == 6)\n"
      "assert(ScaleLater(4) == 8)");
  Check(Protected.IsSuccess(),
        "protected Luau code catches the failure and execution continues: " +
            DiagnosticOf(Protected));
  Check(StackDepth(Owner) == EntryDepth &&
            Hooks::PendingAsyncCallCount(Owner) == 0,
        "protected failure restores the stack and retains nothing");

  Local.Join();
  Live = nullptr;
}

void CheckOnlyTheExecutingChunkCanSuspend() {
  Host Local;
  Live = &Local;

  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.RegisterFunction("ScaleLater", &ScaleLater).IsSuccess() &&
            Registry.RegisterFunction("Direct", &Direct).IsSuccess(),
        "the suspension-boundary surface registers");

  const auto Foreign = Owner.Execute(
      "local worker = coroutine.create(function() return ScaleLater(1) end)\n"
      "local ok, message = coroutine.resume(worker)\n"
      "assert(not ok and string.find(message, 'Unsupported suspension') ~= "
      "nil)\n"
      "assert(ScaleLater(2) == 4)");
  Check(Foreign.IsSuccess(),
        "a script coroutine cannot suspend a Luna call, and the chunk can: " +
            DiagnosticOf(Foreign));
  Check(Hooks::AsyncCallCountersOf(Owner).Refusals == 1 &&
            Hooks::AsyncCallCountersOf(Owner).Suspensions == 1,
        "the refused suspension registered no suspended call");

  const auto Bare = Owner.Execute("coroutine.yield()");
  Check(!Bare.IsSuccess() &&
            DiagnosticOf(Bare).find(
                "yielded without any suspended Luna call") != std::string::npos,
        "a chunk that yields on its own fails deterministically");
  Check(Owner.Execute("assert(Direct(1) == 2)").IsSuccess(),
        "the State stays reusable after both refusals");

  Local.Join();
  Live = nullptr;
}

} // namespace

int RunAsynchronousExecutionIntegrationTests() {
  FailureCount = 0;
  CheckScriptsAwaitValuesThroughTheRealMachine();
  CheckSuspendedCallsResumeThroughTheirRetainedGeneration();
  CheckProtectedExecutionSurvivesAsynchronousFailure();
  CheckOnlyTheExecutingChunkCanSuspend();
  return FailureCount == 0 ? 0 : 1;
}
