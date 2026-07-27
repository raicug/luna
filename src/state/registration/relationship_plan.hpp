#pragma once

// clang-format off
#include <luna/binding/class_relationship.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/stable_type_key.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

struct PlannedClassRelationships final {
  std::vector<BaseRequest> Bases;
  std::vector<CastRequest> Casts;

  [[nodiscard]] bool IsEmpty() const noexcept {
    return Bases.empty() && Casts.empty();
  }
};

[[nodiscard]] TypeId ClassTypeIdentityOf(const StableTypeKey &Key);

struct RelationshipClass final {
  TypeId Type;
  StableTypeKey Key;
  std::string QualifiedName;
  bool IsPending = false;

  std::vector<std::string> MemberNames;

  [[nodiscard]] bool Declares(std::string_view Segment) const noexcept;
};

struct RelationshipBase final {
  TypeId Derived;
  std::string DerivedName;
  StableTypeKey Base;
  bool DeclaresBase = true;
  bool IsAccessible = true;
  bool HasAdjustment = true;
};

struct RelationshipCast final {
  TypeId Target;
  std::string TargetName;
  StableTypeKey Source;
  bool DeclaresBase = true;
  bool IsAccessible = true;
  bool HasPolicy = true;
};

[[nodiscard]] RelationshipBase MakeCandidateBase(const TypeId &Derived,
                                                 std::string DerivedName,
                                                 const BaseRequest &Declared);

[[nodiscard]] RelationshipCast MakeCandidateCast(const TypeId &Target,
                                                 std::string TargetName,
                                                 const CastRequest &Declared);

enum class RelationshipFailure : std::uint8_t {
  None,
  UndeclaredBase,
  InaccessibleBase,
  UnavailableBase,
  DuplicateBase,
  CyclicBase,
  AmbiguousBasePath,
  UndeclaredCastSource,
  UnavailableCastSource,
  DuplicateCast,
  UnsafeCastPolicy
};

[[nodiscard]] std::string_view
RelationshipFailureText(RelationshipFailure Failure) noexcept;

class RelationshipCandidate final {
public:
  void AddClass(RelationshipClass Declared);
  void AddBase(RelationshipBase Edge);
  void AddCast(RelationshipCast Edge);

  [[nodiscard]] const RelationshipClass *
  Find(const StableTypeKey &Key) const noexcept;
  [[nodiscard]] const RelationshipClass *
  Find(const TypeId &Type) const noexcept;

  [[nodiscard]] const std::vector<RelationshipClass> &Classes() const noexcept {
    return Declared;
  }
  [[nodiscard]] const std::vector<RelationshipBase> &Bases() const noexcept {
    return BaseEdges;
  }
  [[nodiscard]] const std::vector<RelationshipCast> &Casts() const noexcept {
    return CastEdges;
  }

  [[nodiscard]] std::vector<TypeId> ReachableBases(const TypeId &Type) const;

  [[nodiscard]] std::size_t
  InheritedDeclarationCount(const TypeId &Derived,
                            std::string_view Segment) const;

  [[nodiscard]] std::size_t PathCount(const TypeId &Source,
                                      const TypeId &Target) const;

private:
  void Walk(const TypeId &Current, const TypeId &Target,
            std::vector<TypeId> &Visited, std::size_t &Found) const;

  std::vector<RelationshipClass> Declared;
  std::vector<RelationshipBase> BaseEdges;
  std::vector<RelationshipCast> CastEdges;
};

[[nodiscard]] RelationshipFailure
ClassifyRelationshipCandidate(const RelationshipCandidate &Candidate);

[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateRelationshipCandidate(const RelationshipCandidate &Candidate);

} // namespace Luna::Detail
