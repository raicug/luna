// clang-format off
#include "state/registration/relationship_plan.hpp"

#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/registration/checks.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] std::string ClassSubjectOf(std::string_view QualifiedName) {
  return SubjectText(SymbolKindText(SymbolKind::Class), QualifiedName);
}

[[nodiscard]] bool IsVisited(const std::vector<TypeId> &Visited,
                             const TypeId &Type) {
  for (const TypeId &Seen : Visited) {
    if (Seen == Type)
      return true;
  }
  return false;
}

[[nodiscard]] std::string KeyTextOf(const StableTypeKey &Key) {
  return Key.Text().empty() ? std::string("<unnamed>") : Key.Text();
}

// The first deterministic refusal, plus what it needs to be worded.
struct RelationshipRefusal final {
  RelationshipFailure Failure = RelationshipFailure::None;
  std::string Subject;
  std::string Named;
};

[[nodiscard]] RelationshipRefusal
Refuse(RelationshipFailure Failure, std::string Subject, std::string Named) {
  RelationshipRefusal Refusal;
  Refusal.Failure = Failure;
  Refusal.Subject = std::move(Subject);
  Refusal.Named = std::move(Named);
  return Refusal;
}

[[nodiscard]] RelationshipRefusal
ScanBases(const RelationshipCandidate &Candidate) {
  for (const RelationshipBase &Edge : Candidate.Bases()) {
    if (!Edge.DeclaresBase)
      return Refuse(RelationshipFailure::UndeclaredBase, Edge.DerivedName,
                    KeyTextOf(Edge.Base));
  }
  for (const RelationshipBase &Edge : Candidate.Bases()) {
    if (!Edge.IsAccessible)
      return Refuse(RelationshipFailure::InaccessibleBase, Edge.DerivedName,
                    KeyTextOf(Edge.Base));
  }
  for (const RelationshipBase &Edge : Candidate.Bases()) {
    const RelationshipClass *Base = Candidate.Find(Edge.Base);
    if (Base == nullptr || !Base->Type.IsValid() || !Edge.HasAdjustment)
      return Refuse(RelationshipFailure::UnavailableBase, Edge.DerivedName,
                    KeyTextOf(Edge.Base));
  }

  const std::vector<RelationshipBase> &Edges = Candidate.Bases();
  for (std::size_t Index = 0; Index < Edges.size(); ++Index) {
    const RelationshipClass *Base = Candidate.Find(Edges[Index].Base);
    for (std::size_t Earlier = 0; Earlier < Index; ++Earlier) {
      const RelationshipClass *Other = Candidate.Find(Edges[Earlier].Base);
      if (Edges[Earlier].Derived == Edges[Index].Derived && Base != nullptr &&
          Other != nullptr && Other->Type == Base->Type)
        return Refuse(RelationshipFailure::DuplicateBase,
                      Edges[Index].DerivedName, KeyTextOf(Edges[Index].Base));
    }
  }

  for (const RelationshipBase &Edge : Edges) {
    const RelationshipClass *Base = Candidate.Find(Edge.Base);
    if (Base == nullptr)
      continue;
    if (Base->Type == Edge.Derived ||
        Candidate.PathCount(Base->Type, Edge.Derived) > 0)
      return Refuse(RelationshipFailure::CyclicBase, Edge.DerivedName,
                    KeyTextOf(Edge.Base));
  }

  for (const RelationshipClass &Source : Candidate.Classes()) {
    for (const RelationshipClass &Target : Candidate.Classes()) {
      if (Source.Type == Target.Type)
        continue;
      if (Candidate.PathCount(Source.Type, Target.Type) > 1)
        return Refuse(RelationshipFailure::AmbiguousBasePath,
                      Source.QualifiedName, Target.QualifiedName);
    }
  }
  return RelationshipRefusal();
}

[[nodiscard]] RelationshipRefusal
ScanCasts(const RelationshipCandidate &Candidate) {
  const std::vector<RelationshipCast> &Edges = Candidate.Casts();
  for (const RelationshipCast &Edge : Edges) {
    if (!Edge.DeclaresBase || !Edge.IsAccessible)
      return Refuse(RelationshipFailure::UndeclaredCastSource, Edge.TargetName,
                    KeyTextOf(Edge.Source));
  }
  for (const RelationshipCast &Edge : Edges) {
    const RelationshipClass *Source = Candidate.Find(Edge.Source);
    if (Source == nullptr || !Source->Type.IsValid() ||
        Candidate.PathCount(Edge.Target, Source->Type) != 1)
      return Refuse(RelationshipFailure::UnavailableCastSource, Edge.TargetName,
                    KeyTextOf(Edge.Source));
  }

  for (std::size_t Index = 0; Index < Edges.size(); ++Index) {
    const RelationshipClass *Source = Candidate.Find(Edges[Index].Source);
    for (std::size_t Earlier = 0; Earlier < Index; ++Earlier) {
      const RelationshipClass *Other = Candidate.Find(Edges[Earlier].Source);
      if (Edges[Earlier].Target == Edges[Index].Target && Source != nullptr &&
          Other != nullptr && Other->Type == Source->Type)
        return Refuse(RelationshipFailure::DuplicateCast,
                      Edges[Index].TargetName, KeyTextOf(Edges[Index].Source));
    }
  }

  for (const RelationshipCast &Edge : Edges) {
    if (!Edge.HasPolicy)
      return Refuse(RelationshipFailure::UnsafeCastPolicy, Edge.TargetName,
                    KeyTextOf(Edge.Source));
  }
  return RelationshipRefusal();
}

