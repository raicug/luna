// clang-format off
#include <luna/tooling/profiling_hook.hpp>

#include <type_traits>
// clang-format on

namespace {

static_assert(std::is_default_constructible_v<Luna::ProfilingEvent>,
              "A profiling event must be default constructible.");
static_assert(std::is_copy_constructible_v<Luna::ProfilingEvent>,
              "A profiling event must be an ordinary owning value.");
static_assert(std::is_default_constructible_v<Luna::ProfilingHook>,
              "A profiling hook must be default constructible so an "
              "uninstalled hook is the empty state.");

static_assert(
    Luna::ProfilingEventKindText(Luna::ProfilingEventKind::Completed) ==
            "completed" &&
        Luna::ProfilingEventKindText(Luna::ProfilingEventKind::Failed) ==
            "failed" &&
        Luna::ProfilingEventKindText(Luna::ProfilingEventKind::Suspended) ==
            "suspended" &&
        Luna::ProfilingEventKindText(Luna::ProfilingEventKind::Resumed) ==
            "resumed" &&
        Luna::ProfilingEventKindText(Luna::ProfilingEventKind::Cancelled) ==
            "cancelled",
    "Every profiling event kind must format canonically.");

} // namespace

void VerifyProfilingHookHeaderCompilesStandalone() {
  Luna::ProfilingEvent Event;
  Event.Kind = Luna::ProfilingEventKind::Completed;
  Event.QualifiedName = "Studio.Sprite.Grow";

  static_cast<void>(Event.Symbol.IsValid());
  static_cast<void>(Event.ReceiverType.IsValid());
  static_cast<void>(Event.QualifiedName.empty());

  const Luna::ProfilingHook Hook = [](const Luna::ProfilingEvent &Observed) {
    static_cast<void>(Observed.Kind);
  };
  static_cast<void>(static_cast<bool>(Hook));
}
