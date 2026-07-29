#pragma once

// clang-format off
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include <functional>
#include <string>
#include <string_view>
// clang-format on

namespace Luna {

// A profiling or debug-UI consumer observes only these stages. None of them
// exists to change how a call resolves, converts, or publishes its result;
// they are reported after Luna has already decided the outcome.
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

// One reported invocation stage. `Symbol` and `ReceiverType` are the same
// canonical `SymbolId` and `TypeId` values reflection and generation use,
// never a private duplicate schema. Both may be invalid: `Symbol` is
// invalid only when Luna could not resolve the callable at all before
// failing, and `ReceiverType` is invalid whenever the callable has no
// receiver. `QualifiedName` is an owning value, so a hook that stores an
// event for later inspection retains a valid name after the call returns.
struct ProfilingEvent final {
  ProfilingEventKind Kind = ProfilingEventKind::Completed;
  SymbolId Symbol;
  TypeId ReceiverType;
  std::string QualifiedName;
};

// A profiling or debug-UI hook. It runs on the State's owner thread only,
// after Luna has already produced the reported outcome, so it can never
// change invocation semantics. An exception it raises is contained and
// never reaches Luau or the calling C++ code; it simply stops being called
// until reinstalled.
using ProfilingHook = std::function<void(const ProfilingEvent &)>;

} // namespace Luna
