#pragma once

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
