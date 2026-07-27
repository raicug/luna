// clang-format off
#include <luna/luna.hpp>

#include "state/testing/test_hooks.hpp"

#include <rapidcheck.h>

#include <cstdint>
#include <string_view>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;

enum class SourceKind : std::uint32_t {
  Success,
  CompilationFailure,
  RuntimeFailure,
  Count
};

[[nodiscard]] SourceKind NormalizeSourceKind(int Value) noexcept {
  return static_cast<SourceKind>(static_cast<std::uint32_t>(Value) %
                                 static_cast<std::uint32_t>(SourceKind::Count));
}

[[nodiscard]] int NormalizeDepth(int Value) noexcept {
  constexpr std::uint32_t ValidDepthCount = 65;
  return static_cast<int>(static_cast<std::uint32_t>(Value) % ValidDepthCount);
}

[[nodiscard]] std::string_view SourceFor(SourceKind Kind) noexcept {
  switch (Kind) {
  case SourceKind::Success:
    return "local value = 42\nassert(value == 42)";
  case SourceKind::CompilationFailure:
    return "local =";
  case SourceKind::RuntimeFailure:
    return "error('generated runtime failure')";
  case SourceKind::Count:
    break;
  }
  return "local =";
}

} // namespace

int RunExecutionStackBalanceProperties() {
  // clang-format off
  const bool Passed = rc::check(
      // clang-format on
      "Execution preserves stack depth",
      [](int GeneratedDepth, int GeneratedSourceKind) {
        Luna::State State;
        RC_ASSERT(State.IsReady());

        const int SeedDepth = NormalizeDepth(GeneratedDepth);
        RC_ASSERT(Hooks::SetRootStackDepth(State, SeedDepth));
        const auto EntryDepth = Hooks::ObserveRootStackDepth(State);
        RC_ASSERT(EntryDepth.has_value());
        RC_ASSERT(*EntryDepth == SeedDepth);

        const auto Kind = NormalizeSourceKind(GeneratedSourceKind);
        const auto Result = State.Execute(SourceFor(Kind));

        if (Kind == SourceKind::Success) {
          RC_ASSERT(Result.IsSuccess());
          RC_ASSERT(Result.Diagnostic() == nullptr);
        } else {
          RC_ASSERT(!Result.IsSuccess());
          RC_ASSERT(Result.Diagnostic() != nullptr);
          const auto ExpectedCategory = Kind == SourceKind::CompilationFailure
                                            ? Luna::ErrorCategory::Compilation
                                            : Luna::ErrorCategory::Runtime;
          RC_ASSERT(Result.Diagnostic()->Category() == ExpectedCategory);
        }

        RC_ASSERT(Hooks::ObserveRootStackDepth(State) == EntryDepth);
      });

  return Passed ? 0 : 1;
}
