// clang-format off
#include "state/testing/fault_injector.hpp"
// clang-format on

namespace Luna::Detail {

void FaultInjector::Inject(StateFaultPoint Point, std::size_t Count) noexcept {
  PendingCounts[Index(Point)] = Count;
}

bool FaultInjector::Consume(StateFaultPoint Point) noexcept {
  auto &Count = PendingCounts[Index(Point)];
  if (Count == 0)
    return false;
  --Count;
  return true;
}

std::size_t FaultInjector::Pending(StateFaultPoint Point) const noexcept {
  return PendingCounts[Index(Point)];
}

void FaultInjector::ClearCallbackStackRestoration() noexcept {
  CallbackStackRestoration.reset();
}

void FaultInjector::RecordCallbackStackRestoration(int EntryDepth,
                                                   int RestoredDepth,
                                                   int ErrorDepth) noexcept {
  CallbackStackRestoration =
      CallbackStackRestorationObservation{.EntryDepth = EntryDepth,
                                          .RestoredDepth = RestoredDepth,
                                          .ErrorDepth = ErrorDepth};
}

std::optional<CallbackStackRestorationObservation>
FaultInjector::LastCallbackStackRestoration() const noexcept {
  return CallbackStackRestoration;
}

} // namespace Luna::Detail
