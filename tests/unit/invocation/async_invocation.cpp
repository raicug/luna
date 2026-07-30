// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <memory>
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
  std::cerr << "asynchronous invocation check failed: " << Description << '\n';
}

struct AsyncFixture final {
  Luna::AsyncCompletionSource<int> Scaled;
  Luna::AsyncCompletionSource<void> Recorded;
  Luna::AsyncCompletionSource<Luna::ReturnPack> Split;
  std::vector<std::thread> Workers;
  std::vector<int> Observed;

  ~AsyncFixture() { Join(); }

  void Join() {
    for (std::thread &Worker : Workers) {
      if (Worker.joinable())
        Worker.join();
    }
    Workers.clear();
  }
};

AsyncFixture *Fixture = nullptr;

[[nodiscard]] Luna::AsyncTask<int> ScaleLater(int Value) {
  Luna::AsyncTask<int> Pending = Fixture->Scaled.Task();
  Luna::AsyncCompletionSource<int> Source = Fixture->Scaled;
  Fixture->Workers.emplace_back([Source, Value]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    static_cast<void>(Source.Complete(Value * 2));
  });
  return Pending;
}

[[nodiscard]] Luna::AsyncTask<int> ScaleAlreadyDone(int Value) {
  Luna::AsyncCompletionSource<int> Source;
  Luna::AsyncTask<int> Pending = Source.Task();
  static_cast<void>(Source.Complete(Value + 1));
  return Pending;
}

[[nodiscard]] Luna::AsyncTask<void> RecordLater(int Value) {
  Luna::AsyncTask<void> Pending = Fixture->Recorded.Task();
  Luna::AsyncCompletionSource<void> Source = Fixture->Recorded;
  Fixture->Observed.push_back(Value);
  Fixture->Workers.emplace_back([Source]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    static_cast<void>(Source.Complete());
  });
  return Pending;
}

[[nodiscard]] Luna::AsyncTask<Luna::ReturnPack> SplitLater(std::string Text) {
  Luna::AsyncTask<Luna::ReturnPack> Pending = Fixture->Split.Task();
  Luna::AsyncCompletionSource<Luna::ReturnPack> Source = Fixture->Split;
  Fixture->Workers.emplace_back([Source, Text]() mutable {
    Luna::ReturnPack Produced;
    Produced.AppendText(Text).AppendInteger(static_cast<int>(Text.size()));
    static_cast<void>(Source.Complete(Produced));
  });
  return Pending;
}

[[nodiscard]] Luna::AsyncTask<int> FailLater(int Value) {
  Luna::AsyncTask<int> Pending = Fixture->Scaled.Task();
  Luna::AsyncCompletionSource<int> Source = Fixture->Scaled;
  Fixture->Workers.emplace_back([Source, Value]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (Value < 0)
      static_cast<void>(Source.Fail("the requested page is negative"));
    else
      static_cast<void>(Source.Cancel("the request was withdrawn"));
  });
  return Pending;
}

[[nodiscard]] Luna::AsyncTask<int> NeverStarted(int) {
  return Luna::AsyncTask<int>();
}

[[nodiscard]] std::future<int> DeferredScale(int Value) {
  return std::async(std::launch::deferred, [Value] { return Value * 3; });
}

[[nodiscard]] std::future<std::string> ThreadedText(std::string Text) {
  auto Promise = std::make_shared<std::promise<std::string>>();
  std::future<std::string> Pending = Promise->get_future();
  Fixture->Workers.emplace_back([Promise, Text]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Promise->set_value(Text + "!");
  });
  return Pending;
}

[[nodiscard]] std::future<int> FutureFailure(int) {
  auto Promise = std::make_shared<std::promise<int>>();
  std::future<int> Pending = Promise->get_future();
  Fixture->Workers.emplace_back([Promise]() mutable {
    try {
      throw std::runtime_error("the worker rejected the request");
    } catch (...) {
      Promise->set_exception(std::current_exception());
    }
  });
  return Pending;
}

[[nodiscard]] int Scale(int Value) { return Value * 2; }

