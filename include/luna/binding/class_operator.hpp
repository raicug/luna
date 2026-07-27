#pragma once

// clang-format off
#include <cstddef>
#include <string_view>
// clang-format on

namespace Luna {

enum class ClassOperator {
  Call,
  Length,
  Equal,
  Less,
  LessEqual,
  Add,
  Subtract,
  Multiply,
  Divide,
  Modulo,
  Power,
  Negate,
  Concatenate,
  ToText,
  Index,
  Assign
};

[[nodiscard]] constexpr std::string_view
ClassOperatorText(ClassOperator Selected) noexcept {
  switch (Selected) {
  case ClassOperator::Call:
    return "call";
  case ClassOperator::Length:
    return "length";
  case ClassOperator::Equal:
    return "equal";
  case ClassOperator::Less:
    return "less";
  case ClassOperator::LessEqual:
    return "less_equal";
  case ClassOperator::Add:
    return "add";
  case ClassOperator::Subtract:
    return "subtract";
  case ClassOperator::Multiply:
    return "multiply";
  case ClassOperator::Divide:
    return "divide";
  case ClassOperator::Modulo:
    return "modulo";
  case ClassOperator::Power:
    return "power";
  case ClassOperator::Negate:
    return "negate";
  case ClassOperator::Concatenate:
    return "concatenate";
  case ClassOperator::ToText:
    return "to_text";
  case ClassOperator::Index:
    return "index";
  case ClassOperator::Assign:
    return "assign";
  }
  return "call";
}

[[nodiscard]] constexpr std::size_t
ClassOperatorOperandCount(ClassOperator Selected) noexcept {
  switch (Selected) {
  case ClassOperator::Call:
    return 0;
  case ClassOperator::Length:
    return 0;
  case ClassOperator::Negate:
    return 0;
  case ClassOperator::ToText:
    return 0;
  case ClassOperator::Assign:
    return 2;
  case ClassOperator::Equal:
  case ClassOperator::Less:
  case ClassOperator::LessEqual:
  case ClassOperator::Add:
  case ClassOperator::Subtract:
  case ClassOperator::Multiply:
  case ClassOperator::Divide:
  case ClassOperator::Modulo:
  case ClassOperator::Power:
  case ClassOperator::Concatenate:
  case ClassOperator::Index:
    return 1;
  }
  return 1;
}

[[nodiscard]] constexpr bool
ClassOperatorForwardsEveryArgument(ClassOperator Selected) noexcept {
  return Selected == ClassOperator::Call;
}

[[nodiscard]] constexpr bool
ClassOperatorProducesValue(ClassOperator Selected) noexcept {
  return Selected != ClassOperator::Assign;
}

[[nodiscard]] constexpr bool
ClassOperatorUsesReservedDispatch(ClassOperator Selected) noexcept {
  return Selected == ClassOperator::Index || Selected == ClassOperator::Assign;
}

} // namespace Luna
