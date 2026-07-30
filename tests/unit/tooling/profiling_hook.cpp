// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <chrono>
#include <iostream>
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
  std::cerr << "profiling hook check failed: " << Description << '\n';
}

struct RecordedEvents final {
  std::vector<Luna::ProfilingEvent> Events;

  void Record(const Luna::ProfilingEvent &Event) { Events.push_back(Event); }
};

[[nodiscard]] int Scale(int Value) { return Value * 2; }

[[nodiscard]] int Fail(int) {
  throw std::runtime_error("the callable refused");
}

struct Cursor final {
  int Position = 0;

  [[nodiscard]] int Advance() { return ++Position; }
};

[[nodiscard]] Luna::AsyncTask<int> ScaleLater(int Value) {
  Luna::AsyncCompletionSource<int> Source;
  Luna::AsyncTask<int> Pending = Source.Task();
  std::thread([Source, Value]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    static_cast<void>(Source.Complete(Value * 2));
  }).detach();
  return Pending;
}

void CheckSuccessfulCallsReportTheCanonicalIdentity() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.RegisterFunction("Scale", &Scale).IsSuccess(),
        "the profiled surface registers");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::ReflectionRecordRange Candidates =
      Snapshot.Symbols(Luna::SymbolKind::FunctionCandidate);
  Luna::SymbolId Expected;
  for (std::size_t Index = 0; Index < Candidates.Size(); ++Index) {
    if (Candidates.At(Index).QualifiedName() == "Scale")
      Expected = Candidates.At(Index).Id();
  }
  Check(Expected.IsValid(), "the callable has one canonical candidate symbol");

  RecordedEvents Recorded;
  Check(
      Registry
          .InstallProfilingHook([&Recorded](const Luna::ProfilingEvent &Event) {
            Recorded.Record(Event);
          })
          .IsSuccess(),
      "installing a profiling hook succeeds");

  Check(Owner.Execute("assert(Scale(21) == 42)").IsSuccess(),
        "the profiled call still succeeds exactly as before");

  Check(Recorded.Events.size() == 1, "one event is reported per call");
  Check(Recorded.Events.front().Kind == Luna::ProfilingEventKind::Completed,
        "a successful call reports the completed stage");
  Check(Recorded.Events.front().Symbol == Expected,
        "the reported symbol is the same one reflection publishes");
  Check(!Recorded.Events.front().ReceiverType.IsValid(),
        "a call with no receiver reports no receiver type");
  Check(Recorded.Events.front().QualifiedName == "Scale",
        "the reported name is the callable's qualified name");
}

void CheckFailedCallsReportTheSameIdentity() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.RegisterFunction("Fail", &Fail).IsSuccess(),
        "the failing surface registers");

  RecordedEvents Recorded;
  Check(
      Registry
          .InstallProfilingHook([&Recorded](const Luna::ProfilingEvent &Event) {
            Recorded.Record(Event);
          })
          .IsSuccess(),
      "installing a profiling hook succeeds");

  const auto Result = Owner.Execute("Fail(1)");
  Check(!Result.IsSuccess(), "the profiled call still fails exactly as before");
  Check(Recorded.Events.size() == 1 &&
            Recorded.Events.front().Kind == Luna::ProfilingEventKind::Failed,
        "a failing call reports the failed stage");
  Check(Recorded.Events.front().Symbol.IsValid(),
        "a call that resolved a candidate before failing reports its symbol");
  Check(Recorded.Events.front().QualifiedName == "Fail",
        "the failure report still names the callable");
}

