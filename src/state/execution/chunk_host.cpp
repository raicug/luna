// clang-format off
#include "state/execution/chunk_host.hpp"

#include "state/execution/executor.hpp"

#include <string>
#include <thread>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

class NestedChunkScope final {
public:
  explicit NestedChunkScope(int &Depth) noexcept : DepthValue(Depth) {
    DepthValue += 1;
  }

  ~NestedChunkScope() { DepthValue -= 1; }

  NestedChunkScope(const NestedChunkScope &) = delete;
  NestedChunkScope &operator=(const NestedChunkScope &) = delete;

private:
  int &DepthValue;
};

} // namespace

void ChunkHost::Bind(lua_State *Root, FaultInjector *Faults,
                     AsyncCallRegistry *Async) noexcept {
  RootValue = Root;
  OwnerValue = std::this_thread::get_id();
  AliveValue = Root != nullptr;
  FaultsValue = Faults;
  AsyncValue = Async;
}

void ChunkHost::Retire() noexcept {
  AliveValue = false;
  RootValue = nullptr;
  FaultsValue = nullptr;
  AsyncValue = nullptr;
}

bool ChunkHost::IsAlive() const noexcept {
  return AliveValue && RootValue != nullptr;
}

bool ChunkHost::IsOwnerThread() const noexcept {
  return std::this_thread::get_id() == OwnerValue;
}

bool ChunkHost::Compile(std::string_view Source, std::string_view Name,
                        std::string &Bytecode, std::string &Diagnostic) {
  if (!IsAlive()) {
    Diagnostic = "the State that would own this chunk is not ready.";
    return false;
  }
  if (!IsOwnerThread()) {
    Diagnostic = "a chunk is compiled on the State's owner thread only.";
    return false;
  }
  return CompileChunk(RootValue, Source, Name, Bytecode, Diagnostic);
}

ChunkResult ChunkHost::Invoke(std::string_view Bytecode, std::string_view Name,
                              const ValuePack &Arguments) {
  if (!IsAlive())
    return ChunkResult::Failure(
        ErrorCategory::StateNotReady,
        "State not ready: the State that owns this chunk is gone.");
  if (!IsOwnerThread())
    return ChunkResult::Failure(
        ErrorCategory::StateNotReady,
        "State not ready: a chunk runs only on the State's owner thread.");
  if (DepthValue >= MaximumNestingDepth)
    return ChunkResult::Failure(
        ErrorCategory::Runtime,
        "Runtime error: chunk invocation nests deeper than " +
            std::to_string(MaximumNestingDepth) +
            " levels, so this call is refused rather than exhausting the "
            "host stack.");

  const NestedChunkScope Nested(DepthValue);
  return InvokeChunk(RootValue, Bytecode, Name, Arguments, FaultsValue,
                     AsyncValue);
}

} // namespace Luna::Detail
