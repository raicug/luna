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

// One staged operator of a staged class. It is an ordinary instance member
// candidate published under the Luna-owned segment its operator names, so it
// joins the same overload grouping, the same receiver validation, the same
// conversion registry, and the same transaction every other member candidate
// does.
struct StagedOperator final {
  ClassOperator Selected = ClassOperator::Call;
  std::string Segment;
  std::string QualifiedName;

  bool DeclaresReceiver = true;
  bool ReceiverIsConst = false;

  // The erased candidate is held through a shared owner because a builder plan
  // is read immutably while it is submitted, and the candidate is moved into
  // its canonical plan entry exactly once at that point.
  std::shared_ptr<ErasedCallableDescriptor> Callable;

  std::string Refusal;
  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;

  [[nodiscard]] bool HasTarget() const noexcept {
    return Callable != nullptr && Callable->HasTarget();
  }
};

// The staged operator one selection resolves to, or null when the staged class
// declares none.
[[nodiscard]] StagedOperator *
FindStagedOperator(std::vector<StagedOperator> &Operators,
                   ClassOperator Selected) noexcept;

// The staged operator one Luna-owned member segment resolves to, or null.
[[nodiscard]] StagedOperator *
FindStagedOperator(std::vector<StagedOperator> &Operators,
                   std::string_view Segment) noexcept;

// Validates one staged operator and reports the first deterministic failure: a
// refusal the declaration recorded, a missing target, a receiver that does not
// operate on a value of a class, or a declared operand count no call of that
// operator could supply.
[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateStagedOperator(const StagedOperator &Declaration);

} // namespace Luna::Detail
