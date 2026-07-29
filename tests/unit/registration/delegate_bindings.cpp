// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/delegate.hpp>
#include <luna/binding/parameter_descriptor.hpp>
#include <luna/binding/signal.hpp>
#include <luna/core/diagnostics/error_category.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>

#include "state/testing/test_hooks.hpp"

#include <cstddef>
#include <functional>
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
  std::cerr << "delegate binding check failed: " << Description << '\n';
}

// One native event source shared by the registered callables. It owns every
// subscribed handler as an ordinary delegate.
struct Hub final {
  Luna::Signal<void(int)> Damage;
  Luna::Signal<bool(int)> Filter;
  std::vector<int> Delivered;
  std::vector<int> Reentrant;
  int UnsubscribeDuringEmit = 0;
  int SubscribeDuringEmit = 0;
  Luna::Delegate<void(int)> Retained;
};

Hub *Active = nullptr;

[[nodiscard]] int Subscribe(Luna::Delegate<void(int)> Handler) {
  if (!Active)
    return 0;
  Active->Retained = Handler;
  return Active->Damage.Subscribe(std::move(Handler));
}

[[nodiscard]] bool Unsubscribe(int Token) {
  return Active && Active->Damage.Unsubscribe(Token);
}

[[nodiscard]] int Emit(int Amount) {
  if (!Active)
    return -1;
  const Luna::SignalEmission Reported = Active->Damage.Emit(Amount);
  return static_cast<int>(Reported.Delivered);
}

[[nodiscard]] int Deliveries() {
  return Active ? static_cast<int>(Active->Delivered.size()) : -1;
}

[[nodiscard]] int Subscribers() {
  return Active ? static_cast<int>(Active->Damage.SubscriberCount()) : -1;
}

[[nodiscard]] int Record(int Amount) {
  if (Active)
    Active->Delivered.push_back(Amount);
  return Amount;
}

void CheckDelegateParametersDeclareCanonicalDescriptors() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Hub Local;
  Active = &Local;

  Check(
      Registry.RegisterFunction("Subscribe", &Subscribe).IsSuccess() &&
          Registry.RegisterFunction("Unsubscribe", &Unsubscribe).IsSuccess() &&
          Registry.RegisterFunction("Emit", &Emit).IsSuccess(),
      "subscribing, unsubscribing, and emitting register as ordinary "
      "callables");

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

  const Luna::ReflectionRecord Subscribing = CandidateOf("Subscribe");
  Check(Published.Find("Subscribe").IsValid() &&
            Published.Find("Subscribe").Kind() == Luna::SymbolKind::OverloadSet,
        "a delegate callable publishes the same canonical overload set");
  Check(Subscribing.IsValid() && Subscribing.ParameterCount() == 1,
        "a delegate parameter is published as one canonical parameter");

  const Luna::ParameterRecord Handler = Subscribing.IsValid()
                                            ? Subscribing.Parameter(0)
                                            : Luna::ParameterRecord();
  Check(Handler.IsValid() &&
            Handler.Disposition() == Luna::ParameterDisposition::Required,
        "a subscribed handler is always supplied");
  Check(Handler.IsValid() && Handler.Type().IsValid() &&
            Handler.Descriptor().Kind() == Luna::TypeKind::Callable &&
            Handler.Descriptor().ChildCount() == 2,
        "a delegate parameter names one canonical callable type");
  Check(Subscribing.IsValid() &&
            Subscribing.Signature().find("callable") != std::string_view::npos,
        "the canonical signature carries the delegate call shape");

  Active = nullptr;
}

