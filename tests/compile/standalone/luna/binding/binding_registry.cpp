// clang-format off
#include <luna/binding/binding_registry.hpp>

#include <type_traits>
#include <utility>
// clang-format on

namespace {

// The freeze-facing public API is complete in this header alone: freezing a
// State and querying it afterwards need no other Luna header, no Luau
// declaration, and no include path beyond Luna's own.
static_assert(
    std::is_same_v<decltype(std::declval<Luna::BindingRegistry &>().Freeze()),
                   Luna::RegistrationResult>,
    "BindingRegistry::Freeze must keep returning RegistrationResult.");
static_assert(
    std::is_same_v<
        decltype(std::declval<const Luna::BindingRegistry &>().Reflection()),
        Luna::ReflectionSnapshot>,
    "A frozen State must keep answering reflection with an owning snapshot.");

} // namespace