void CheckMemberCallsReportTheReceiverType() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::ClassBuilder<Cursor> Class = Registry.RegisterClass<Cursor>(
      "Cursor", Luna::StableTypeKey("Studio.Cursor"));
  Class.Constructor<>().Method("Advance", &Cursor::Advance);
  Check(Class.Commit().IsSuccess(), "the class surface registers");

  const Luna::ReflectionSnapshot Snapshot = Registry.Reflection();
  const Luna::TypeRecordRange Types = Snapshot.Types();
  Luna::TypeId ExpectedReceiver;
  for (std::size_t Index = 0; Index < Types.Size(); ++Index) {
    if (Types.At(Index).Name() == "Cursor")
      ExpectedReceiver = Types.At(Index).Id();
  }
  Check(ExpectedReceiver.IsValid(), "the class has one canonical type");

  RecordedEvents Recorded;
  Check(
      Registry
          .InstallProfilingHook([&Recorded](const Luna::ProfilingEvent &Event) {
            Recorded.Record(Event);
          })
          .IsSuccess(),
      "installing a profiling hook succeeds");

  Check(Owner
            .Execute("Value = Cursor.New()\n"
                     "assert(Value:Advance() == 1)")
            .IsSuccess(),
        "constructing and calling a method still succeeds");

  bool FoundMemberCall = false;
  for (const Luna::ProfilingEvent &Event : Recorded.Events) {
    if (Event.QualifiedName == "Cursor.Advance") {
      FoundMemberCall = true;
      Check(Event.Kind == Luna::ProfilingEventKind::Completed,
            "the method call reports the completed stage");
      Check(Event.ReceiverType == ExpectedReceiver,
            "a member call reports its canonical receiver type");
    }
  }
  Check(FoundMemberCall, "the method call itself is reported");
}

void CheckSuspendedCallsReportEveryStage() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.RegisterFunction("ScaleLater", &ScaleLater).IsSuccess(),
        "the asynchronous surface registers");

  RecordedEvents Recorded;
  Check(
      Registry
          .InstallProfilingHook([&Recorded](const Luna::ProfilingEvent &Event) {
            Recorded.Record(Event);
          })
          .IsSuccess(),
      "installing a profiling hook succeeds");

  Check(Owner.Execute("assert(ScaleLater(21) == 42)").IsSuccess(),
        "the suspended call still resumes exactly as before");

  Check(Recorded.Events.size() == 3, "every stage of one suspension reports");
  Check(Recorded.Events[0].Kind == Luna::ProfilingEventKind::Suspended &&
            Recorded.Events[1].Kind == Luna::ProfilingEventKind::Resumed &&
            Recorded.Events[2].Kind == Luna::ProfilingEventKind::Completed,
        "the stages report in the order they actually happened");
  Check(Recorded.Events[0].Symbol == Recorded.Events[2].Symbol,
        "every reported stage of one call names the same symbol");
}

void CheckRepresentativeSurfaceReportsThroughTheRealMachine() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();

  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Studio.RegisterFunction("Scale", Luna::Overload<int(int)>(&Scale));
  Studio.RegisterFunction(
      "Scale", Luna::Overload<int(int, int)>(
                   [](int Value, int Factor) { return Value * Factor; }));
  Luna::ClassBuilder<Cursor> Class = Studio.RegisterClass<Cursor>(
      "Cursor", Luna::StableTypeKey("Studio.Cursor"));
  Class.Constructor<>().Method("Advance", &Cursor::Advance);
  Check(Studio.Commit().IsSuccess(), "the representative surface registers");

  const auto Manifest = Luna::ModuleManifest::TryCreate(
      "studio.extra", Luna::SemanticVersion::TryParse("1.0.0").value(), {},
      "Extra module.", {});
  Check(Manifest.has_value(), "the module manifest is valid");
  Check(Registry
            .RegisterModule(*Manifest,
                            [](Luna::NamespaceBuilder &Builder) {
                              Luna::NamespaceBuilder Extra =
                                  Builder.RegisterNamespace("Extra");
                              static_cast<void>(
                                  Extra.RegisterConstant("Ready", true));
                            })
            .IsSuccess(),
        "the module loads");

  RecordedEvents Recorded;
  Check(
      Registry
          .InstallProfilingHook([&Recorded](const Luna::ProfilingEvent &Event) {
            Recorded.Record(Event);
          })
          .IsSuccess(),
      "installing a profiling hook succeeds");

  Check(Owner
            .Execute("assert(Studio.Scale(3) == 6)\n"
                     "assert(Studio.Scale(3, 4) == 12)\n"
                     "local Value = Studio.Cursor.New()\n"
                     "assert(Value:Advance() == 1)\n"
                     "assert(Extra.Ready)")
            .IsSuccess(),
        "the representative surface runs exactly as before profiling");

  bool SawSingleArgumentOverload = false;
  bool SawTwoArgumentOverload = false;
  bool SawMethod = false;
  for (const Luna::ProfilingEvent &Event : Recorded.Events) {
    if (Event.QualifiedName == "Studio.Scale" &&
        Event.Kind == Luna::ProfilingEventKind::Completed) {
      if (!SawSingleArgumentOverload)
        SawSingleArgumentOverload = true;
      else
        SawTwoArgumentOverload = true;
    }
    if (Event.QualifiedName == "Studio.Cursor.Advance")
      SawMethod = true;
  }
  Check(SawSingleArgumentOverload && SawTwoArgumentOverload,
        "every resolved overload of one name is reported separately");
  Check(SawMethod, "the class method is reported with its receiver type");
  Check(Recorded.Events.size() == 4,
        "the module load itself invokes no profiled callable");
}

