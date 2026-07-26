// clang-format off
#include "state/userdata/class_relationships.hpp"

#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] bool IsVisited(const std::vector<TypeId> &Visited,
                             const TypeId &Type) noexcept {
  for (const TypeId &Seen : Visited) {
    if (Seen == Type)
      return true;
  }
  return false;
}

} // namespace

std::string_view ClassConversionKindText(ClassConversionKind Kind) noexcept {
  switch (Kind) {
  case ClassConversionKind::Identity:
    return "identity";
  case ClassConversionKind::Upcast:
    return "upcast";
  case ClassConversionKind::SafeDowncast:
    return "safe_downcast";
  case ClassConversionKind::Unrelated:
    return "unrelated";
  }
  return "unrelated";
}

void ClassRelationships::RecordNode(const TypeId &Type,
                                    const MetatableId &Metatable,
                                    std::string QualifiedName) {
  if (!Type.IsValid())
    return;
  for (ClassRelationshipNode &Node : Nodes) {
    if (Node.Type == Type) {
      Node.Metatable = Metatable;
      Node.QualifiedName = std::move(QualifiedName);
      return;
    }
  }

  ClassRelationshipNode Node;
  Node.Type = Type;
  Node.Metatable = Metatable;
  Node.QualifiedName = std::move(QualifiedName);
  Nodes.push_back(std::move(Node));
}

void ClassRelationships::RecordBase(ClassBaseEdge Edge) {
  if (!Edge.Derived.IsValid() || !Edge.Base.IsValid() || Edge.Upcast == nullptr)
    return;
  for (const ClassBaseEdge &Existing : Bases) {
    if (Existing.Derived == Edge.Derived && Existing.Base == Edge.Base)
      return;
  }
  Bases.push_back(std::move(Edge));
}

void ClassRelationships::RecordCast(ClassCastEdge Edge) {
  if (!Edge.Source.IsValid() || !Edge.Target.IsValid() ||
      Edge.Compatible == nullptr || Edge.Downcast == nullptr)
    return;
  for (ClassCastEdge &Existing : Casts) {
    if (Existing.Source == Edge.Source && Existing.Target == Edge.Target) {
      Existing = std::move(Edge);
      return;
    }
  }
  Casts.push_back(std::move(Edge));
}

void ClassRelationships::Reach(const TypeId &Source, const TypeId &Current,
                               std::vector<ClassPointerAdjustment> &Chain,
                               std::vector<TypeId> &Visited) {
  for (const ClassBaseEdge &Edge : Bases) {
    if (!(Edge.Derived == Current) || IsVisited(Visited, Edge.Base))
      continue;

    Chain.push_back(Edge.Upcast);
    Visited.push_back(Edge.Base);

    bool Recorded = false;
    for (const ClassUpcastPath &Existing : Paths) {
      if (Existing.Source == Source && Existing.Target == Edge.Base) {
        Recorded = true;
        break;
      }
    }
    if (!Recorded) {
      ClassUpcastPath Path;
      Path.Source = Source;
      Path.Target = Edge.Base;
      Path.Adjustments = Chain;
      Paths.push_back(std::move(Path));
    }

    Reach(Source, Edge.Base, Chain, Visited);
    Visited.pop_back();
    Chain.pop_back();
  }
}

void ClassRelationships::Rebuild() {
  Paths.clear();
  for (const ClassRelationshipNode &Node : Nodes) {
    std::vector<ClassPointerAdjustment> Chain;
    std::vector<TypeId> Visited{Node.Type};
    Reach(Node.Type, Node.Type, Chain, Visited);
  }
}

const ClassRelationshipNode *
ClassRelationships::FindNode(const TypeId &Type) const noexcept {
  for (const ClassRelationshipNode &Node : Nodes) {
    if (Node.Type == Type)
      return &Node;
  }
  return nullptr;
}

bool ClassRelationships::Contains(const TypeId &Type) const noexcept {
  return FindNode(Type) != nullptr;
}

MetatableId ClassRelationships::MetatableOf(const TypeId &Type) const noexcept {
  const ClassRelationshipNode *Node = FindNode(Type);
  return Node ? Node->Metatable : MetatableId();
}

std::string_view ClassRelationships::NameOf(const TypeId &Type) const noexcept {
  const ClassRelationshipNode *Node = FindNode(Type);
  return Node ? std::string_view(Node->QualifiedName) : std::string_view();
}

ClassConversion
ClassRelationships::Resolve(const TypeId &Source,
                            const TypeId &Target) const noexcept {
  ClassConversion Resolved;
  if (!Source.IsValid() || !Target.IsValid())
    return Resolved;
  if (Source == Target) {
    Resolved.Kind = ClassConversionKind::Identity;
    return Resolved;
  }

  for (const ClassUpcastPath &Path : Paths) {
    if (Path.Source == Source && Path.Target == Target) {
      Resolved.Kind = ClassConversionKind::Upcast;
      Resolved.Path = &Path;
      return Resolved;
    }
  }

  for (const ClassCastEdge &Edge : Casts) {
    if (Edge.Source == Source && Edge.Target == Target) {
      Resolved.Kind = ClassConversionKind::SafeDowncast;
      Resolved.Cast = &Edge;
      return Resolved;
    }
  }
  return Resolved;
}

std::vector<TypeId>
ClassRelationships::AccessibleBases(const TypeId &Source) const {
  std::vector<TypeId> Reached;
  if (!Source.IsValid())
    return Reached;
  for (const ClassUpcastPath &Path : Paths) {
    if (Path.Source == Source)
      Reached.push_back(Path.Target);
  }
  return Reached;
}

bool ClassRelationships::IsDirectBase(const TypeId &Derived,
                                      const TypeId &Base) const noexcept {
  for (const ClassBaseEdge &Edge : Bases) {
    if (Edge.Derived == Derived && Edge.Base == Base)
      return true;
  }
  return false;
}

const void *ProbeClassConversion(const ClassConversion &Resolved,
                                 const void *Storage) noexcept {
  if (!Resolved.IsViable() || Storage == nullptr)
    return nullptr;
  if (!Resolved.RequiresCompatibilityCheck())
    return Storage;
  if (Resolved.Cast == nullptr || Resolved.Cast->Compatible == nullptr)
    return nullptr;
  return Resolved.Cast->Compatible(Storage);
}

void *ApplyClassConversion(const ClassConversion &Resolved,
                           void *Storage) noexcept {
  if (!Resolved.IsViable() || Storage == nullptr)
    return nullptr;

  switch (Resolved.Kind) {
  case ClassConversionKind::Identity:
    return Storage;
  case ClassConversionKind::Upcast: {
    if (Resolved.Path == nullptr)
      return nullptr;
    void *Adjusted = Storage;
    for (const ClassPointerAdjustment Step : Resolved.Path->Adjustments) {
      if (Step == nullptr || Adjusted == nullptr)
        return nullptr;
      Adjusted = Step(Adjusted);
    }
    return Adjusted;
  }
  case ClassConversionKind::SafeDowncast:
    if (Resolved.Cast == nullptr || Resolved.Cast->Downcast == nullptr)
      return nullptr;
    return Resolved.Cast->Downcast(Storage);
  case ClassConversionKind::Unrelated:
    break;
  }
  return nullptr;
}

} // namespace Luna::Detail
