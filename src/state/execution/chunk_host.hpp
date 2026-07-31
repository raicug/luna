#pragma once

// clang-format off
#include <luna/state/chunk.hpp>

#include <string>
#include <string_view>
#include <thread>
// clang-format on

struct lua_State;

namespace Luna::Detail {

class AsyncCallRegistry;
class FaultInjector;

class ChunkHost final {
public:
  static constexpr int MaximumNestingDepth = 16;

  ChunkHost() = default;

  ChunkHost(const ChunkHost &) = delete;
  ChunkHost &operator=(const ChunkHost &) = delete;

  void Bind(lua_State *Root, FaultInjector *Faults,
            AsyncCallRegistry *Async) noexcept;

  void Retire() noexcept;

  [[nodiscard]] bool IsAlive() const noexcept;

  [[nodiscard]] bool IsOwnerThread() const noexcept;

  [[nodiscard]] int Depth() const noexcept { return DepthValue; }

  [[nodiscard]] bool Compile(std::string_view Source, std::string_view Name,
                             std::string &Bytecode, std::string &Diagnostic);

  [[nodiscard]] ChunkResult Invoke(std::string_view Bytecode,
                                   std::string_view Name,
                                   const ValuePack &Arguments);

private:
  lua_State *RootValue = nullptr;
  std::thread::id OwnerValue;
  bool AliveValue = false;
  FaultInjector *FaultsValue = nullptr;
  AsyncCallRegistry *AsyncValue = nullptr;
  int DepthValue = 0;
};

} // namespace Luna::Detail
