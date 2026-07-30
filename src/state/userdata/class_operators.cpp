// clang-format off
#include "state/userdata/class_operators.hpp"

#include <luna/binding/class_operator.hpp>

#include <span>
#include <string_view>
// clang-format on

namespace Luna::Detail {
namespace {

constexpr ClassOperatorDescriptor Supported[] = {
    {ClassOperator::Call, "__LunaOperatorCall", "__call", 0, true, true},
    {ClassOperator::Length, "__LunaOperatorLength", "__len", 0, false, true},
    {ClassOperator::Equal, "__LunaOperatorEqual", "__eq", 1, false, true},
    {ClassOperator::Less, "__LunaOperatorLess", "__lt", 1, false, true},
    {ClassOperator::LessEqual, "__LunaOperatorLessEqual", "__le", 1, false,
     true},
    {ClassOperator::Add, "__LunaOperatorAdd", "__add", 1, false, true},
    {ClassOperator::Subtract, "__LunaOperatorSubtract", "__sub", 1, false,
     true},
    {ClassOperator::Multiply, "__LunaOperatorMultiply", "__mul", 1, false,
     true},
    {ClassOperator::Divide, "__LunaOperatorDivide", "__div", 1, false, true},
    {ClassOperator::Modulo, "__LunaOperatorModulo", "__mod", 1, false, true},
    {ClassOperator::Power, "__LunaOperatorPower", "__pow", 1, false, true},
    {ClassOperator::Negate, "__LunaOperatorNegate", "__unm", 0, false, true},
    {ClassOperator::Concatenate, "__LunaOperatorConcatenate", "__concat", 1,
     false, true},
    {ClassOperator::ToText, "__LunaOperatorToText", "__tostring", 0, false,
     true},
    {ClassOperator::Index, "__LunaOperatorIndex", "", 1, false, true},
    {ClassOperator::Assign, "__LunaOperatorAssign", "", 2, false, false},
    {ClassOperator::Iterate, "__LunaOperatorIterate", "__iter", 1, false, true,
     true, true},
};

constexpr ReservedMetamethod Reserved[] = {
    {"__type", ReservedMetamethodRole::Identity},
    {"__metatable", ReservedMetamethodRole::MetatableProtection},
    {"__LunaMetatable", ReservedMetamethodRole::MetatableProtection},
    {"__index", ReservedMetamethodRole::Dispatch},
    {"__newindex", ReservedMetamethodRole::Dispatch},
    {"__namecall", ReservedMetamethodRole::Dispatch},
    {"__mode", ReservedMetamethodRole::Lifetime},
    {"__LunaLifetime", ReservedMetamethodRole::Lifetime},
    {"__gc", ReservedMetamethodRole::Collection},
    {"__LunaCollect", ReservedMetamethodRole::Collection},
};

} // namespace

std::span<const ClassOperatorDescriptor> ClassOperatorDescriptors() noexcept {
  return std::span<const ClassOperatorDescriptor>(Supported);
}

const ClassOperatorDescriptor *
FindClassOperator(ClassOperator Selected) noexcept {
  for (const ClassOperatorDescriptor &Described : Supported) {
    if (Described.Selected == Selected)
      return &Described;
  }
  return nullptr;
}

std::string_view
ReservedMetamethodRoleText(ReservedMetamethodRole Role) noexcept {
  switch (Role) {
  case ReservedMetamethodRole::Identity:
    return "type identity";
  case ReservedMetamethodRole::Dispatch:
    return "member dispatch";
  case ReservedMetamethodRole::MetatableProtection:
    return "metatable protection";
  case ReservedMetamethodRole::Lifetime:
    return "lifetime";
  case ReservedMetamethodRole::Collection:
    return "garbage collection";
  }
  return "member dispatch";
}

std::span<const ReservedMetamethod> ReservedMetamethods() noexcept {
  return std::span<const ReservedMetamethod>(Reserved);
}

const ReservedMetamethod *
FindReservedMetamethod(std::string_view Name) noexcept {
  for (const ReservedMetamethod &Owned : Reserved) {
    if (Owned.Name == Name)
      return &Owned;
  }
  return nullptr;
}

} // namespace Luna::Detail
