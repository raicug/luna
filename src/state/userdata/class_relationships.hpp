#pragma once

// The explicit class relationship graph of one logical State.
//
// Nodes are registered classes, base edges are the derived-to-base adjustments
// their declarations captured, and cast edges are the safe downcast policies a
// base-to-derived access is permitted through. Registration validates the graph
// before it is ever recorded here, so every ordered pair of nodes is connected
// by at most one accessible base path and the precomputed closure below holds
// exactly that path.
//
// Nothing here derives identity from a runtime type name, a runtime type
// address, or a native object address: a node is one canonical `TypeId` plus
// the metatable identity that class owns in this State, and a cast policy that
// uses runtime type assistance still identifies itself by its declared policy
// name.

// clang-format off
#include <luna/binding/class_relationship.hpp>
#include <luna/reflection/ids.hpp>

#include "state/userdata/identity.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

struct ClassRelationshipNode final {
  TypeId Type;
  MetatableId Metatable;
  std::string QualifiedName;
};

struct ClassBaseEdge final {
  TypeId Derived;
  TypeId Base;
  ClassPointerAdjustment Upcast = nullptr;
};

struct ClassCastEdge final {
  TypeId Source;
  TypeId Target;
  std::string Policy;
  bool UsesRuntimeTypeAssistance = false;
  ClassCompatibilityProbe Compatible = nullptr;
  ClassPointerAdjustment Downcast = nullptr;
};

// The one accessible base path between two nodes, as the ordered adjustments
// that reach the target from the source.
struct ClassUpcastPath final {
  TypeId Source;
  TypeId Target;
  std::vector<ClassPointerAdjustment> Adjustments;
};

// How one object of a registered class reaches a requested view of it.
enum class ClassConversionKind : std::uint8_t {
  Identity,
  Upcast,
  SafeDowncast,
  Unrelated
};

[[nodiscard]] std::string_view
ClassConversionKindText(ClassConversionKind Kind) noexcept;

struct ClassConversion final {
  ClassConversionKind Kind = ClassConversionKind::Unrelated;
  const ClassUpcastPath *Path = nullptr;
  const ClassCastEdge *Cast = nullptr;

  [[nodiscard]] bool IsViable() const noexcept {
    return Kind != ClassConversionKind::Unrelated;
  }

  [[nodiscard]] bool RequiresCompatibilityCheck() const noexcept {
    return Kind == ClassConversionKind::SafeDowncast;
  }
};

class ClassRelationships final {
public:
  // Publication is the only writer. Recording the same node twice refreshes its
  // metatable identity, which is what a later compatible replacement needs.
  void RecordNode(const TypeId &Type, const MetatableId &Metatable,
                  std::string QualifiedName);
  void RecordBase(ClassBaseEdge Edge);
  void RecordCast(ClassCastEdge Edge);

  // Recomputes the unique accessible base path of every ordered pair. It must
  // run after the last edge of one publication is recorded and before any
  // access resolves through the graph.
  void Rebuild();

  [[nodiscard]] const std::vector<ClassBaseEdge> &BaseEdges() const noexcept {
    return Bases;
  }

  [[nodiscard]] const std::vector<ClassCastEdge> &CastEdges() const noexcept {
    return Casts;
  }

  [[nodiscard]] bool IsEmpty() const noexcept { return Nodes.empty(); }
  [[nodiscard]] std::size_t NodeCount() const noexcept { return Nodes.size(); }
  [[nodiscard]] std::size_t BaseCount() const noexcept { return Bases.size(); }
  [[nodiscard]] std::size_t CastCount() const noexcept { return Casts.size(); }
  [[nodiscard]] std::size_t PathCount() const noexcept { return Paths.size(); }

  [[nodiscard]] bool Contains(const TypeId &Type) const noexcept;
  [[nodiscard]] MetatableId MetatableOf(const TypeId &Type) const noexcept;
  [[nodiscard]] std::string_view NameOf(const TypeId &Type) const noexcept;

  // How an object whose dynamic class is `Source` reaches the requested view
  // `Target`.
  [[nodiscard]] ClassConversion Resolve(const TypeId &Source,
                                        const TypeId &Target) const noexcept;

  // Every class one class reaches through its accessible base paths, in the
  // order the precomputed closure holds them.
  [[nodiscard]] std::vector<TypeId> AccessibleBases(const TypeId &Source) const;

  [[nodiscard]] bool IsDirectBase(const TypeId &Derived,
                                  const TypeId &Base) const noexcept;

private:
  [[nodiscard]] const ClassRelationshipNode *
  FindNode(const TypeId &Type) const noexcept;

  void Reach(const TypeId &Source, const TypeId &Current,
             std::vector<ClassPointerAdjustment> &Chain,
             std::vector<TypeId> &Visited);

  std::vector<ClassRelationshipNode> Nodes;
  std::vector<ClassBaseEdge> Bases;
  std::vector<ClassCastEdge> Casts;
  std::vector<ClassUpcastPath> Paths;
};

// The read-only pointer one safe downcast accepts, or null when the object is
// not a value of the requested class. An unrelated or identity conversion needs
// no check and reports the storage unchanged.
[[nodiscard]] const void *ProbeClassConversion(const ClassConversion &Resolved,
                                               const void *Storage) noexcept;

// The native pointer the requested view sees, or null when the conversion is
// unavailable or the object is incompatible.
[[nodiscard]] void *ApplyClassConversion(const ClassConversion &Resolved,
                                         void *Storage) noexcept;

} // namespace Luna::Detail
