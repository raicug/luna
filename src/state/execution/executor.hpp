#pragma once

// clang-format off
#include <luna/core/results/execution_result.hpp>
#include <luna/state/chunk.hpp>

#include <string>
#include <string_view>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class AsyncCallRegistry;
class FaultInjector;

[[nodiscard]] ExecutionResult ExecuteSource(lua_State *Root,
                                            std::string_view Source,
                                            const ExecutionPolicy &Policy,
                                            FaultInjector &Faults,
                                            AsyncCallRegistry *Async);

[[nodiscard]] bool CompileChunk(lua_State *Root, std::string_view Source,
                                std::string_view Name, std::string &Bytecode,
                                std::string &Diagnostic);

[[nodiscard]] ChunkResult
InvokeChunk(lua_State *Root, std::string_view Bytecode, std::string_view Name,
            const ValuePack &Arguments, FaultInjector *Faults,
            AsyncCallRegistry *Async, const ExecutionPolicy &Policy);

} // namespace Luna::Detail
