#pragma once

// clang-format off
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/class_operator.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/reflection/ids.hpp>

#include "state/reflection/storage.hpp"
#include "state/userdata/class_operators.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

struct StagedOperator final {
  ClassOperator Selected = ClassOperator::Call;
  std::string Segment;
  std::string QualifiedName;

  bool DeclaresReceiver = true;
  bool ReceiverIsConst = false;

  std::shared_ptr<ErasedCallableDescriptor> Callable;

  std::string Refusal;
  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;

  [[nodiscard]] bool HasTarget() const noexcept {
    return Callable != nullptr && Callable->HasTarget();
  }
};

[[nodiscard]] StagedOperator *
FindStagedOperator(std::vector<StagedOperator> &Operators,
                   ClassOperator Selected) noexcept;

[[nodiscard]] StagedOperator *
FindStagedOperator(std::vector<StagedOperator> &Operators,
                   std::string_view Segment) noexcept;

[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateStagedOperator(const StagedOperator &Declaration);

} // namespace Luna::Detail
