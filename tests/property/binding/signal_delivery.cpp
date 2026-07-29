// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <rapidcheck.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

// One generated action against a single signal and its subscribers. The
// argument selects which subscriber a subscribe/unsubscribe acts on when that
// is meaningful, modulo the count ever subscribed at the time it runs.
enum class SignalActionKind : std::uint32_t {
  Subscribe,
  Unsubscribe,
  Emit,
  AdvanceLifecycleGeneration,
  Count
};

[[nodiscard]] SignalActionKind NormalizeAction(int Value) noexcept {
  return static_cast<SignalActionKind>(
      static_cast<std::uint32_t>(Value) %
      static_cast<std::uint32_t>(SignalActionKind::Count));
}

struct GeneratedAction final {
  SignalActionKind Kind = SignalActionKind::Subscribe;
  int Argument = 0;
};

// One subscription's independent state. `Tracked` mirrors whether the
// subscription is still present in the signal's own list; `Live` mirrors
// whether the handler still holds its native virtual-machine reference.
// A signal only drops an untracked, invalid entry lazily, when an emission
// finishes, so the two can disagree for a while: a handler invalidated by a
// lifecycle-generation replacement stays tracked (and therefore still
// unsubscribable by token) until the next emission or explicit unsubscribe
// removes it.
struct ModelHandler final {
  int Token = 0;
  bool Tracked = true;
  bool Live = true;
};

// The independent state machine. It predicts, for every generated action,
// exactly what a correct signal must do: whether a subscribe or unsubscribe
// call succeeds, how many deliveries one emission produces, and how many
// native references stay outstanding after a lifecycle-generation replaces
// every handler it invalidates.
struct SignalModel final {
  std::vector<ModelHandler> Handlers;
  int NextToken = 1;
  std::size_t TotalDelivered = 0;

  [[nodiscard]] std::size_t LiveCount() const noexcept {
    std::size_t Count = 0;
    for (const ModelHandler &Handler : Handlers) {
      if (Handler.Live)
        ++Count;
    }
    return Count;
  }

  int Subscribe() {
    const int Token = NextToken;
    ++NextToken;
    Handlers.push_back(ModelHandler{Token, true, true});
    return Token;
  }

  // Finds the handler at the requested ordinal among every handler ever
  // subscribed. Unsubscribing succeeds only while that handler is still
  // tracked by the signal, whether or not it is still callable.
  [[nodiscard]] bool Unsubscribe(int Position) {
    if (Handlers.empty())
      return false;
    const std::size_t Index =
        static_cast<std::size_t>(Position) % Handlers.size();
    ModelHandler &Handler = Handlers[Index];
    if (!Handler.Tracked)
      return false;
    Handler.Tracked = false;
    Handler.Live = false;
    return true;
  }

  // An emission delivers to every handler that is both tracked and live at
  // the moment it runs. Afterward, any handler the emission found tracked but
  // no longer live is dropped from tracking, the way a signal lazily forgets
  // an invalidated subscriber once it finishes emitting.
  [[nodiscard]] std::size_t Emit() {
    std::size_t Delivered = 0;
    for (ModelHandler &Handler : Handlers) {
      if (!Handler.Tracked)
        continue;
      if (Handler.Live)
        ++Delivered;
      else
        Handler.Tracked = false;
    }
    TotalDelivered += Delivered;
    return Delivered;
  }

  // Replacing the lifecycle generation invalidates every handler that still
  // holds a live native reference. It never removes a subscription from the
  // signal's own list by itself.
  void AdvanceGeneration() {
    for (ModelHandler &Handler : Handlers) {
      if (Handler.Live)
        Handler.Live = false;
    }
  }
};

struct Hub final {
  Luna::Signal<void()> Emitter;
  std::size_t Deliveries = 0;
};

Hub *Active = nullptr;

[[nodiscard]] int Subscribe(Luna::Delegate<void()> Handler) {
  return Active ? Active->Emitter.Subscribe(std::move(Handler)) : 0;
}

[[nodiscard]] bool Unsubscribe(int Token) {
  return Active && Active->Emitter.Unsubscribe(Token);
}

[[nodiscard]] int EmitOnce() {
  if (!Active)
    return 0;
  const Luna::SignalEmission Reported = Active->Emitter.Emit();
  return static_cast<int>(Reported.Delivered);
}

