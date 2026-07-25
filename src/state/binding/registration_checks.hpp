#pragma once

// clang-format off
#include <luna/core/diagnostics/error_diagnostic.hpp>

#include <optional>
#include <string_view>
// clang-format on

namespace Luna::Detail {

[[nodiscard]] std::optional<ErrorDiagnostic>
CheckRegistrationPreconditions(std::string_view GlobalName, bool StateReady,
                               bool HasCallableTarget, bool IsDuplicate);

} // namespace Luna::Detail
