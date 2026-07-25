#pragma once

struct lua_State;

namespace Luna::Detail {

class StackCheckpoint final {
public:
  explicit StackCheckpoint(lua_State *State) noexcept;
  ~StackCheckpoint();

  StackCheckpoint(const StackCheckpoint &) = delete;
  StackCheckpoint &operator=(const StackCheckpoint &) = delete;

  [[nodiscard]] int EntryDepth() const noexcept { return Depth; }

private:
  lua_State *State = nullptr;
  int Depth = 0;
};

} // namespace Luna::Detail
