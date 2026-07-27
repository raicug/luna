#pragma once

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

struct ClassUpcastPath final {
  TypeId Source;
  TypeId Target;
  std::vector<ClassPointerAdjustment> Adjustments;
};

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
  void RecordNode(const TypeId &Type, const MetatableId &Metatable,
                  std::string QualifiedName);
  void RecordBase(ClassBaseEdge Edge);
  void RecordCast(ClassCastEdge Edge);

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

  [[nodiscard]] ClassConversion Resolve(const TypeId &Source,
                                        const TypeId &Target) const noexcept;

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

[[nodiscard]] const void *ProbeClassConversion(const ClassConversion &Resolved,
                                               const void *Storage) noexcept;

[[nodiscard]] void *ApplyClassConversion(const ClassConversion &Resolved,
                                         void *Storage) noexcept;

} // namespace Luna::Detail