void CheckSubscribedHandlersRunAndRelease() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Hub Local;
  Active = &Local;

  Check(
      Registry.RegisterFunction("Subscribe", &Subscribe).IsSuccess() &&
          Registry.RegisterFunction("Unsubscribe", &Unsubscribe).IsSuccess() &&
          Registry.RegisterFunction("Emit", &Emit).IsSuccess() &&
          Registry.RegisterFunction("Record", &Record).IsSuccess() &&
          Registry.RegisterFunction("Deliveries", &Deliveries).IsSuccess() &&
          Registry.RegisterFunction("Subscribers", &Subscribers).IsSuccess(),
      "the event surface registers");

  const int EntryDepth = Hooks::ObserveRootStackDepth(Owner).value_or(-1);

  Check(Owner
            .Execute("Token = Subscribe(function(Amount) Record(Amount) end)\n"
                     "assert(Token == 1)\n"
                     "assert(Emit(7) == 1)\n"
                     "assert(Deliveries() == 1)")
            .IsSuccess(),
        "a script subscribes a handler and one emission delivers it");
  Check(Local.Delivered.size() == 1 && Local.Delivered.front() == 7,
        "the handler received the emitted value");
  Check(Hooks::OutstandingDelegateCount(Owner) == 1,
        "one subscribed handler is retained through Luna's own reference");
  Check(Hooks::ObserveRootStackDepth(Owner).value_or(-1) == EntryDepth,
        "subscribing and emitting restore the exact entry stack depth");

  Check(Owner
            .Execute("assert(Unsubscribe(Token))\n"
                     "assert(Emit(9) == 0)\n"
                     "assert(Deliveries() == 1)")
            .IsSuccess(),
        "unsubscribing stops delivery immediately");
  Check(!Local.Retained.IsValid(),
        "unsubscribing releases the handler for every copy");
  Check(Hooks::OutstandingDelegateCount(Owner) == 0,
        "unsubscribing releases the retained reference deterministically");

  const Luna::DelegateCallResult Refused = Local.Retained.Invoke(1);
  Check(!Refused.IsSuccess() &&
            Refused.Status() == Luna::DelegateStatus::Released &&
            !Refused.Diagnostic().empty(),
        "calling a released handler reports one deterministic diagnostic");

  Check(Owner.Execute("assert(Subscribers() == 0)").IsSuccess() &&
            Hooks::ObserveRootStackDepth(Owner).value_or(-1) == EntryDepth,
        "the State stays reusable with an exact stack after every release");

  const Luna::Detail::DelegateCounters Counters =
      Hooks::DelegateCountersOf(Owner);
  Check(Counters.Adopted == 1 && Counters.Released == 1 &&
            Counters.Invocations == 1 && Counters.Failures == 1,
        "the registry counts every adoption, release, call, and refusal");

  Active = nullptr;
}

void CheckWrongArgumentsAreRefusedBeforeAdoption() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Hub Local;
  Active = &Local;

  Check(Registry.RegisterFunction("Subscribe", &Subscribe).IsSuccess(),
        "subscribing registers");
  const int EntryDepth = Hooks::ObserveRootStackDepth(Owner).value_or(-1);

  const auto Refused = Owner.Execute("Subscribe(42)");
  Check(!Refused.IsSuccess() && Refused.Diagnostic() != nullptr,
        "a non-function argument refuses the call");
  Check(Refused.Diagnostic() && Refused.Diagnostic()->Message().find(
                                    "expected function") != std::string::npos,
        "the refusal names the expected function argument");
  Check(Hooks::OutstandingDelegateCount(Owner) == 0 &&
            Hooks::DelegateCountersOf(Owner).Adopted == 0,
        "a refused call adopts no handler");
  Check(Hooks::ObserveRootStackDepth(Owner).value_or(-1) == EntryDepth,
        "a refused call restores the exact entry stack depth");

  const auto Missing = Owner.Execute("Subscribe()");
  Check(!Missing.IsSuccess() && Missing.Diagnostic() != nullptr,
        "an omitted handler refuses the call");
  Check(Hooks::ObserveRootStackDepth(Owner).value_or(-1) == EntryDepth,
        "an omitted handler leaves the stack exact");

  Active = nullptr;
}