void CheckClearingTheHookStopsReporting() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.RegisterFunction("Scale", &Scale).IsSuccess(),
        "the profiled surface registers");

  RecordedEvents Recorded;
  Check(
      Registry
          .InstallProfilingHook([&Recorded](const Luna::ProfilingEvent &Event) {
            Recorded.Record(Event);
          })
          .IsSuccess(),
      "installing a profiling hook succeeds");
  Check(Owner.Execute("assert(Scale(1) == 2)").IsSuccess(),
        "a profiled call succeeds while the hook is installed");
  Check(Recorded.Events.size() == 1, "the installed hook reports one event");

  Check(Registry.ClearProfilingHook().IsSuccess(),
        "clearing the profiling hook succeeds");
  Check(Owner.Execute("assert(Scale(2) == 4)").IsSuccess(),
        "the callable still works after clearing the hook");
  Check(Recorded.Events.size() == 1,
        "no further event reaches the cleared hook");
}

void CheckAThrowingHookIsContainedAndUninstalled() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Check(Registry.RegisterFunction("Scale", &Scale).IsSuccess(),
        "the profiled surface registers");

  int Calls = 0;
  Check(Registry
            .InstallProfilingHook([&Calls](const Luna::ProfilingEvent &) {
              ++Calls;
              throw std::runtime_error("the hook itself failed");
            })
            .IsSuccess(),
        "installing a throwing profiling hook still succeeds");

  const int EntryDepth = Hooks::ObserveRootStackDepth(Owner).value_or(-1);
  Check(Owner.Execute("assert(Scale(3) == 6)").IsSuccess(),
        "a throwing hook never reaches Luau or fails the profiled call");
  Check(Hooks::ObserveRootStackDepth(Owner).value_or(-1) == EntryDepth,
        "a throwing hook leaves the exact entry stack depth");
  Check(Calls == 1, "the throwing hook ran exactly once");

  Check(Owner.Execute("assert(Scale(4) == 8)").IsSuccess(),
        "the State keeps working after a throwing hook");
  Check(Calls == 1, "the throwing hook is uninstalled after it threw");
}

void CheckInstallingRequiresTheOwnerThreadAndAReadyState() {
  Luna::State Owner;
  const Luna::RegistrationResult Empty =
      Owner.Bindings().InstallProfilingHook(Luna::ProfilingHook());
  Check(!Empty.IsSuccess() && Empty.Diagnostic() != nullptr,
        "installing an empty hook is refused deterministically");

  bool Observed = false;
  std::thread Foreign([&Owner, &Observed]() {
    const Luna::RegistrationResult Result =
        Owner.Bindings().InstallProfilingHook(
            [](const Luna::ProfilingEvent &) {});
    Observed = !Result.IsSuccess();
  });
  Foreign.join();
  Check(Observed, "installing a hook from another thread is refused");

  Check(Owner.Bindings()
            .InstallProfilingHook([](const Luna::ProfilingEvent &) {})
            .IsSuccess(),
        "the owner thread may still install a hook afterward");
}

} // namespace

int RunProfilingHookTests() {
  FailureCount = 0;
  CheckSuccessfulCallsReportTheCanonicalIdentity();
  CheckFailedCallsReportTheSameIdentity();
  CheckMemberCallsReportTheReceiverType();
  CheckRepresentativeSurfaceReportsThroughTheRealMachine();
  CheckSuspendedCallsReportEveryStage();
  CheckClearingTheHookStopsReporting();
  CheckAThrowingHookIsContainedAndUninstalled();
  CheckInstallingRequiresTheOwnerThreadAndAReadyState();
  return FailureCount == 0 ? 0 : 1;
}