void Deliver() {
  if (Active)
    ++Active->Deliveries;
}

[[nodiscard]] int Trailing(int Value) {
  return Value + 1;
}

} // namespace

int RunSignalDeliveryProperties() {
  // clang-format off
  // Feature: reflection-driven-binding-system, Property 33: Signal subscription and delivery follow the retained-generation handler model
  const bool Passed = rc::check(
      // clang-format on
      "Signal subscription and delivery follow the retained-generation "
      "handler model",
      [](const std::vector<int> &GeneratedKinds,
         const std::vector<int> &GeneratedArguments) {
        Hub Local;
        Active = &Local;

        Luna::State Owner;
        Luna::BindingRegistry Registry = Owner.Bindings();
        RC_ASSERT(
            Registry.RegisterFunction("Subscribe", &Subscribe).IsSuccess());
        RC_ASSERT(
            Registry.RegisterFunction("Unsubscribe", &Unsubscribe).IsSuccess());
        RC_ASSERT(Registry.RegisterFunction("EmitOnce", &EmitOnce).IsSuccess());
        RC_ASSERT(Registry.RegisterFunction("Deliver", &Deliver).IsSuccess());

        const auto EntryDepth = Hooks::ObserveRootStackDepth(Owner);
        RC_ASSERT(EntryDepth.has_value());

        SignalModel Model;
        const std::size_t Count =
            GeneratedKinds.size() < 12 ? GeneratedKinds.size() : 12;

        for (std::size_t Index = 0; Index < Count; ++Index) {
          const GeneratedAction Action{NormalizeAction(GeneratedKinds[Index]),
                                       Index < GeneratedArguments.size()
                                           ? GeneratedArguments[Index]
                                           : static_cast<int>(Index)};

          switch (Action.Kind) {
          case SignalActionKind::Subscribe: {
            const int ExpectedToken = Model.Subscribe();
            const auto Result =
                Owner.Execute("assert(Subscribe(function() Deliver() end) == " +
                              std::to_string(ExpectedToken) + ")");
            RC_ASSERT(Result.IsSuccess());
            break;
          }
          case SignalActionKind::Unsubscribe: {
            if (Model.Handlers.empty())
              break;
            const std::size_t ModelIndex =
                static_cast<std::size_t>(Action.Argument) %
                Model.Handlers.size();
            const int Token = Model.Handlers[ModelIndex].Token;
            const bool Expected = Model.Unsubscribe(Action.Argument);
            const auto Result = Owner.Execute(
                std::string("assert(Unsubscribe(") + std::to_string(Token) +
                ") == " + (Expected ? "true" : "false") + ")");
            RC_ASSERT(Result.IsSuccess());
            break;
          }
          case SignalActionKind::Emit: {
            const std::size_t Expected = Model.Emit();
            const auto Result = Owner.Execute(
                "assert(EmitOnce() == " + std::to_string(Expected) + ")");
            RC_ASSERT(Result.IsSuccess());
            RC_ASSERT(Local.Deliveries == Model.TotalDelivered);
            break;
          }
          case SignalActionKind::AdvanceLifecycleGeneration:
            Model.AdvanceGeneration();
            RC_ASSERT(Hooks::AdvanceLifecycleGeneration(Owner));
            break;
          case SignalActionKind::Count:
            break;
          }

          // Every action leaves the entry stack depth exactly restored, and
          // the outstanding native reference count matches the model's live
          // count, whatever mix of subscriptions, releases, and lifecycle
          // replacements produced it.
          RC_ASSERT(Hooks::ObserveRootStackDepth(Owner) == EntryDepth);
          RC_ASSERT(Hooks::OutstandingDelegateCount(Owner) ==
                    Model.LiveCount());
        }

        // The State stays reusable after every generated sequence, through an
        // unrelated callable that touches no signal.
        RC_ASSERT(Registry.RegisterFunction("Trailing", &Trailing).IsSuccess());
        RC_ASSERT(Owner.Execute("assert(Trailing(1) == 2)").IsSuccess());

        const Luna::Detail::DelegateCounters Counters =
            Hooks::DelegateCountersOf(Owner);
        RC_ASSERT(Counters.Invocations == Model.TotalDelivered);

        Active = nullptr;
        return true;
      });

  return Passed ? 0 : 1;
}
