#pragma once

// clang-format off
#include <string>
#include <variant>
// clang-format on

namespace Luna {

enum class ValueKind { Boolean, Integer, Number, String };

using Value = std::variant<bool, int, double, std::string>;

static_assert(sizeof(int) == 4,
              "Luna bindings require int to be a signed 32-bit value.");

} // namespace Luna
