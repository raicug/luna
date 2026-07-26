#pragma once

// Private counters that make one overload resolution observable.
//
// Resolution must probe without committing anything, and only the selected
// candidate may convert. Neither statement is observable from the outside: a
// refused call and a resolved call both look like one Luau call. These counters
// close that gap for Luna's own tests.
//
//   * `ArgumentProbes` counts every side-effect-free argument probe.
//   * `CommittingArgumentReads` counts every committing argument conversion.
//
// A refused resolution therefore reports probes and no committing read at all,
// and a resolved call reports exactly one committing read per supplied argument
// of the selected candidate. Nothing here is reachable from the public API, and
// the counters never influence resolution.

// clang-format off
#include <cstddef>
// clang-format on

namespace Luna::Detail {

struct OverloadInstrumentationCounts final {
  std::size_t ArgumentProbes = 0;
  std::size_t CommittingArgumentReads = 0;
};

void ResetOverloadInstrumentation() noexcept;

void RecordArgumentProbe() noexcept;
void RecordCommittingArgumentRead() noexcept;

[[nodiscard]] OverloadInstrumentationCounts
OverloadInstrumentationTotals() noexcept;

} // namespace Luna::Detail
