#pragma once

// The five foundation types behind the registry. `bool`, signed 32-bit `int`,
// `double`, `std::string`, and `void` are declared here as ordinary canonical
// type records, so the converters the foundation shipped are reached only
// through the immutable type generation an invocation captured.
//
// The records keep the foundation's behavior exactly: the same accepted Luau
// representations, the same 1,048,576-byte string policy, the same integer
// classification of non-finite, out-of-range, and fractional numbers, and the
// same public names the foundation's diagnostics printed.

// clang-format off
#include "state/type/type_record.hpp"

#include <vector>
// clang-format on

namespace Luna::Detail {

// The complete foundation declaration set, in declaration-independent content.
[[nodiscard]] std::vector<TypeRecord> FoundationTypeRecords();

} // namespace Luna::Detail
