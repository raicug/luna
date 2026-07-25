#pragma once

// clang-format off
#include "state/testing/fault_point.hpp"

#include <array>
#include <cstddef>
#include <optional>
// clang-format on

namespace Luna::Detail {

struct CallbackStackRestorationObservation final {
  int EntryDepth = 0;
  int RestoredDepth = 0;
  int ErrorDepth = 0;
};

class FaultInjector final {
public:
  void Inject(StateFaultPoint Point, std::size_t Count) noexcept;
  [[nodiscard]] bool Consume(StateFaultPoint Point) noexcept;
  [[nodiscard]] std::size_t Pending(StateFaultPoint Point) const noexcept;

  void ClearCallbackStackRestoration() noexcept;
  void RecordCallbackStackRestoration(int EntryDepth, int RestoredDepth,
                                      int ErrorDepth) noexcept;
  [[nodiscard]] std::optional<CallbackStackRestorationObservation>
  LastCallbackStackRestoration() const noexcept;

private:
  static constexpr std::size_t PointCount =
      static_cast<std::size_t>(StateFaultPoint::Count);
  [[nodiscard]] static constexpr std::size_t Index(StateFaultPoint Point) {
    return static_cast<std::size_t>(Point);
  }

  std::array<std::size_t, PointCount> PendingCounts{};
  std::optional<CallbackStackRestorationObservation> CallbackStackRestoration;
};

} // namespace Luna::Detail
