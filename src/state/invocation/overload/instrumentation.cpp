// clang-format off
#include "state/invocation/overload/instrumentation.hpp"

#include <cstddef>
// clang-format on

namespace Luna::Detail {
namespace {

thread_local OverloadInstrumentationCounts Counts;

}

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
