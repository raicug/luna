#pragma once

// clang-format off
#include <luna/binding/class_operator.hpp>
#include <luna/reflection/ids.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
// clang-format on

namespace Luna::Detail {

struct ClassOperatorDescriptor final {
  ClassOperator Selected = ClassOperator::Call;
  std::string_view Segment;
  std::string_view Metamethod;
  std::size_t OperandCount = 0;
  bool ForwardsEveryArgument = false;
  bool ProducesValue = true;
};

[[nodiscard]] std::span<const ClassOperatorDescriptor>
ClassOperatorDescriptors() noexcept;

[[nodiscard]] const ClassOperatorDescriptor *
FindClassOperator(ClassOperator Selected) noexcept;

enum class ReservedMetamethodRole : std::uint8_t {
  Identity,
  Dispatch,
  MetatableProtection,
  Lifetime,
  Collection
};

[[nodiscard]] std::string_view
ReservedMetamethodRoleText(ReservedMetamethodRole Role) noexcept;

struct ReservedMetamethod final {
  std::string_view Name;
  ReservedMetamethodRole Role = ReservedMetamethodRole::Dispatch;
};

[[nodiscard]] std::span<const ReservedMetamethod>
ReservedMetamethods() noexcept;

[[nodiscard]] const ReservedMetamethod *
FindReservedMetamethod(std::string_view Name) noexcept;

struct RegisteredOperator final {
  ClassOperator Selected = ClassOperator::Call;
  std::string Segment;
  std::string QualifiedName;
  SymbolId Symbol;
};

} // namespace Luna::Detail
