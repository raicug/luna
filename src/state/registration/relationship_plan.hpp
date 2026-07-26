#pragma once

// The candidate class relationship graph of one registration attempt, and the
// deterministic order it is refused in.
//
// A relationship is only ever accepted as part of a whole graph: committed
// classes and their published edges plus every class and edge the pending plan
// declares. That is what makes duplicate edges, cycles, and a pair of classes
// reachable through more than one base path decidable before anything is
// published, and it is why declaration order inside one plan never changes the
// outcome.

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

// The relationships one class declares, as its plan entry carries them.
struct PlannedClassRelationships final {
  std::vector<BaseRequest> Bases;
  std::vector<CastRequest> Casts;

  [[nodiscard]] bool IsEmpty() const noexcept {
    return Bases.empty() && Casts.empty();
  }
};

// The canonical type identity one registered class owns, derived from its
// stable key alone: no registration order, no runtime type name, and no address
// participates.
[[nodiscard]] TypeId ClassTypeIdentityOf(const StableTypeKey &Key);

// One class of the candidate graph.
struct RelationshipClass final {
  TypeId Type;
  StableTypeKey Key;
  std::string QualifiedName;
  bool IsPending = false;

  // The member names this class declares itself, which is what makes a name
  // inherited from more than one base decidable.
  std::vector<std::string> MemberNames;

  [[nodiscard]] bool Declares(std::string_view Segment) const noexcept;
};

// One base edge of the candidate graph: the class that declares it, the class
// it names, and the declared C++ facts the declaration captured. A published
// edge joins the candidate with exactly the same shape.
struct RelationshipBase final {
  TypeId Derived;
  std::string DerivedName;
  StableTypeKey Base;
  bool DeclaresBase = true;
  bool IsAccessible = true;
  bool HasAdjustment = true;
};

// One safe downcast of the candidate graph: the class it targets, the class it
// starts at, and whether the declaration named an identified non-mutating
// policy.
struct RelationshipCast final {
  TypeId Target;
  std::string TargetName;
  StableTypeKey Source;
  bool DeclaresBase = true;
  bool IsAccessible = true;
  bool HasPolicy = true;
};

// One declared base edge as a candidate edge of the class that declared it.
[[nodiscard]] RelationshipBase MakeCandidateBase(const TypeId &Derived,
                                                 std::string DerivedName,
                                                 const BaseRequest &Declared);

// One declared safe downcast as a candidate edge of the class it targets.
[[nodiscard]] RelationshipCast MakeCandidateCast(const TypeId &Target,
                                                 std::string TargetName,
                                                 const CastRequest &Declared);

// Why one candidate graph is refused. The enumerator order is exactly the order
// the checks run in.
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

  // The accessible base classes of one class, in canonical declaration order of
  // the graph, including the classes reached through them.
  [[nodiscard]] std::vector<TypeId> ReachableBases(const TypeId &Type) const;

  // How many accessible bases of one class declare one member name themselves.
  // More than one is the inherited ambiguity of that name.
  [[nodiscard]] std::size_t
  InheritedDeclarationCount(const TypeId &Derived,
                            std::string_view Segment) const;

  // How many accessible base paths lead from `Source` to `Target`. Registration
  // accepts a pair only while this is at most one.
  [[nodiscard]] std::size_t PathCount(const TypeId &Source,
                                      const TypeId &Target) const;

private:
  void Walk(const TypeId &Current, const TypeId &Target,
            std::vector<TypeId> &Visited, std::size_t &Found) const;

  std::vector<RelationshipClass> Declared;
  std::vector<RelationshipBase> BaseEdges;
  std::vector<RelationshipCast> CastEdges;
};

// The first deterministic refusal of one candidate graph, in the documented
// order.
[[nodiscard]] RelationshipFailure
ClassifyRelationshipCandidate(const RelationshipCandidate &Candidate);

// The diagnostic of the first deterministic refusal, or nothing when the whole
// graph is acceptable.
[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateRelationshipCandidate(const RelationshipCandidate &Candidate);

} // namespace Luna::Detail
