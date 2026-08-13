// clang-format off
#include <luna/state/chunk.hpp>

#include "state/execution/chunk_host.hpp"

#include <utility>
// clang-format on

namespace Luna {

ChunkResult Chunk::Invoke() const {
  return Invoke(ValuePack());
}

ChunkResult Chunk::Invoke(const ValuePack &Arguments) const {
  if (DiagnosticValue)
    return ChunkResult::Failure(*DiagnosticValue);
  if (!HostValue || BytecodeValue.empty())
    return ChunkResult::Failure(
        ErrorCategory::StateNotReady,
        "State not ready: this chunk holds no loaded bytecode.");
  return HostValue->Invoke(BytecodeValue, NameValue, Arguments, PolicyValue);
}

} // namespace Luna