[[nodiscard]] RelationshipRefusal
ScanCandidate(const RelationshipCandidate &Candidate) {
  const RelationshipRefusal Refused = ScanBases(Candidate);
  if (Refused.Failure != RelationshipFailure::None)
    return Refused;
  return ScanCasts(Candidate);
}

[[nodiscard]] std::string ReasonOf(const RelationshipRefusal &Refusal) {
  switch (Refusal.Failure) {
  case RelationshipFailure::UndeclaredBase:
    return "'" + Refusal.Named +
           "' is not a base of this class, so no base edge could ever adjust a "
           "value of it.";
  case RelationshipFailure::InaccessibleBase:
    return "the base '" + Refusal.Named +
           "' is not reachable through one unambiguous public path of this "
           "class.";
  case RelationshipFailure::UnavailableBase:
    return "the base '" + Refusal.Named +
           "' is not a registered class of this State.";
  case RelationshipFailure::DuplicateBase:
    return "the base '" + Refusal.Named +
           "' is already declared by this class; one base edge is declared "
           "exactly once.";
  case RelationshipFailure::CyclicBase:
    return "the base '" + Refusal.Named +
           "' already reaches this class, so the base edge would close a "
           "cycle.";
  case RelationshipFailure::AmbiguousBasePath:
    return "more than one accessible base path leads from this class to '" +
           Refusal.Named + "', so no single conversion path owns the pair.";
  case RelationshipFailure::UndeclaredCastSource:
    return "'" + Refusal.Named +
           "' is not an accessible base of this class, so no downcast to this "
           "class could start at it.";
  case RelationshipFailure::UnavailableCastSource:
    return "the cast source '" + Refusal.Named +
           "' is not connected to this class by exactly one registered "
           "accessible base path.";
  case RelationshipFailure::DuplicateCast:
    return "a downcast from '" + Refusal.Named +
           "' is already declared by this class; one cast policy is declared "
           "exactly once.";
  case RelationshipFailure::UnsafeCastPolicy:
    return "the downcast from '" + Refusal.Named +
           "' names no identified non-mutating safe cast policy.";
  case RelationshipFailure::None:
    break;
  }
  return "the declared class relationship is unavailable.";
}

} // namespace

std::string_view RelationshipFailureText(RelationshipFailure Failure) noexcept {
  switch (Failure) {
  case RelationshipFailure::None:
    return "none";
  case RelationshipFailure::UndeclaredBase:
    return "undeclared_base";
  case RelationshipFailure::InaccessibleBase:
    return "inaccessible_base";
  case RelationshipFailure::UnavailableBase:
    return "unavailable_base";
  case RelationshipFailure::DuplicateBase:
    return "duplicate_base";
  case RelationshipFailure::CyclicBase:
    return "cyclic_base";
  case RelationshipFailure::AmbiguousBasePath:
    return "ambiguous_base_path";
  case RelationshipFailure::UndeclaredCastSource:
    return "undeclared_cast_source";
  case RelationshipFailure::UnavailableCastSource:
    return "unavailable_cast_source";
  case RelationshipFailure::DuplicateCast:
    return "duplicate_cast";
  case RelationshipFailure::UnsafeCastPolicy:
    return "unsafe_cast_policy";
  }
  return "none";
}

TypeId ClassTypeIdentityOf(const StableTypeKey &Key) {
  if (const auto Identity =
          TypeIdentityRegistry::ComputeIdentity(TypeDescriptor::ForClass(Key)))
    return *Identity;
  return TypeId();
}

bool RelationshipClass::Declares(std::string_view Segment) const noexcept {
  for (const std::string &Declared : MemberNames) {
    if (Declared == Segment)
      return true;
  }
  return false;
}

RelationshipBase MakeCandidateBase(const TypeId &Derived,
                                   std::string DerivedName,
                                   const BaseRequest &Declared) {
  RelationshipBase Edge;
  Edge.Derived = Derived;
  Edge.DerivedName = std::move(DerivedName);
  Edge.Base = Declared.Base;
  Edge.DeclaresBase = Declared.DeclaresBase;
  Edge.IsAccessible = Declared.IsAccessible;
  Edge.HasAdjustment = Declared.Upcast != nullptr;
  return Edge;
}

