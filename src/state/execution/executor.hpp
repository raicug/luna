#pragma once

// clang-format off
#include <luna/core/results/execution_result.hpp>

#include <string_view>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class AsyncCallRegistry;
class FaultInjector;

[[nodiscard]] ExecutionResult ExecuteSource(lua_State *Root,
                                            std::string_view Source,
                                            FaultInjector &Faults,
                                            AsyncCallRegistry *Async);

} // namespace Luna::Detail