void CheckHandlerFailuresTranslateDeterministically() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Hub Local;
  Active = &Local;

  Check(Registry.RegisterFunction("Subscribe", &Subscribe).IsSuccess() &&
            Registry.RegisterFunction("Emit", &Emit).IsSuccess(),
        "the failing-handler surface registers");
  const int EntryDepth = Hooks::ObserveRootStackDepth(Owner).value_or(-1);

  Check(Owner
            .Execute("Subscribe(function() error('handler refused') end)\n"
                     "assert(Emit(1) == 0)")
            .IsSuccess(),
        "a failing handler does not fail the emission that called it");
  Check(Hooks::ObserveRootStackDepth(Owner).value_or(-1) == EntryDepth,
        "a failing handler restores the exact stack");

  const Luna::SignalEmission Reported = Local.Damage.Emit(2);
  Check(Reported.Failed == 1 && Reported.Delivered == 0 &&
            Reported.Diagnostic.find("handler refused") != std::string::npos,
        "the emission reports the handler failure verbatim");

  const Luna::DelegateCallResult Direct = Local.Retained.Invoke(3);
  Check(!Direct.IsSuccess() &&
            Direct.Status() == Luna::DelegateStatus::HandlerFailed,
        "a handler failure is translated, never thrown");

  bool Translated = false;
  try {
    Local.Retained(4);
  } catch (const Luna::DelegateFailure &Failure) {
    Translated = Failure.Status() == Luna::DelegateStatus::HandlerFailed;
  } catch (...) {
  }
  Check(Translated,
        "the throwing call operator reports the same failure as one exception");

  Active = nullptr;
}

void CheckDeclaredResultsAndMismatchesAreExact() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Hub Local;
  Active = &Local;

  Check(Registry
            .RegisterFunction(
                "Filter",
                +[](Luna::Delegate<bool(int)> Handler) {
                  if (!Active)
                    return false;
                  Active->Filter.Clear();
                  const int Token =
                      Active->Filter.Subscribe(std::move(Handler));
                  if (Token == 0)
                    return false;
                  return Active->Filter.Emit(5).Delivered == 1;
                })
            .IsSuccess(),
        "a result-producing delegate registers");

  Check(Owner.Execute("assert(Filter(function(Amount) return Amount > 1 end))")
            .IsSuccess(),
        "a handler publishes its declared result");

  Check(Owner.Execute("assert(not Filter(function() return 'text' end))")
            .IsSuccess(),
        "a handler that publishes the wrong type refuses the call");

  Active = nullptr;
}

void CheckReentrantSubscriptionStaysSafe() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Hub Local;
  Active = &Local;

  Check(
      Registry.RegisterFunction("Subscribe", &Subscribe).IsSuccess() &&
          Registry.RegisterFunction("Unsubscribe", &Unsubscribe).IsSuccess() &&
          Registry.RegisterFunction("Emit", &Emit).IsSuccess() &&
          Registry.RegisterFunction("Record", &Record).IsSuccess() &&
          Registry.RegisterFunction("Subscribers", &Subscribers).IsSuccess(),
      "the reentrancy surface registers");

  // The first handler unsubscribes itself and subscribes another handler
  // while the emission is still running.
  Check(Owner
            .Execute("First = Subscribe(function(Amount)\n"
                     "  Record(Amount)\n"
                     "  assert(Unsubscribe(First))\n"
                     "  Second = Subscribe(function(Later) Record(-Later) "
                     "end)\n"
                     "end)\n"
                     "assert(Emit(1) == 1)")
            .IsSuccess(),
        "a handler may unsubscribe itself and subscribe another mid-emission");
  Check(Local.Delivered.size() == 1 && Local.Delivered.front() == 1,
        "a handler added during an emission is not called by that emission");

  Check(Owner.Execute("assert(Emit(2) == 1)").IsSuccess(),
        "the next emission delivers exactly the surviving subscriber");
  Check(Local.Delivered.size() == 2 && Local.Delivered.back() == -2,
        "the removed handler is never called again and the new one runs once");
  Check(Owner.Execute("assert(Subscribers() == 1)").IsSuccess(),
        "the subscriber list stays exact across reentrant mutation");

  Active = nullptr;
}