RelationshipCast MakeCandidateCast(const TypeId &Target, std::string TargetName,
                                   const CastRequest &Declared) {
  RelationshipCast Edge;
  Edge.Target = Target;
  Edge.TargetName = std::move(TargetName);
  Edge.Source = Declared.Source;
  Edge.DeclaresBase = Declared.DeclaresBase;
  Edge.IsAccessible = Declared.IsAccessible;
  Edge.HasPolicy = !Declared.Policy.empty() && Declared.Compatible != nullptr &&
                   Declared.Downcast != nullptr;
  return Edge;
}

void RelationshipCandidate::AddClass(RelationshipClass Described) {
  for (RelationshipClass &Existing : Declared) {
    if (Existing.Type == Described.Type) {
      Existing.IsPending = Existing.IsPending || Described.IsPending;
      for (std::string &Member : Described.MemberNames) {
        if (!Existing.Declares(Member))
          Existing.MemberNames.push_back(std::move(Member));
      }
      return;
    }
  }
  Declared.push_back(std::move(Described));
}

void RelationshipCandidate::AddBase(RelationshipBase Edge) {
  BaseEdges.push_back(std::move(Edge));
}

void RelationshipCandidate::AddCast(RelationshipCast Edge) {
  CastEdges.push_back(std::move(Edge));
}

const RelationshipClass *
RelationshipCandidate::Find(const StableTypeKey &Key) const noexcept {
  for (const RelationshipClass &Existing : Declared) {
    if (Existing.Key == Key)
      return &Existing;
  }
  return nullptr;
}

const RelationshipClass *
RelationshipCandidate::Find(const TypeId &Type) const noexcept {
  for (const RelationshipClass &Existing : Declared) {
    if (Existing.Type == Type)
      return &Existing;
  }
  return nullptr;
}

void RelationshipCandidate::Walk(const TypeId &Current, const TypeId &Target,
                                 std::vector<TypeId> &Visited,
                                 std::size_t &Found) const {
  for (const RelationshipBase &Edge : BaseEdges) {
    if (!(Edge.Derived == Current))
      continue;
    const RelationshipClass *Base = Find(Edge.Base);
    if (Base == nullptr || IsVisited(Visited, Base->Type))
      continue;
    if (Base->Type == Target) {
      ++Found;
      continue;
    }
    Visited.push_back(Base->Type);
    Walk(Base->Type, Target, Visited, Found);
    Visited.pop_back();
  }
}

std::size_t RelationshipCandidate::PathCount(const TypeId &Source,
                                             const TypeId &Target) const {
  if (!Source.IsValid() || !Target.IsValid() || Source == Target)
    return 0;
  std::vector<TypeId> Visited{Source};
  std::size_t Found = 0;
  Walk(Source, Target, Visited, Found);
  return Found;
}

std::vector<TypeId>
RelationshipCandidate::ReachableBases(const TypeId &Type) const {
  std::vector<TypeId> Reached;
  if (!Type.IsValid())
    return Reached;

  std::vector<TypeId> Pending{Type};
  while (!Pending.empty()) {
    const TypeId Current = Pending.back();
    Pending.pop_back();
    for (const RelationshipBase &Edge : BaseEdges) {
      if (!(Edge.Derived == Current))
        continue;
      const RelationshipClass *Base = Find(Edge.Base);
      if (Base == nullptr || Base->Type == Type ||
          IsVisited(Reached, Base->Type))
        continue;
      Reached.push_back(Base->Type);
      Pending.push_back(Base->Type);
    }
  }
  return Reached;
}

std::size_t RelationshipCandidate::InheritedDeclarationCount(
    const TypeId &Derived, std::string_view Segment) const {
  std::size_t Owners = 0;
  for (const TypeId &Base : ReachableBases(Derived)) {
    const RelationshipClass *Described = Find(Base);
    if (Described != nullptr && Described->Declares(Segment))
      ++Owners;
  }
  return Owners;
}

RelationshipFailure
ClassifyRelationshipCandidate(const RelationshipCandidate &Candidate) {
  return ScanCandidate(Candidate).Failure;
}

std::optional<ErrorDiagnostic>
ValidateRelationshipCandidate(const RelationshipCandidate &Candidate) {
  const RelationshipRefusal Refusal = ScanCandidate(Candidate);
  if (Refusal.Failure == RelationshipFailure::None)
    return std::nullopt;
  return MalformedMetadataDiagnostic(ClassSubjectOf(Refusal.Subject),
                                     ReasonOf(Refusal));
}

} // namespace Luna::Detail
