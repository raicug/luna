#pragma once

// clang-format off
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include <functional>
#include <string>
#include <string_view>
// clang-format on

namespace Luna {

enum class ProfilingEventKind {
  Completed,

  Failed,

  Suspended,

  Resumed,

  Cancelled
};

[[nodiscard]] constexpr std::string_view
ProfilingEventKindText(ProfilingEventKind Kind) noexcept {
  switch (Kind) {
  case ProfilingEventKind::Completed:
    return "completed";
  case ProfilingEventKind::Failed:
    return "failed";
  case ProfilingEventKind::Suspended:
    return "suspended";
  case ProfilingEventKind::Resumed:
    return "resumed";
  case ProfilingEventKind::Cancelled:
    return "cancelled";
  }
  return "completed";
}

struct ProfilingEvent final {
  ProfilingEventKind Kind = ProfilingEventKind::Completed;
  SymbolId Symbol;
  TypeId ReceiverType;
  std::string QualifiedName;
};

using ProfilingHook = std::function<void(const ProfilingEvent &)>;

} // namespace Luna
