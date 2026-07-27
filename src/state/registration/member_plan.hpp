#pragma once

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

inline constexpr std::string_view ReservedMemberPrefix = "__";

[[nodiscard]] bool IsReservedMemberName(std::string_view Segment) noexcept;

struct StagedMember final {
  std::string Segment;
  std::string QualifiedName;

  SymbolKind Kind = SymbolKind::Property;
  MemberAccess Access = MemberAccess::ReadOnly;
  PropertyEvaluation Evaluation = PropertyEvaluation::Immediate;
  MemberOwnership Ownership = MemberOwnership::Copied;

  TypeDescriptor ValueType;
  TypeDescriptor ReceiverType;

  bool ReadRequiresMutableReceiver = false;

  MemberReadOperation Read;
  MemberWriteOperation Write;

  std::string Refusal;

  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;

  [[nodiscard]] bool HasReader() const noexcept { return Read != nullptr; }
  [[nodiscard]] bool HasWriter() const noexcept { return Write != nullptr; }
};

[[nodiscard]] StagedMember *
FindStagedClassMember(std::vector<StagedMember> &Members,
                      std::string_view Segment);

[[nodiscard]] DescriptorPlanEntry
MakeMemberPlanEntry(const StagedMember &Declaration,
                    const TypeDescriptor &OwnerType,
                    const SymbolId &ClassSymbol);

[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateStagedMember(const StagedMember &Declaration);

enum class MemberCollision : std::uint8_t {
  None,
  ReservedSystemName,
  SameCategory,
  IncompatibleCategory,
  InheritedAmbiguity
};

[[nodiscard]] std::string_view
MemberCollisionText(MemberCollision Collision) noexcept;

struct MemberCollisionRequest final {
  std::string_view Segment;
  std::string_view QualifiedName;
  SymbolKind Kind = SymbolKind::Property;

  bool NameIsDeclared = false;
  SymbolKind ExistingKind = SymbolKind::Property;
  PlanEntryKind ExistingCategory = PlanEntryKind::ClassMember;
  bool ExistingIsPending = false;

  bool InheritedNameIsAmbiguous = false;
};

[[nodiscard]] MemberCollision
ClassifyMemberCollision(const MemberCollisionRequest &Request) noexcept;

[[nodiscard]] std::optional<ErrorDiagnostic>
DiagnoseMemberCollision(const MemberCollisionRequest &Request);

} // namespace Luna::Detail