void CheckAsyncMetadataIsReflectedCanonically() {
  AsyncFixture Local;
  Fixture = &Local;

  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.RegisterFunction("ScaleLater", &ScaleLater).IsSuccess() &&
            Registry.RegisterFunction("Scale", &Scale).IsSuccess() &&
            Registry.RegisterFunction("SplitLater", &SplitLater).IsSuccess() &&
            Registry.RegisterFunction("RecordLater", &RecordLater).IsSuccess(),
        "asynchronous callables register through the ordinary path");

  const Luna::ReflectionSnapshot Published = Registry.Reflection();
  const auto CandidateOf = [&Published](std::string_view QualifiedName) {
    const Luna::ReflectionRecordRange Candidates =
        Published.Symbols(Luna::SymbolKind::FunctionCandidate);
    for (std::size_t Index = 0; Index < Candidates.Size(); ++Index) {
      const Luna::ReflectionRecord Candidate = Candidates.At(Index);
      if (Candidate.QualifiedName() == QualifiedName)
        return Candidate;
    }
    return Luna::ReflectionRecord();
  };

  const Luna::ReflectionRecord Later = CandidateOf("ScaleLater");
  const Luna::ReflectionRecord Direct = CandidateOf("Scale");
  const Luna::ReflectionRecord Pack = CandidateOf("SplitLater");
  const Luna::ReflectionRecord Nothing = CandidateOf("RecordLater");
  Check(Published.Find("ScaleLater").IsValid() &&
            Published.Find("ScaleLater").Kind() ==
                Luna::SymbolKind::OverloadSet,
        "an asynchronous function publishes the same canonical symbols");

  Check(Later.IsValid() && Later.IsAsynchronous(),
        "one reflection record names the asynchronous delivery");
  Check(Direct.IsValid() && !Direct.IsAsynchronous(),
        "a direct callable stays synchronous in reflection");
  Check(Later.Returns() == Luna::ReturnShape::Scalar &&
            Later.Signature() == Direct.Signature(),
        "the awaited value is the reflected return type and signature");
  Check(Pack.IsValid() && Pack.IsAsynchronous() &&
            Pack.Returns() == Luna::ReturnShape::Multiple,
        "an awaited pack reflects multiple returns");
  Check(Nothing.IsValid() && Nothing.IsAsynchronous() &&
            Nothing.Returns() == Luna::ReturnShape::Zero,
        "an awaited void reflects zero returns");
  Check(Later.ParameterCount() == 1 && Later.Parameter(0).IsValid(),
        "the declared parameters are unchanged by asynchronous delivery");

  Fixture = nullptr;
}

void CheckSuspendedCallsResumeWithTheirValues() {
  AsyncFixture Local;
  Fixture = &Local;

  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(
      Registry.RegisterFunction("ScaleLater", &ScaleLater).IsSuccess() &&
          Registry.RegisterFunction("ScaleNow", &ScaleAlreadyDone)
              .IsSuccess() &&
          Registry.RegisterFunction("RecordLater", &RecordLater).IsSuccess() &&
          Registry.RegisterFunction("SplitLater", &SplitLater).IsSuccess(),
      "the asynchronous surface registers");

  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  const std::uint64_t EntryGeneration = Hooks::DispatchGenerationOf(Owner);

  const auto Scaled = Owner.Execute("assert(ScaleLater(21) == 42)");
  Check(Scaled.IsSuccess(), "a suspended call resumes with its awaited value");

  const auto Ready = Owner.Execute("assert(ScaleNow(41) == 42)");
  Check(Ready.IsSuccess(),
        "work that already finished still resumes exactly once");

  const auto Recorded = Owner.Execute("assert(RecordLater(7) == nil)");
  Check(Recorded.IsSuccess() && Local.Observed.size() == 1 &&
            Local.Observed[0] == 7,
        "an awaited void publishes no value and runs its work once");

  const auto Split = Owner.Execute("local text, size = SplitLater('luna')\n"
                                   "assert(text == 'luna' and size == 4)");
  Check(Split.IsSuccess(), "an awaited pack resumes with every value");

  const Luna::Detail::AsyncCallCounters Counters =
      Hooks::AsyncCallCountersOf(Owner);
  Check(Counters.Suspensions == 4 && Counters.Completions == 4 &&
            Counters.Failures == 0 && Counters.Cancellations == 0,
        "every suspension settles exactly once as completed");
  Check(Hooks::PendingAsyncCallCount(Owner) == 0,
        "no suspended call outlives the execution that started it");
  Check(Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "suspension and resumption restore the exact root stack depth");
  Check(Hooks::DispatchGenerationOf(Owner) == EntryGeneration,
        "suspension publishes no new dispatch generation");

  Local.Join();
  Fixture = nullptr;
}

