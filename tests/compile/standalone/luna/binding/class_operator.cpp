// clang-format off
#include <luna/binding/class_operator.hpp>

#include <cstddef>
#include <string_view>
#include <type_traits>
// clang-format on

namespace {

// The operator surface compiles from this header alone: no other Luna header,
// no Luau include path, and nothing but the standard library.
static_assert(std::is_enum_v<Luna::ClassOperator>,
              "One declared operator is an ordinary enumeration value.");

static_assert(Luna::ClassOperatorText(Luna::ClassOperator::Add) == "add",
              "Every supported operator names itself canonically.");
static_assert(Luna::ClassOperatorText(Luna::ClassOperator::LessEqual) ==
                  "less_equal",
              "Ordering operators keep their canonical text.");
static_assert(Luna::ClassOperatorText(Luna::ClassOperator::ToText) == "to_text",
              "String conversion keeps its canonical text.");

static_assert(Luna::ClassOperatorOperandCount(Luna::ClassOperator::Length) == 0,
              "A length operator takes only its receiver.");
static_assert(Luna::ClassOperatorOperandCount(Luna::ClassOperator::Negate) == 0,
              "A unary operator takes only its receiver.");
static_assert(
    Luna::ClassOperatorOperandCount(Luna::ClassOperator::Concatenate) == 1,
    "Concatenation takes one operand beyond its receiver.");
static_assert(Luna::ClassOperatorOperandCount(Luna::ClassOperator::Assign) == 2,
              "Assignment takes a key and a value beyond its receiver.");

static_assert(
    Luna::ClassOperatorForwardsEveryArgument(Luna::ClassOperator::Call),
    "A call operator forwards whatever the call site supplied.");
static_assert(
    !Luna::ClassOperatorForwardsEveryArgument(Luna::ClassOperator::Add),
    "Every other operator has a fixed operand count.");

static_assert(Luna::ClassOperatorProducesValue(Luna::ClassOperator::Equal),
              "An equality operator produces one value.");
static_assert(!Luna::ClassOperatorProducesValue(Luna::ClassOperator::Assign),
              "An assignment operator produces none.");

static_assert(
    Luna::ClassOperatorUsesReservedDispatch(Luna::ClassOperator::Index) &&
        Luna::ClassOperatorUsesReservedDispatch(Luna::ClassOperator::Assign),
    "Indexing and assignment stay behind Luna's own reserved dispatch.");
static_assert(
    !Luna::ClassOperatorUsesReservedDispatch(Luna::ClassOperator::Multiply),
    "Every other operator answers a metamethod of its own.");

[[nodiscard]] bool OperatorFactsHold() {
  std::size_t Operands = 0;
  for (const Luna::ClassOperator Selected :
       {Luna::ClassOperator::Call, Luna::ClassOperator::Length,
        Luna::ClassOperator::Equal, Luna::ClassOperator::Index}) {
    Operands += Luna::ClassOperatorOperandCount(Selected);
    if (Luna::ClassOperatorText(Selected).empty())
      return false;
  }
  return Operands == 2;
}

const bool OperatorsCompile = OperatorFactsHold();

} // namespace

namespace Luna::Detail::StandaloneCompileChecks {

[[nodiscard]] bool ClassOperatorHeaderCompiles() { return OperatorsCompile; }

} // namespace Luna::Detail::StandaloneCompileChecks
