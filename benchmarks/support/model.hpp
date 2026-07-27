#pragma once

// clang-format off
#include <luna/luna.hpp>

#include "support/harness.hpp"

#include <functional>
#include <iostream>
#include <string>
#include <string_view>
// clang-format on

namespace LunaBenchmark {

[[nodiscard]] inline std::string
DiagnosticText(const Luna::RegistrationResult &Result) {
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    return std::string(Diagnostic->Message());
  return "Luna returned no diagnostic";
}

[[nodiscard]] inline std::string
DiagnosticText(const Luna::ExecutionResult &Result) {
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    return std::string(Diagnostic->Message());
  return "Luna returned no diagnostic";
}

class ScenarioModel final {
public:
  using Configure =
      std::function<Luna::RegistrationResult(Luna::BindingRegistry &)>;

  ScenarioModel(CacheMode Mode, const Configure &Register) {
    if (!Owner.IsReady()) {
      BlockerValue = "the State is not ready";
      return;
    }
    Luna::BindingRegistry Registry = Owner.Bindings();
    const Luna::RegistrationResult Registered = Register(Registry);
    if (!Registered.IsSuccess()) {
      BlockerValue = DiagnosticText(Registered);
      return;
    }
    if (Mode == CacheMode::FrozenCached) {
      const Luna::RegistrationResult Frozen = Registry.Freeze();
      if (!Frozen.IsSuccess()) {
        BlockerValue = DiagnosticText(Frozen);
        return;
      }
    }
    PreparedValue = true;
  }

  ScenarioModel(const ScenarioModel &) = delete;
  ScenarioModel &operator=(const ScenarioModel &) = delete;

  [[nodiscard]] bool IsPrepared() const noexcept { return PreparedValue; }
  [[nodiscard]] std::string_view Blocker() const noexcept {
    return BlockerValue;
  }

  [[nodiscard]] Luna::BindingRegistry Bindings() noexcept {
    return Owner.Bindings();
  }

  [[nodiscard]] bool Run(const std::string &Script) {
    const Luna::ExecutionResult Result = Owner.Execute(Script);
    if (Result.IsSuccess())
      return true;
    std::cerr << "luna-benchmark execution-failure reason=\""
              << DiagnosticText(Result) << "\"\n";
    return false;
  }

private:
  Luna::State Owner;
  bool PreparedValue = false;
  std::string BlockerValue;
};

} // namespace LunaBenchmark