void CheckSuspendedCallsSurviveNestingAndRepetition() {
  AsyncFixture Local;
  Fixture = &Local;

  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Scope = Registry.RegisterNamespace("Jobs");
  Scope.RegisterFunction("ScaleLater", &ScaleAlreadyDone);
  Check(Scope.Commit().IsSuccess(),
        "a scoped asynchronous callable commits in its outer transaction");

  const auto Nested = Owner.Execute(
      "local function twice(value)\n"
      "  return Jobs.ScaleLater(value) + Jobs.ScaleLater(value)\n"
      "end\n"
      "assert(twice(1) == 4)\n"
      "local total = 0\n"
      "for index = 1, 3 do total = total + Jobs.ScaleLater(index) end\n"
      "assert(total == 9)");
  Check(Nested.IsSuccess(), "suspension works inside Luau functions and loops");

  const Luna::Detail::AsyncCallCounters Counters =
      Hooks::AsyncCallCountersOf(Owner);
  Check(Counters.Suspensions == 5 && Counters.Completions == 5,
        "each nested call suspends and resumes exactly once");

  Local.Join();
  Fixture = nullptr;
}

void CheckStandardFuturesAreAwaited() {
  AsyncFixture Local;
  Fixture = &Local;

  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(
      Registry.RegisterFunction("DeferredScale", &DeferredScale).IsSuccess() &&
          Registry.RegisterFunction("ThreadedText", &ThreadedText)
              .IsSuccess() &&
          Registry.RegisterFunction("FutureFailure", &FutureFailure)
              .IsSuccess(),
      "a standard future is a supported asynchronous result");

  Check(Owner.Execute("assert(DeferredScale(4) == 12)").IsSuccess(),
        "deferred work runs on the owner thread and resumes the call");
  Check(Owner.Execute("assert(ThreadedText('luna') == 'luna!')").IsSuccess(),
        "work finished on another thread resumes the call");

  const auto Failed = Owner.Execute("FutureFailure(1)");
  Check(!Failed.IsSuccess() && Failed.Diagnostic() != nullptr &&
            Failed.Diagnostic()->Category() == Luna::ErrorCategory::Runtime,
        "a rejected future becomes a deterministic runtime failure");
  Check(Failed.Diagnostic() != nullptr &&
            Failed.Diagnostic()->Message().find(
                "the worker rejected the request") != std::string::npos,
        "the rejection reason reaches the execution diagnostic");

  Local.Join();
  Fixture = nullptr;
}

void CheckFailedAndCancelledWorkRecoversDeterministically() {
  AsyncFixture Local;
  Fixture = &Local;

  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.RegisterFunction("FailLater", &FailLater).IsSuccess() &&
            Registry.RegisterFunction("NeverStarted", &NeverStarted)
                .IsSuccess() &&
            Registry.RegisterFunction("Scale", &Scale).IsSuccess(),
        "the failing surface registers");

  const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  const auto Failed = Owner.Execute("FailLater(-1)");
  Check(!Failed.IsSuccess() && Failed.Diagnostic() != nullptr &&
            Failed.Diagnostic()->Category() == Luna::ErrorCategory::Runtime,
        "asynchronous failure is reported as a runtime failure");
  Check(Failed.Diagnostic() != nullptr &&
            Failed.Diagnostic()->Message().find(
                "the requested page is negative") != std::string::npos,
        "the failure reason is deterministic and complete");
  const auto Restoration = Hooks::ObserveLastCallbackStackRestoration(Owner);
  Check(Restoration.has_value() &&
            Restoration->EntryDepth == Restoration->RestoredDepth &&
            Restoration->ErrorDepth == Restoration->RestoredDepth + 1,
        "a failed resumption restores the exact callback stack depth");

  const auto Repeated = Owner.Execute("FailLater(-1)");
  Check(Repeated.Diagnostic() != nullptr && Failed.Diagnostic() != nullptr &&
            Repeated.Diagnostic()->Message() == Failed.Diagnostic()->Message(),
        "repeating the same asynchronous failure repeats its diagnostic");

  AsyncFixture Cancelling;
  Fixture = &Cancelling;
  const auto Cancelled = Owner.Execute("FailLater(1)");
  Check(!Cancelled.IsSuccess() && Cancelled.Diagnostic() != nullptr &&
            Cancelled.Diagnostic()->Message().find(
                "the request was withdrawn") != std::string::npos,
        "cancelled work reports why it was cancelled");
  Check(Cancelling.Scaled.Stage() == Luna::AsyncStage::Cancelled,
        "the host observes the cancelled stage on its own completion state");

  const auto Missing = Owner.Execute("NeverStarted(1)");
  Check(!Missing.IsSuccess() && Missing.Diagnostic() != nullptr,
        "a callable that started no work fails deterministically");

  Check(Owner.Execute("assert(Scale(21) == 42)").IsSuccess(),
        "the State stays reusable after every asynchronous failure");
  Check(Hooks::PendingAsyncCallCount(Owner) == 0 &&
            Hooks::ObserveRootStackDepth(Owner) == EntryDepth,
        "no failed suspension is retained and the stack is restored");

  const Luna::Detail::AsyncCallCounters Counters =
      Hooks::AsyncCallCountersOf(Owner);
  Check(Counters.Failures == 3 && Counters.Cancellations == 1,
        "each settled stage is counted exactly once");

  Cancelling.Join();
  Local.Join();
  Fixture = nullptr;
}

