// clang-format off
#include "state/invocation/overload/instrumentation.hpp"

#include <cstddef>
// clang-format on

namespace Luna::Detail {
namespace {

// One count per thread. Every invocation of one call runs on the thread that
// entered the virtual machine, so a counted resolution is never mixed with the
// resolution of another thread's State.
thread_local OverloadInstrumentationCounts Counts;

} // namespace

void ResetOverloadInstrumentation() noexcept {
  Counts = OverloadInstrumentationCounts{};
}

void RecordArgumentProbe() noexcept { ++Counts.ArgumentProbes; }

void RecordCommittingArgumentRead() noexcept {
  ++Counts.CommittingArgumentReads;
}

OverloadInstrumentationCounts OverloadInstrumentationTotals() noexcept {
  return Counts;
}

} // namespace Luna::Detail