void CheckLifecycleAndStateBoundariesInvalidateHandlers() {
  Hub Local;
  Active = &Local;

  {
    Luna::State Owner;
    Luna::BindingRegistry Registry = Owner.Bindings();
    Check(Registry.RegisterFunction("Subscribe", &Subscribe).IsSuccess() &&
              Registry.RegisterFunction("Emit", &Emit).IsSuccess() &&
              Registry.RegisterFunction("Record", &Record).IsSuccess(),
          "the lifetime surface registers");

    Check(Owner.Execute("Subscribe(function(Amount) Record(Amount) end)")
              .IsSuccess(),
          "a handler is subscribed before the lifecycle moves");
    Check(Local.Retained.IsValid() &&
              Hooks::OutstandingDelegateCount(Owner) == 1,
          "the handler is live before the lifecycle moves");

    // Freezing closes registration but never retires a live handler.
    Check(Registry.Freeze().IsSuccess(), "the State freezes");
    Check(Local.Retained.IsValid() && Local.Damage.Emit(4).Delivered == 1,
          "a frozen State keeps delivering to its subscribed handlers");

    // Replacing the lifecycle generation retires every handler subscribed
    // through the generation that went away.
    Check(Hooks::AdvanceLifecycleGeneration(Owner),
          "the lifecycle generation advances");
    Check(!Local.Retained.IsValid() &&
              Hooks::OutstandingDelegateCount(Owner) == 0,
          "replacing the lifecycle generation invalidates every handler");
    const Luna::SignalEmission Reported = Local.Damage.Emit(5);
    Check(Reported.Delivered == 0 && Reported.Skipped == 1,
          "an invalidated handler is skipped rather than called");
    Check(Local.Delivered.size() == 1 && Local.Delivered.front() == 4,
          "no invalidated handler ever runs again");

    Check(Owner.Execute("Subscribe(function(Amount) Record(Amount) end)")
                  .IsSuccess() &&
              Local.Retained.IsValid(),
          "the State keeps subscribing handlers after invalidation");
  }

  Check(!Local.Retained.IsValid(),
        "destroying the State invalidates every handler it owned");
  const Luna::DelegateCallResult Refused = Local.Retained.Invoke(6);
  Check(!Refused.IsSuccess() &&
            Refused.Status() == Luna::DelegateStatus::Released,
        "a handler that outlives its State refuses deterministically");

  Active = nullptr;
}

void CheckForeignThreadsRefuseHandlerCalls() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Hub Local;
  Active = &Local;

  Check(Registry.RegisterFunction("Subscribe", &Subscribe).IsSuccess() &&
            Registry.RegisterFunction("Record", &Record).IsSuccess(),
        "the owner-thread surface registers");
  Check(Owner.Execute("Subscribe(function(Amount) Record(Amount) end)")
            .IsSuccess(),
        "a handler is subscribed on the owner thread");

  Luna::DelegateStatus Observed = Luna::DelegateStatus::Ready;
  std::thread Foreign(
      [&Local, &Observed]() { Observed = Local.Retained.Invoke(11).Status(); });
  Foreign.join();

  Check(Observed == Luna::DelegateStatus::ForeignThread,
        "a handler call from another thread is refused");
  Check(Local.Delivered.empty(),
        "a refused foreign-thread call never reaches the handler");
  Check(Hooks::DelegateCountersOf(Owner).ForeignThreadRefusals == 1,
        "the registry counts every owner-thread refusal");
  Check(Local.Retained.IsValid() && Local.Damage.Emit(12).Delivered == 1,
        "the handler still runs on the owner thread afterwards");

  Active = nullptr;
}

} // namespace

int RunDelegateBindingTests() {
  FailureCount = 0;
  CheckDelegateParametersDeclareCanonicalDescriptors();
  CheckSubscribedHandlersRunAndRelease();
  CheckWrongArgumentsAreRefusedBeforeAdoption();
  CheckHandlerFailuresTranslateDeterministically();
  CheckDeclaredResultsAndMismatchesAreExact();
  CheckReentrantSubscriptionStaysSafe();
  CheckLifecycleAndStateBoundariesInvalidateHandlers();
  CheckForeignThreadsRefuseHandlerCalls();
  return FailureCount == 0 ? 0 : 1;
}
