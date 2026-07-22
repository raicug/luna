#pragma once

// clang-format off
#include <memory>
// clang-format on

namespace Luna {
class Runtime {
public:
  Runtime();
  ~Runtime();

  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  Runtime(Runtime &&) noexcept;
  Runtime &operator=(Runtime &&) noexcept;

  [[nodiscard]] bool IsReady() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> Implementation;
};
} // namespace Luna
