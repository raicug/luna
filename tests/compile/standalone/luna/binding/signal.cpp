// clang-format off
#include <luna/binding/signal.hpp>

#include <string>
#include <type_traits>
#include <utility>
// clang-format on

namespace {

using DamageSignal = Luna::Signal<void(int)>;

static_assert(std::is_default_constructible_v<DamageSignal>,
              "A signal must be default constructible.");
static_assert(!std::is_copy_constructible_v<DamageSignal>,
              "A signal must own its subscribers exclusively.");
static_assert(std::is_move_constructible_v<DamageSignal>,
              "A signal must be movable with its subscribers.");
static_assert(
    std::is_same_v<DamageSignal::HandlerType, Luna::Delegate<void(int)>>,
    "A signal must subscribe delegates of its own shape.");

} // namespace

void VerifySignalHeaderCompilesStandalone() {
  DamageSignal Damage;
  const int Refused = Damage.Subscribe(Luna::Delegate<void(int)>());
  const bool Removed = Damage.Unsubscribe(Refused);
  const Luna::SignalEmission Reported = Damage.Emit(3);

  Luna::Signal<bool(std::string)> Filter;
  const Luna::SignalEmission Filtered = Filter.Emit(std::string("text"));

  Damage.Clear();

  static_cast<void>(Refused);
  static_cast<void>(Removed);
  static_cast<void>(Reported.IsComplete());
  static_cast<void>(Reported.Delivered);
  static_cast<void>(Reported.Skipped);
  static_cast<void>(Reported.Failed);
  static_cast<void>(Reported.Diagnostic.empty());
  static_cast<void>(Damage.SubscriberCount());
  static_cast<void>(Damage.LiveSubscriberCount());
  static_cast<void>(Damage.EmitDepth());
  static_cast<void>(Damage.IsSubscribed(1));
  static_cast<void>(Filtered.Delivered);
}
