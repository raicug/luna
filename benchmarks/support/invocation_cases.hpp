#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace LunaBenchmark::InvocationCases {

inline constexpr std::size_t LoopCount = 250;
inline constexpr std::string_view VoidName = "VoidCall";
inline constexpr std::string_view ScalarName = "ScalarCall";
inline constexpr std::string_view DynamicName = "DynamicPackCall";
inline constexpr std::string_view VoidCorpus =
    "250 nullary native calls returning no value";
inline constexpr std::string_view ScalarCorpus =
    "250 two-argument native calls returning one value";
inline constexpr std::string_view DynamicCorpus =
    "250 native calls publishing one three-value dynamic pack";

[[nodiscard]] inline std::string VoidScript() {
  return "for Index = 1, " + std::to_string(LoopCount) +
         " do\n"
         "  Touch()\n"
         "end\n";
}

[[nodiscard]] inline std::string ScalarScript() {
  return "local Total = 0\nfor Index = 1, " + std::to_string(LoopCount) +
         " do\n"
         "  Total = Total + Add(Index, 1)\n"
         "end\n"
         "assert(Total > 0)\n";
}

[[nodiscard]] inline std::string DynamicScript() {
  return "local Total = 0\nfor Index = 1, " + std::to_string(LoopCount) +
         " do\n"
         "  local First, Second, Third = Dynamic(Index)\n"
         "  Total = Total + First + Second + Third\n"
         "end\n"
         "assert(Total > 0)\n";
}

} // namespace LunaBenchmark::InvocationCases
