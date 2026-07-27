// clang-format off
#include <luna/binding/class_member.hpp>

#include <string>
#include <string_view>
#include <type_traits>
// clang-format on

namespace {

static_assert(Luna::MemberAccessText(Luna::MemberAccess::ReadOnly) ==
                  std::string_view("read-only"),
              "A member's declared directions have reflected text.");
static_assert(Luna::MemberAccessText(Luna::MemberAccess::WriteOnly) ==
                  std::string_view("write-only"),
              "A write-only member has reflected text.");
static_assert(
    Luna::PropertyEvaluationText(Luna::PropertyEvaluation::Immediate) ==
        std::string_view("immediate"),
    "A property's declared evaluation has reflected text.");
static_assert(
    Luna::PropertyEvaluationText(Luna::PropertyEvaluation::Computed) ==
        std::string_view("computed"),
    "A computed property has reflected text.");
static_assert(Luna::MemberOwnershipText(Luna::MemberOwnership::Shared) ==
                  std::string_view("shared"),
              "A field's declared ownership has reflected text.");
static_assert(Luna::PermitsMemberRead(Luna::MemberAccess::ReadWrite) &&
                  Luna::PermitsMemberWrite(Luna::MemberAccess::ReadWrite),
              "A read-write member permits both directions.");
static_assert(!Luna::PermitsMemberRead(Luna::MemberAccess::WriteOnly),
              "A write-only member permits no read.");

static_assert(std::is_copy_constructible_v<Luna::PropertyPolicy> &&
                  std::is_default_constructible_v<Luna::PropertyPolicy>,
              "A property policy is an ordinary value.");
static_assert(std::is_copy_constructible_v<Luna::FieldPolicy> &&
                  std::is_default_constructible_v<Luna::FieldPolicy>,
              "A field policy is an ordinary value.");
static_assert(std::is_same_v<decltype(Luna::PropertyPolicy::Lazy()),
                             Luna::PropertyPolicy>,
              "Every property policy factory yields the same value type.");
static_assert(std::is_same_v<decltype(Luna::FieldPolicy::Owned(
                                 Luna::MemberOwnership::Copied)),
                             Luna::FieldPolicy>,
              "Every field policy factory yields the same value type.");

static_assert(std::is_same_v<Luna::Detail::MemberReadOutcome,
                             decltype(Luna::Detail::MemberReadOutcome::Accept(
                                 Luna::Value(1)))>,
              "A generated getter produces one Luna-owned outcome.");
static_assert(std::is_same_v<Luna::Detail::MemberWriteOutcome,
                             decltype(Luna::Detail::MemberWriteOutcome::Refuse(
                                 std::string()))>,
              "A generated setter produces one Luna-owned outcome.");

} // namespace
