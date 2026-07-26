#pragma once

// Property and field declarations as canonical plan entries.
//
// One member of a registered class contributes exactly one staged thing: the
// member symbol itself, described with the same `DescriptorPlanEntry` schema
// every other category uses. A property is one symbol rather than one callable
// candidate per direction, because a consumer and a generator read one member
// with a readability, a writability, and an evaluation policy - not two
// unrelated functions that happen to share a name. The two generated getter and
// setter descriptors travel with the entry as the member's private payload and
// become reachable only once the transaction publishes.
//
// Member names collide in exactly one deterministic order, and the order is the
// whole rule:
//
//   1. a Luna-reserved metamethod or system name;
//   2. a same-category duplicate of a member already declared;
//   3. an incompatible-category collision with a declaration of another kind;
//   4. an inherited-name ambiguity.
//
// The fourth arm is implemented and unreachable today: a class has no base
// edges until `Base` and `Cast` exist, so no name can be inherited from two
// places yet. Its position in the order is fixed here so the later milestone
// adds a graph, not a precedence.

// clang-format off
#include <luna/binding/class_member.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/reflection/storage.hpp"
#include "state/registration/plan.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

// The prefix Luna reserves for its own metamethods and system names on a class.
// A member declared with it is a reserved-name collision, never a replacement
// of Luna's own behavior.
inline constexpr std::string_view ReservedMemberPrefix = "__";

// True when one member segment names something Luna owns rather than something
// the consumer may declare.
[[nodiscard]] bool IsReservedMemberName(std::string_view Segment) noexcept;

// One staged property or field of a staged class. The two generated descriptors
// are ordinary copyable Luna-owned operations, so a builder plan stays readable
// as an immutable value while it is submitted.
struct StagedMember final {
  std::string Segment;
  std::string QualifiedName;

  SymbolKind Kind = SymbolKind::Property;
  MemberAccess Access = MemberAccess::ReadOnly;
  PropertyEvaluation Evaluation = PropertyEvaluation::Immediate;
  MemberOwnership Ownership = MemberOwnership::Copied;

  // The canonical declared value type of the member and the canonical receiver
  // type it is reached through.
  TypeDescriptor ValueType;
  TypeDescriptor ReceiverType;

  // The declared getter reaches the object through a mutable receiver, so a
  // const view refuses even the read.
  bool ReadRequiresMutableReceiver = false;

  MemberReadOperation Read;
  MemberWriteOperation Write;

  // The first deterministic refusal the declaration itself recorded, such as a
  // policy that contradicts the accessors it was given.
  std::string Refusal;

  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;

  [[nodiscard]] bool HasReader() const noexcept { return Read != nullptr; }
  [[nodiscard]] bool HasWriter() const noexcept { return Write != nullptr; }
};

// The staged member one name resolves to, or null when none is declared.
[[nodiscard]] StagedMember *
FindStagedClassMember(std::vector<StagedMember> &Members,
                      std::string_view Segment);

// One member declaration as a plan entry: the member symbol parented at the
// class symbol, the reflection record that describes its receiver, value type,
// readability, writability, evaluation, attributes, and documentation, and the
// generated descriptors publication records with the registered class.
[[nodiscard]] DescriptorPlanEntry
MakeMemberPlanEntry(const StagedMember &Declaration,
                    const TypeDescriptor &OwnerType,
                    const SymbolId &ClassSymbol);

// Validates one staged member and reports the first deterministic failure: a
// refusal the declaration recorded, a value type Luna cannot carry across the
// member boundary, a direction with no descriptor behind it, or an evaluation
// policy nothing could produce a value for.
[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateStagedMember(const StagedMember &Declaration);

// Why one member name is refused. The enumerator order is exactly the order the
// checks run in, so two refusals can be compared to prove which one is earlier
// without repeating the order.
enum class MemberCollision : std::uint8_t {
  None,
  ReservedSystemName,
  SameCategory,
  IncompatibleCategory,
  InheritedAmbiguity
};

[[nodiscard]] std::string_view
MemberCollisionText(MemberCollision Collision) noexcept;

// What one member-name decision knows.
struct MemberCollisionRequest final {
  std::string_view Segment;
  std::string_view QualifiedName;
  SymbolKind Kind = SymbolKind::Property;

  // An existing declaration of this class already owns the name.
  bool NameIsDeclared = false;
  SymbolKind ExistingKind = SymbolKind::Property;
  PlanEntryKind ExistingCategory = PlanEntryKind::ClassMember;
  bool ExistingIsPending = false;

  // The name is reachable from more than one base of this class. No class has a
  // base edge until `Base` and `Cast` exist, so this is always false today; its
  // position in the order is what this milestone fixes.
  bool InheritedNameIsAmbiguous = false;
};

// The first deterministic collision of one member name, in the documented
// order.
[[nodiscard]] MemberCollision
ClassifyMemberCollision(const MemberCollisionRequest &Request) noexcept;

// The diagnostic of the first deterministic collision, or nothing when the name
// is available.
[[nodiscard]] std::optional<ErrorDiagnostic>
DiagnoseMemberCollision(const MemberCollisionRequest &Request);

} // namespace Luna::Detail