void CheckCallSitesThatCannotSuspendAreRefused() {
  AsyncFixture Local;
  Fixture = &Local;

  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.RegisterFunction("ScaleNow", &ScaleAlreadyDone).IsSuccess(),
        "the refusal surface registers");

  const auto Observed =
      Hooks::InvokeBinding(Owner, "ScaleNow", {Luna::Value(2)});
  Check(!Observed.Succeeded &&
            Observed.ErrorMessage.find("Unsupported suspension") !=
                std::string::npos,
        "a call site that cannot suspend is refused deterministically");
  Check(Observed.EntryStackDepth == Observed.FinalStackDepth,
        "a refused suspension restores the exact stack depth");
  Check(Hooks::PendingAsyncCallCount(Owner) == 0 &&
            Hooks::AsyncCallCountersOf(Owner).Refusals == 1 &&
            Hooks::AsyncCallCountersOf(Owner).Suspensions == 0,
        "a refused suspension registers no suspended call");
  Check(Owner.Execute("assert(ScaleNow(41) == 42)").IsSuccess(),
        "the same callable still suspends from an executing chunk");

  Local.Join();
  Fixture = nullptr;
}

void CheckCompletionStateOutlivesItsState() {
  AsyncFixture Local;
  Fixture = &Local;

  Luna::AsyncCompletionSource<int> Retained = Local.Scaled;
  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Check(Registry.RegisterFunction("ScaleLater", &ScaleLater).IsSuccess(),
          "the lifetime surface registers");
    Check(Owner.Execute("assert(ScaleLater(21) == 42)").IsSuccess(),
          "the suspended call completes before the State is destroyed");
    Local.Join();
  }

  Check(Retained.Stage() == Luna::AsyncStage::Ready,
        "the host keeps its own completion state after the State is gone");
  Check(!Retained.Complete(1),
        "settling completed work again is refused instead of undefined");

  Luna::AsyncCompletionSource<int> Abandoned;
  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Check(Registry.RegisterFunction("ScaleNow", &ScaleAlreadyDone).IsSuccess(),
          "the abandoned surface registers");
    static_cast<void>(Owner.Execute("assert(ScaleNow(1) == 2)"));
  }
  Check(Abandoned.Stage() == Luna::AsyncStage::Pending && Abandoned.Complete(5),
        "completion state Luna never saw is untouched by State destruction");

  Fixture = nullptr;
}

struct Cursor final {
  int Position = 0;

  [[nodiscard]] Luna::AsyncTask<int> Advance() {
    Luna::AsyncCompletionSource<int> Source;
    Luna::AsyncTask<int> Pending = Source.Task();
    static_cast<void>(Source.Complete(Position + 1));
    return Pending;
  }
};

void CheckAsyncClassMembersAreRefusedAtRegistration() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  Luna::ClassBuilder<Cursor> Class = Registry.RegisterClass<Cursor>(
      "Cursor", Luna::StableTypeKey("Studio.Cursor"));
  Class.Constructor<>().Method("Advance", &Cursor::Advance);
  const auto Result = Class.Commit();

  Check(!Result.IsSuccess() && Result.Diagnostic() != nullptr,
        "an asynchronous class member is refused at registration time");
  Check(Result.Diagnostic() != nullptr &&
            Result.Diagnostic()->Message().find("asynchronous delivery") !=
                std::string::npos,
        "the refusal names why the member cannot deliver its value later");
  Check(Registry.Reflection().IsEmpty() &&
            Hooks::RegisteredClassCount(Owner) == 0,
        "a refused asynchronous member publishes no partial class");
  Check(Registry.RegisterFunction("Scale", &Scale).IsSuccess() &&
            Owner.Execute("assert(Scale(2) == 4)").IsSuccess(),
        "the State stays reusable after the refusal");
}

} // namespace

int RunAsynchronousInvocationTests() {
  FailureCount = 0;
  CheckAsyncMetadataIsReflectedCanonically();
  CheckSuspendedCallsResumeWithTheirValues();
  CheckSuspendedCallsSurviveNestingAndRepetition();
  CheckStandardFuturesAreAwaited();
  CheckFailedAndCancelledWorkRecoversDeterministically();
  CheckCallSitesThatCannotSuspendAreRefused();
  CheckCompletionStateOutlivesItsState();
  CheckAsyncClassMembersAreRefusedAtRegistration();
  return FailureCount == 0 ? 0 : 1;
}
