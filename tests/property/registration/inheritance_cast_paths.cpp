// Property 29: inheritance and casts agree with unique accessible paths.
//
// Two halves are generated together, and both are compared with an independent
// path-counting model written here rather than with Luna's own accounting.
//
// The first half generates whole candidate relationship graphs as values,
// because every question registration asks about a relationship is decidable
// without a virtual machine at all: how many nodes there are, which of them are
// registered classes, which declared edges really are base edges, which are
// publicly reachable, which carry a pointer adjustment, which are declared
// twice, which close a cycle, which pairs are connected by two paths, and which
// safe downcasts mirror one registered accessible path. The model counts simple
// paths over the generated adjacency itself and predicts the earliest refusal
// in the documented order, the number of accessible paths of every ordered
// pair, the reachable base set of every class, and how many bases declare one
// inherited name.
//
// Every graph the model accepts is then published, and the published graph is
// compared with the same model again: the conversion kind of every ordered
// pair, the composed pointer adjustment of every unique accessible path, and
// the outcome of every safe downcast against a generated dynamic type. Whether
// one object is a value of the target class is carried by the object itself,
// never by where it happens to be stored, so recycled storage can never inherit
// another object's dynamic type. A rejected compatibility check is required to
// perform no committing conversion and to reach no native target at all.
//
// The second half takes a generated slice through real registration, so what
// the graph decides is observable exactly as a consumer sees it: the acceptance
// of a whole plan, the one contextual diagnostic of each refusal family, the
// adjusted pointer each base view of a derived object receives, the safe
// downcast that delivers a compatible object and refuses an incompatible one
// before native code, the ordinary receiver validation and overload ranking a
// base member reached with a derived receiver still goes through, and the exact
// stack depth every outcome restores.

// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_relationship.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/binding/overload.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/core/results/execution_result.hpp>
#include <luna/core/results/registration_result.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/registration/relationship_plan.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/userdata/class_relationships.hpp"
#include "state/userdata/identity.hpp"

#include <rapidcheck.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::Detail::ClassConversion;
using Luna::Detail::ClassConversionKind;
using Luna::Detail::ClassRelationships;
using Luna::Detail::ConstAccess;
using Luna::Detail::OwnershipModel;
using Luna::Detail::RelationshipCandidate;
using Luna::Detail::RelationshipFailure;

// Deterministic byte source. Equal bytes always drive the equal graph, so a
// shrunk counterexample replays exactly the same relationships.
class ByteCursor final {
public:
  explicit ByteCursor(const std::vector<std::uint8_t> &Bytes) noexcept
      : BytesValue(&Bytes) {}

  [[nodiscard]] std::uint8_t Next() noexcept {
    const std::size_t Index = IndexValue++;
    if (BytesValue->empty())
      return static_cast<std::uint8_t>(Index * 37U + 11U);
    return (*BytesValue)[Index % BytesValue->size()];
  }

  [[nodiscard]] std::size_t Pick(std::size_t Count) noexcept {
    return Count == 0 ? 0 : static_cast<std::size_t>(Next()) % Count;
  }

private:
  const std::vector<std::uint8_t> *BytesValue;
  std::size_t IndexValue = 0;
};

// ---------------------------------------------------------------------------
// The generated graph, described entirely by node indices so nothing about it
// depends on Luna's own identities.
// ---------------------------------------------------------------------------

constexpr std::size_t NodePoolSize = 6;
constexpr std::size_t AdjustmentCount = 4;
constexpr std::size_t MemberNamePoolSize = 2;

[[nodiscard]] std::string NodeSuffix(std::size_t Index) {
  static const char *const Suffixes[NodePoolSize] = {"A", "B", "C",
                                                     "D", "E", "F"};
  return std::string(Suffixes[Index % NodePoolSize]);
}

[[nodiscard]] Luna::StableTypeKey NodeKey(std::size_t Index) {
  return Luna::StableTypeKey("Studio.PathNode" + NodeSuffix(Index));
}

[[nodiscard]] std::string NodeName(std::size_t Index) {
  return "Studio.Node" + NodeSuffix(Index);
}

[[nodiscard]] std::string MemberName(std::size_t Index) {
  static const char *const Names[MemberNamePoolSize] = {"Alpha", "Beta"};
  return std::string(Names[Index % MemberNamePoolSize]);
}

// One generated base edge: the class that declares it, the class it names, and
// the declared C++ facts the declaration would have captured.
struct GeneratedBase final {
  std::size_t Derived = 0;
  std::size_t Base = 0;
  bool DeclaresBase = true;
  bool IsAccessible = true;
  bool HasAdjustment = true;
  std::size_t Adjustment = 0;
};

// One generated safe downcast: the class it targets, the class it starts at,
// and whether the declaration named an identified non-mutating policy.
struct GeneratedCast final {
  std::size_t Target = 0;
  std::size_t Source = 0;
  bool DeclaresBase = true;
  bool IsAccessible = true;
  bool HasPolicy = true;
};

struct ReferenceGraph final {
  std::size_t NodeCount = 0;
  std::array<bool, NodePoolSize> Registered{};
  std::vector<GeneratedBase> Bases;
  std::vector<GeneratedCast> Casts;
  std::array<std::array<bool, MemberNamePoolSize>, NodePoolSize> Members{};
};

[[nodiscard]] bool Holds(const std::vector<std::size_t> &Values,
                         std::size_t Value) {
  for (const std::size_t Held : Values) {
    if (Held == Value)
      return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// The independent path-counting model.
// ---------------------------------------------------------------------------

// Every simple path of registered classes that leads from one class to another,
// counted over the generated adjacency alone. A declared edge that names no
// registered class connects nothing, and a class is never revisited, so a cycle
// contributes no path of its own.
void CountPaths(const ReferenceGraph &Graph, std::size_t Current,
                std::size_t Target, std::vector<std::size_t> &Visited,
                std::size_t &Found) {
  for (const GeneratedBase &Edge : Graph.Bases) {
    if (Edge.Derived != Current || !Graph.Registered[Edge.Base] ||
        Holds(Visited, Edge.Base))
      continue;
    if (Edge.Base == Target) {
      ++Found;
      continue;
    }
    Visited.push_back(Edge.Base);
    CountPaths(Graph, Edge.Base, Target, Visited, Found);
    Visited.pop_back();
  }
}

[[nodiscard]] std::size_t ReferencePathCount(const ReferenceGraph &Graph,
                                             std::size_t Source,
                                             std::size_t Target) {
  if (Source == Target || !Graph.Registered[Source] ||
      !Graph.Registered[Target])
    return 0;
  std::vector<std::size_t> Visited{Source};
  std::size_t Found = 0;
  CountPaths(Graph, Source, Target, Visited, Found);
  return Found;
}

// The ordered pointer adjustments of the one path a pair is connected by.
[[nodiscard]] bool FindPathOffsets(const ReferenceGraph &Graph,
                                   std::size_t Current, std::size_t Target,
                                   std::vector<std::size_t> &Visited,
                                   std::vector<std::size_t> &Offsets) {
  for (const GeneratedBase &Edge : Graph.Bases) {
    if (Edge.Derived != Current || !Graph.Registered[Edge.Base] ||
        Holds(Visited, Edge.Base))
      continue;
    Offsets.push_back(Edge.Adjustment);
    if (Edge.Base == Target)
      return true;
    Visited.push_back(Edge.Base);
    if (FindPathOffsets(Graph, Edge.Base, Target, Visited, Offsets))
      return true;
    Visited.pop_back();
    Offsets.pop_back();
  }
  return false;
}

// The base classes one class reaches, as a set.
[[nodiscard]] std::vector<std::size_t>
ReferenceReachableBases(const ReferenceGraph &Graph, std::size_t Source) {
  std::vector<std::size_t> Reached;
  std::vector<std::size_t> Pending{Source};
  while (!Pending.empty()) {
    const std::size_t Current = Pending.back();
    Pending.pop_back();
    for (const GeneratedBase &Edge : Graph.Bases) {
      if (Edge.Derived != Current || !Graph.Registered[Edge.Base] ||
          Edge.Base == Source || Holds(Reached, Edge.Base))
        continue;
      Reached.push_back(Edge.Base);
      Pending.push_back(Edge.Base);
    }
  }
  return Reached;
}

[[nodiscard]] std::size_t ReferenceInheritedCount(const ReferenceGraph &Graph,
                                                  std::size_t Derived,
                                                  std::size_t Name) {
  std::size_t Owners = 0;
  for (const std::size_t Base : ReferenceReachableBases(Graph, Derived)) {
    if (Graph.Members[Base][Name])
      ++Owners;
  }
  return Owners;
}

// Why one generated graph is refused. The order of the enumerators is the order
// the model asks its questions in, which is the order the requirements state.
enum class ReferenceFailure : std::uint8_t {
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
ReferenceFailureText(ReferenceFailure Failure) noexcept {
  switch (Failure) {
  case ReferenceFailure::None:
    return "accepted";
  case ReferenceFailure::UndeclaredBase:
    return "undeclared_base";
  case ReferenceFailure::InaccessibleBase:
    return "inaccessible_base";
  case ReferenceFailure::UnavailableBase:
    return "unavailable_base";
  case ReferenceFailure::DuplicateBase:
    return "duplicate_base";
  case ReferenceFailure::CyclicBase:
    return "cyclic_base";
  case ReferenceFailure::AmbiguousBasePath:
    return "ambiguous_base_path";
  case ReferenceFailure::UndeclaredCastSource:
    return "undeclared_cast_source";
  case ReferenceFailure::UnavailableCastSource:
    return "unavailable_cast_source";
  case ReferenceFailure::DuplicateCast:
    return "duplicate_cast";
  case ReferenceFailure::UnsafeCastPolicy:
    break;
  }
  return "unsafe_cast_policy";
}

// The failure Luna reports, named in the model's own vocabulary, so the two are
// compared as outcomes rather than as one enumeration cast to the other.
[[nodiscard]] std::string_view NamedFailure(RelationshipFailure Failure) {
  return Luna::Detail::RelationshipFailureText(Failure);
}

[[nodiscard]] std::string_view NamedFailure(ReferenceFailure Failure) {
  return Failure == ReferenceFailure::None ? std::string_view("none")
                                           : ReferenceFailureText(Failure);
}

[[nodiscard]] ReferenceFailure ClassifyReference(const ReferenceGraph &Graph) {
  for (const GeneratedBase &Edge : Graph.Bases) {
    if (!Edge.DeclaresBase)
      return ReferenceFailure::UndeclaredBase;
  }
  for (const GeneratedBase &Edge : Graph.Bases) {
    if (!Edge.IsAccessible)
      return ReferenceFailure::InaccessibleBase;
  }
  for (const GeneratedBase &Edge : Graph.Bases) {
    if (!Graph.Registered[Edge.Base] || !Edge.HasAdjustment)
      return ReferenceFailure::UnavailableBase;
  }
  for (std::size_t Index = 0; Index < Graph.Bases.size(); ++Index) {
    for (std::size_t Earlier = 0; Earlier < Index; ++Earlier) {
      if (Graph.Bases[Earlier].Derived == Graph.Bases[Index].Derived &&
          Graph.Bases[Earlier].Base == Graph.Bases[Index].Base)
        return ReferenceFailure::DuplicateBase;
    }
  }
  for (const GeneratedBase &Edge : Graph.Bases) {
    if (Edge.Base == Edge.Derived ||
        ReferencePathCount(Graph, Edge.Base, Edge.Derived) > 0)
      return ReferenceFailure::CyclicBase;
  }
  for (std::size_t Source = 0; Source < NodePoolSize; ++Source) {
    for (std::size_t Target = 0; Target < NodePoolSize; ++Target) {
      if (Source == Target || !Graph.Registered[Source] ||
          !Graph.Registered[Target])
        continue;
      if (ReferencePathCount(Graph, Source, Target) > 1)
        return ReferenceFailure::AmbiguousBasePath;
    }
  }

  for (const GeneratedCast &Edge : Graph.Casts) {
    if (!Edge.DeclaresBase || !Edge.IsAccessible)
      return ReferenceFailure::UndeclaredCastSource;
  }
  for (const GeneratedCast &Edge : Graph.Casts) {
    if (!Graph.Registered[Edge.Source] ||
        ReferencePathCount(Graph, Edge.Target, Edge.Source) != 1)
      return ReferenceFailure::UnavailableCastSource;
  }
  for (std::size_t Index = 0; Index < Graph.Casts.size(); ++Index) {
    for (std::size_t Earlier = 0; Earlier < Index; ++Earlier) {
      if (Graph.Casts[Earlier].Target == Graph.Casts[Index].Target &&
          Graph.Casts[Earlier].Source == Graph.Casts[Index].Source)
        return ReferenceFailure::DuplicateCast;
    }
  }
  for (const GeneratedCast &Edge : Graph.Casts) {
    if (!Edge.HasPolicy)
      return ReferenceFailure::UnsafeCastPolicy;
  }
  return ReferenceFailure::None;
}


// ---------------------------------------------------------------------------
// The published graph, and the objects its conversions are applied to.
//
// A node of a generated graph has no C++ type of its own, so one base edge is
// represented by a distinct pure pointer adjustment and one safe downcast by a
// distinct non-mutating compatibility check. Nothing is ever dereferenced
// through an adjusted pointer: the model composes the same offsets itself and
// the two results are compared.
// ---------------------------------------------------------------------------

template <std::size_t Offset> [[nodiscard]] void *AdjustForward(void *Object) {
  return static_cast<void *>(static_cast<std::byte *>(Object) + Offset);
}

constexpr std::array<std::size_t, AdjustmentCount> AdjustmentOffsets{8, 16, 24,
                                                                     32};

[[nodiscard]] Luna::Detail::ClassPointerAdjustment
AdjustmentFor(std::size_t Index) {
  switch (Index % AdjustmentCount) {
  case 0:
    return &AdjustForward<8>;
  case 1:
    return &AdjustForward<16>;
  case 2:
    return &AdjustForward<24>;
  default:
    break;
  }
  return &AdjustForward<32>;
}

// One object of a generated graph. Its dynamic class is carried by the object
// itself, so no compatibility decision is ever keyed by storage address and
// recycled storage never inherits another object's dynamic type.
struct alignas(8) Subject final {
  std::uint32_t DynamicTag = 0;
  std::uint32_t Reserved = 0;
  std::byte Bytes[256]{};
};

std::size_t CompatibilityChecks = 0;
std::size_t CommittedDowncasts = 0;
std::size_t NativeTargetCalls = 0;

void ResetConversionCounters() {
  CompatibilityChecks = 0;
  CommittedDowncasts = 0;
  NativeTargetCalls = 0;
}

[[nodiscard]] std::size_t DowncastOffsetOf(std::size_t Target) {
  return 8U * (Target + 1U);
}

[[nodiscard]] std::uint32_t ReadDynamicTag(const void *Object) {
  std::uint32_t Tag = 0;
  std::memcpy(&Tag, Object, sizeof(Tag));
  return Tag;
}

template <std::uint32_t Target>
[[nodiscard]] const void *ProbeSubject(const void *Object) {
  ++CompatibilityChecks;
  if (ReadDynamicTag(Object) != Target)
    return nullptr;
  return static_cast<const void *>(static_cast<const std::byte *>(Object) +
                                   DowncastOffsetOf(Target));
}

template <std::uint32_t Target>
[[nodiscard]] void *DowncastSubject(void *Object) {
  ++CommittedDowncasts;
  if (ReadDynamicTag(Object) != Target)
    return nullptr;
  return static_cast<void *>(static_cast<std::byte *>(Object) +
                             DowncastOffsetOf(Target));
}

[[nodiscard]] Luna::Detail::ClassCompatibilityProbe
ProbeFor(std::size_t Target) {
  switch (Target % NodePoolSize) {
  case 0:
    return &ProbeSubject<0>;
  case 1:
    return &ProbeSubject<1>;
  case 2:
    return &ProbeSubject<2>;
  case 3:
    return &ProbeSubject<3>;
  case 4:
    return &ProbeSubject<4>;
  default:
    break;
  }
  return &ProbeSubject<5>;
}

[[nodiscard]] Luna::Detail::ClassPointerAdjustment
DowncastFor(std::size_t Target) {
  switch (Target % NodePoolSize) {
  case 0:
    return &DowncastSubject<0>;
  case 1:
    return &DowncastSubject<1>;
  case 2:
    return &DowncastSubject<2>;
  case 3:
    return &DowncastSubject<3>;
  case 4:
    return &DowncastSubject<4>;
  default:
    break;
  }
  return &DowncastSubject<5>;
}

// ---------------------------------------------------------------------------
// Building the candidate graph Luna validates, from the generated description.
// ---------------------------------------------------------------------------

[[nodiscard]] std::array<Luna::TypeId, NodePoolSize> NodeIdentities() {
  std::array<Luna::TypeId, NodePoolSize> Types;
  for (std::size_t Index = 0; Index < NodePoolSize; ++Index)
    Types[Index] = Luna::Detail::ClassTypeIdentityOf(NodeKey(Index));
  return Types;
}

[[nodiscard]] RelationshipCandidate
BuildCandidate(const ReferenceGraph &Graph,
               const std::array<Luna::TypeId, NodePoolSize> &Types,
               bool ReversedClassOrder) {
  RelationshipCandidate Candidate;
  for (std::size_t Step = 0; Step < NodePoolSize; ++Step) {
    const std::size_t Index =
        ReversedClassOrder ? NodePoolSize - 1 - Step : Step;
    if (!Graph.Registered[Index])
      continue;
    Luna::Detail::RelationshipClass Described;
    Described.Type = Types[Index];
    Described.Key = NodeKey(Index);
    Described.QualifiedName = NodeName(Index);
    Described.IsPending = true;
    for (std::size_t Name = 0; Name < MemberNamePoolSize; ++Name) {
      if (Graph.Members[Index][Name])
        Described.MemberNames.push_back(MemberName(Name));
    }
    Candidate.AddClass(std::move(Described));
  }

  for (const GeneratedBase &Edge : Graph.Bases) {
    Luna::Detail::RelationshipBase Declared;
    Declared.Derived = Types[Edge.Derived];
    Declared.DerivedName = NodeName(Edge.Derived);
    Declared.Base = NodeKey(Edge.Base);
    Declared.DeclaresBase = Edge.DeclaresBase;
    Declared.IsAccessible = Edge.IsAccessible;
    Declared.HasAdjustment = Edge.HasAdjustment;
    Candidate.AddBase(std::move(Declared));
  }

  for (const GeneratedCast &Edge : Graph.Casts) {
    Luna::Detail::RelationshipCast Declared;
    Declared.Target = Types[Edge.Target];
    Declared.TargetName = NodeName(Edge.Target);
    Declared.Source = NodeKey(Edge.Source);
    Declared.DeclaresBase = Edge.DeclaresBase;
    Declared.IsAccessible = Edge.IsAccessible;
    Declared.HasPolicy = Edge.HasPolicy;
    Candidate.AddCast(std::move(Declared));
  }
  return Candidate;
}

// ---------------------------------------------------------------------------
// The one gate every conversion of a generated graph is taken through: the
// non-mutating check first, the committing adjustment only after it accepted,
// and the native target only after that.
// ---------------------------------------------------------------------------

struct GateOutcome final {
  bool Accepted = false;
  void *Delivered = nullptr;
};

// Pointer identity, compared as a plain answer so no address is ever printed
// into a counterexample.
[[nodiscard]] bool Delivers(const void *Delivered, const void *Expected) {
  return Delivered != nullptr && Delivered == Expected;
}

[[nodiscard]] bool DeliversNothing(const void *Delivered) {
  return Delivered == nullptr;
}

[[nodiscard]] GateOutcome RunConversionGate(const ClassConversion &Resolved,
                                            Subject &Value) {
  const std::size_t CommitsBefore = CommittedDowncasts;
  const std::size_t CallsBefore = NativeTargetCalls;

  GateOutcome Outcome;
  const void *const Checked =
      Luna::Detail::ProbeClassConversion(Resolved, &Value);
  if (Checked == nullptr) {
    // A refused object never reaches a committing conversion, and never reaches
    // a native target at all.
    RC_ASSERT(CommittedDowncasts == CommitsBefore);
    RC_ASSERT(NativeTargetCalls == CallsBefore);
    return Outcome;
  }

  Outcome.Delivered = Luna::Detail::ApplyClassConversion(Resolved, &Value);
  RC_ASSERT(!DeliversNothing(Outcome.Delivered));
  // A checked conversion commits exactly the view its check accepted; a
  // conversion that needs no check reports the storage it was offered.
  if (Resolved.RequiresCompatibilityCheck())
    RC_ASSERT(Delivers(Outcome.Delivered, Checked));
  else
    RC_ASSERT(Delivers(Checked, &Value));
  ++NativeTargetCalls;
  Outcome.Accepted = true;
  return Outcome;
}


[[nodiscard]] ReferenceGraph GenerateGraph(ByteCursor &Cursor) {
  ReferenceGraph Graph;
  Graph.NodeCount = 2 + Cursor.Pick(NodePoolSize - 1);
  for (std::size_t Index = 0; Index < Graph.NodeCount; ++Index)
    Graph.Registered[Index] = true;

  // One node of the pool is sometimes left unregistered, so an edge that names
  // it names a class this State never registered at all.
  if (Graph.NodeCount >= 3 && Cursor.Pick(6) == 0)
    Graph.Registered[Cursor.Pick(Graph.NodeCount - 1)] = false;

  std::vector<std::size_t> Derivable;
  for (std::size_t Index = 1; Index < Graph.NodeCount; ++Index) {
    if (Graph.Registered[Index])
      Derivable.push_back(Index);
  }

  // Edges normally lead from a higher node to a lower one, which keeps most
  // graphs acyclic and lets diamonds appear on their own; the remaining shapes
  // are an edge onto the class itself and an edge that points back down.
  const std::size_t EdgeCount = Derivable.empty() ? 0 : Cursor.Pick(7);
  for (std::size_t Step = 0; Step < EdgeCount; ++Step) {
    if (!Graph.Bases.empty() && Cursor.Pick(14) == 0) {
      Graph.Bases.push_back(Graph.Bases[Cursor.Pick(Graph.Bases.size())]);
      continue;
    }

    GeneratedBase Edge;
    Edge.Derived = Derivable[Cursor.Pick(Derivable.size())];
    Edge.Base = Cursor.Pick(Edge.Derived);
    const std::size_t Shape = Cursor.Pick(16);
    if (Shape == 0)
      Edge.Base = Edge.Derived;
    else if (Shape == 1 && Graph.Registered[Edge.Base])
      std::swap(Edge.Derived, Edge.Base);
    Edge.DeclaresBase = Cursor.Pick(16) != 0;
    Edge.IsAccessible = Cursor.Pick(16) != 0;
    Edge.HasAdjustment = Cursor.Pick(18) != 0;
    Edge.Adjustment = Cursor.Pick(AdjustmentCount);

    // A duplicate edge is only ever the deliberate one above, so the graphs
    // that reach the accepted-and-published half stay plentiful.
    bool Repeated = false;
    for (const GeneratedBase &Existing : Graph.Bases) {
      if (Existing.Derived == Edge.Derived && Existing.Base == Edge.Base)
        Repeated = true;
    }
    if (!Repeated)
      Graph.Bases.push_back(Edge);
  }

  const std::size_t CastCount = Cursor.Pick(3);
  for (std::size_t Step = 0; Step < CastCount; ++Step) {
    if (!Graph.Casts.empty() && Cursor.Pick(10) == 0) {
      Graph.Casts.push_back(Graph.Casts[Cursor.Pick(Graph.Casts.size())]);
      continue;
    }

    GeneratedCast Edge;
    if (!Graph.Bases.empty() && Cursor.Pick(4) != 0) {
      // A downcast that mirrors one declared base edge, which is the only shape
      // registration is able to accept.
      const GeneratedBase &Mirrored =
          Graph.Bases[Cursor.Pick(Graph.Bases.size())];
      Edge.Target = Mirrored.Derived;
      Edge.Source = Mirrored.Base;
    } else {
      Edge.Target =
          Derivable.empty() ? 0 : Derivable[Cursor.Pick(Derivable.size())];
      Edge.Source = Cursor.Pick(Graph.NodeCount);
    }
    Edge.DeclaresBase = Cursor.Pick(16) != 0;
    Edge.IsAccessible = Cursor.Pick(16) != 0;
    Edge.HasPolicy = Cursor.Pick(10) != 0;

    bool Repeated = false;
    for (const GeneratedCast &Existing : Graph.Casts) {
      if (Existing.Target == Edge.Target && Existing.Source == Edge.Source)
        Repeated = true;
    }
    if (!Repeated)
      Graph.Casts.push_back(Edge);
  }

  for (std::size_t Index = 0; Index < NodePoolSize; ++Index) {
    if (!Graph.Registered[Index])
      continue;
    for (std::size_t Name = 0; Name < MemberNamePoolSize; ++Name)
      Graph.Members[Index][Name] = Cursor.Pick(3) == 0;
  }
  return Graph;
}

// ---------------------------------------------------------------------------
// Every accepted graph, published and resolved.
// ---------------------------------------------------------------------------

void VerifyPublishedConversions(
    const ReferenceGraph &Graph,
    const std::array<Luna::TypeId, NodePoolSize> &Types, ByteCursor &Cursor) {
  ResetConversionCounters();

  ClassRelationships Published;
  std::size_t Nodes = 0;
  for (std::size_t Index = 0; Index < NodePoolSize; ++Index) {
    if (!Graph.Registered[Index])
      continue;
    Published.RecordNode(Types[Index],
                         Luna::Detail::MetatableId::FromValue(Index + 1),
                         NodeName(Index));
    ++Nodes;
  }
  for (const GeneratedBase &Edge : Graph.Bases) {
    Luna::Detail::ClassBaseEdge Recorded;
    Recorded.Derived = Types[Edge.Derived];
    Recorded.Base = Types[Edge.Base];
    Recorded.Upcast = AdjustmentFor(Edge.Adjustment);
    Published.RecordBase(Recorded);
  }
  for (const GeneratedCast &Edge : Graph.Casts) {
    Luna::Detail::ClassCastEdge Recorded;
    Recorded.Source = Types[Edge.Source];
    Recorded.Target = Types[Edge.Target];
    Recorded.Policy = std::string(Luna::Detail::RuntimeTypeCastPolicyName);
    Recorded.UsesRuntimeTypeAssistance = true;
    Recorded.Compatible = ProbeFor(Edge.Target);
    Recorded.Downcast = DowncastFor(Edge.Target);
    Published.RecordCast(Recorded);
  }
  Published.Rebuild();

  RC_ASSERT(Published.NodeCount() == Nodes);
  RC_ASSERT(Published.BaseCount() == Graph.Bases.size());
  RC_ASSERT(Published.CastCount() == Graph.Casts.size());

  std::size_t Connected = 0;
  for (std::size_t Source = 0; Source < NodePoolSize; ++Source) {
    for (std::size_t Target = 0; Target < NodePoolSize; ++Target) {
      if (ReferencePathCount(Graph, Source, Target) == 1)
        ++Connected;
    }
  }
  RC_ASSERT(Published.PathCount() == Connected);

  for (std::size_t Index = 0; Index < NodePoolSize; ++Index) {
    if (!Graph.Registered[Index]) {
      RC_ASSERT(!Published.Contains(Types[Index]));
      continue;
    }
    RC_ASSERT(Published.Contains(Types[Index]));
    RC_ASSERT(Published.MetatableOf(Types[Index]) ==
              Luna::Detail::MetatableId::FromValue(Index + 1));
    RC_ASSERT(Published.NameOf(Types[Index]) == NodeName(Index));
  }

  for (std::size_t Source = 0; Source < NodePoolSize; ++Source) {
    if (!Graph.Registered[Source])
      continue;
    for (std::size_t Target = 0; Target < NodePoolSize; ++Target) {
      if (!Graph.Registered[Target])
        continue;

      const ClassConversion Resolved =
          Published.Resolve(Types[Source], Types[Target]);

      // The dynamic class of this object, carried by the object itself. One
      // generated value names no registered class at all.
      Subject Value;
      Value.DynamicTag =
          static_cast<std::uint32_t>(Cursor.Pick(NodePoolSize + 1));
      std::byte *const Storage = reinterpret_cast<std::byte *>(&Value);
      const std::size_t ChecksBefore = CompatibilityChecks;

      if (Source == Target) {
        RC_ASSERT(Resolved.Kind == ClassConversionKind::Identity);
        RC_ASSERT(!Resolved.RequiresCompatibilityCheck());
        const GateOutcome Outcome = RunConversionGate(Resolved, Value);
        RC_ASSERT(Outcome.Accepted);
        RC_ASSERT(Delivers(Outcome.Delivered, Storage));
        RC_ASSERT(CompatibilityChecks == ChecksBefore);
        continue;
      }

      const std::size_t Paths = ReferencePathCount(Graph, Source, Target);
      RC_ASSERT(Paths <= 1);
      if (Paths == 1) {
        std::vector<std::size_t> Visited{Source};
        std::vector<std::size_t> Offsets;
        RC_ASSERT(FindPathOffsets(Graph, Source, Target, Visited, Offsets));
        std::size_t Total = 0;
        for (const std::size_t Index : Offsets)
          Total += AdjustmentOffsets[Index % AdjustmentCount];

        RC_ASSERT(Resolved.Kind == ClassConversionKind::Upcast);
        RC_ASSERT(!Resolved.RequiresCompatibilityCheck());
        const GateOutcome Outcome = RunConversionGate(Resolved, Value);
        RC_ASSERT(Outcome.Accepted);
        RC_ASSERT(Delivers(Outcome.Delivered, Storage + Total));
        RC_ASSERT(CompatibilityChecks == ChecksBefore);
        continue;
      }

      bool Declared = false;
      for (const GeneratedCast &Edge : Graph.Casts) {
        if (Edge.Source == Source && Edge.Target == Target)
          Declared = true;
      }

      if (!Declared) {
        // No registered accessible path and no registered policy: the pair is
        // unrelated, so nothing is checked, adjusted, or invoked.
        RC_ASSERT(Resolved.Kind == ClassConversionKind::Unrelated);
        RC_ASSERT(!Resolved.IsViable());
        const GateOutcome Outcome = RunConversionGate(Resolved, Value);
        RC_ASSERT(!Outcome.Accepted);
        RC_ASSERT(DeliversNothing(Outcome.Delivered));
        RC_ASSERT(DeliversNothing(
            Luna::Detail::ApplyClassConversion(Resolved, &Value)));
        RC_ASSERT(CompatibilityChecks == ChecksBefore);
        continue;
      }

      RC_ASSERT(Resolved.Kind == ClassConversionKind::SafeDowncast);
      RC_ASSERT(Resolved.RequiresCompatibilityCheck());
      const bool Compatible =
          Value.DynamicTag == static_cast<std::uint32_t>(Target);

      const GateOutcome First = RunConversionGate(Resolved, Value);
      RC_ASSERT(CompatibilityChecks == ChecksBefore + 1);
      RC_ASSERT(First.Accepted == Compatible);
      if (Compatible)
        RC_ASSERT(
            Delivers(First.Delivered, Storage + DowncastOffsetOf(Target)));
      else
        RC_ASSERT(DeliversNothing(First.Delivered));

      // The same object decides the same way every time it is offered.
      const GateOutcome Second = RunConversionGate(Resolved, Value);
      RC_ASSERT(Second.Accepted == First.Accepted);
      RC_ASSERT(Delivers(Second.Delivered, First.Delivered) ||
                (DeliversNothing(Second.Delivered) &&
                 DeliversNothing(First.Delivered)));
    }
  }
}

// ---------------------------------------------------------------------------
// The generated graph half.
// ---------------------------------------------------------------------------

void VerifyGeneratedRelationshipGraph(ByteCursor &Cursor) {
  const ReferenceGraph Graph = GenerateGraph(Cursor);
  const std::array<Luna::TypeId, NodePoolSize> Types = NodeIdentities();

  // Every node owns one canonical identity of its own, derived from its stable
  // key alone.
  for (std::size_t Index = 0; Index < NodePoolSize; ++Index) {
    RC_ASSERT(Types[Index].IsValid());
    RC_ASSERT(Types[Index] ==
              Luna::Detail::ClassTypeIdentityOf(NodeKey(Index)));
    for (std::size_t Other = 0; Other < Index; ++Other)
      RC_ASSERT(!(Types[Index] == Types[Other]));
  }

  const ReferenceFailure Expected = ClassifyReference(Graph);
  const RelationshipCandidate Candidate = BuildCandidate(Graph, Types, false);
  const RelationshipFailure Reported =
      Luna::Detail::ClassifyRelationshipCandidate(Candidate);
  RC_ASSERT(NamedFailure(Reported) == NamedFailure(Expected));

  const std::optional<Luna::ErrorDiagnostic> Diagnostic =
      Luna::Detail::ValidateRelationshipCandidate(Candidate);
  RC_ASSERT(Diagnostic.has_value() == (Expected != ReferenceFailure::None));
  if (Diagnostic)
    RC_ASSERT(!Diagnostic->Message().empty());

  // Declaration order inside one plan never changes the outcome.
  const RelationshipCandidate Reversed = BuildCandidate(Graph, Types, true);
  RC_ASSERT(Luna::Detail::ClassifyRelationshipCandidate(Reversed) == Reported);

  for (std::size_t Source = 0; Source < NodePoolSize; ++Source) {
    for (std::size_t Target = 0; Target < NodePoolSize; ++Target) {
      RC_ASSERT(Candidate.PathCount(Types[Source], Types[Target]) ==
                ReferencePathCount(Graph, Source, Target));
    }
  }

  for (std::size_t Index = 0; Index < NodePoolSize; ++Index) {
    const std::vector<Luna::TypeId> Reached =
        Candidate.ReachableBases(Types[Index]);
    const std::vector<std::size_t> Modelled =
        ReferenceReachableBases(Graph, Index);
    RC_ASSERT(Reached.size() == Modelled.size());
    for (const std::size_t Base : Modelled) {
      bool Present = false;
      for (const Luna::TypeId &Type : Reached) {
        if (Type == Types[Base])
          Present = true;
      }
      RC_ASSERT(Present);
    }
    RC_ASSERT(Candidate.ReachableBases(Types[Index]) == Reached);

    for (std::size_t Name = 0; Name < MemberNamePoolSize; ++Name) {
      RC_ASSERT(
          Candidate.InheritedDeclarationCount(Types[Index], MemberName(Name)) ==
          ReferenceInheritedCount(Graph, Index, Name));
    }
  }

  if (Expected == ReferenceFailure::None)
    VerifyPublishedConversions(Graph, Types, Cursor);

  RC_TAG(std::string("graph: ") + std::string(ReferenceFailureText(Expected)));
}


// ---------------------------------------------------------------------------
// The real registration slice: one generated scenario per case, taken through
// the public builder and the ordinary access gate.
// ---------------------------------------------------------------------------

struct Vehicle {
  virtual ~Vehicle() = default;
  int Wheels = 4;

  [[nodiscard]] int WheelCount() const { return Wheels; }
  [[nodiscard]] int Describe(int Extra) const { return Wheels * 100 + Extra; }
  [[nodiscard]] int Describe(std::string Text) const {
    return 1000 + static_cast<int>(Text.size());
  }
};

// A second base, so a derived-to-base adjustment of it is a real pointer
// adjustment rather than the identity.
struct Painted {
  int Coats = 2;

  [[nodiscard]] int CoatCount() const { return Coats; }
};

struct Truck final : Vehicle, Painted {
  int Beds = 1;

  [[nodiscard]] int BedCount() const { return Beds; }
};

struct Foreign final {
  int Value = 11;
};

struct Concealed final : private Vehicle {
  int Kept = 5;
};

struct Wing : Vehicle {
  int Span = 1;
};

struct Rotor : Vehicle {
  int Blades = 2;
};

struct Hybrid final : Wing, Rotor {
  int Mix = 3;
};

[[nodiscard]] Luna::StableTypeKey VehicleKey() {
  return Luna::StableTypeKey("Studio.PathVehicle");
}

[[nodiscard]] Luna::StableTypeKey PaintedKey() {
  return Luna::StableTypeKey("Studio.PathPainted");
}

[[nodiscard]] Luna::StableTypeKey TruckKey() {
  return Luna::StableTypeKey("Studio.PathTruck");
}

[[nodiscard]] Luna::StableTypeKey ForeignKey() {
  return Luna::StableTypeKey("Studio.PathForeign");
}

enum class Scenario : std::uint8_t {
  AcceptedWithoutCast,
  AcceptedWithCast,
  UndeclaredBase,
  InaccessibleBase,
  UnavailableBase,
  DuplicateBase,
  AmbiguousBasePath,
  UnavailableCastSource,
  DuplicateCast,
  UnsafeCastPolicy
};

struct ScenarioTraits final {
  const char *Name;
  bool Accepted;
  const char *Phrase;
};

constexpr std::array<ScenarioTraits, 10> ScenarioTable{
    ScenarioTraits{"accepted without a cast policy", true, ""},
    ScenarioTraits{"accepted with a cast policy", true, ""},
    ScenarioTraits{"a class that is not a base at all", false,
                   "is not a base of this class"},
    ScenarioTraits{"a base that is not publicly reachable", false,
                   "not reachable through one unambiguous public path"},
    ScenarioTraits{"a base that is not a registered class", false,
                   "not a registered class"},
    ScenarioTraits{"a duplicate base edge", false,
                   "already declared by this class"},
    ScenarioTraits{"two accessible paths to one base", false,
                   "more than one accessible base path"},
    ScenarioTraits{"a downcast without the base path it mirrors", false,
                   "exactly one registered accessible base path"},
    ScenarioTraits{"a duplicate downcast policy", false,
                   "already declared by this class"},
    ScenarioTraits{"a downcast with no safe policy", false,
                   "no identified non-mutating safe cast policy"}};

[[nodiscard]] Luna::RegistrationResult RegisterScenario(Luna::State &Owner,
                                                        Scenario Which) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");

  if (Which == Scenario::InaccessibleBase) {
    Luna::ClassBuilder<Vehicle> Base =
        Studio.RegisterClass<Vehicle>("Vehicle", VehicleKey());
    static_cast<void>(Base.QualifiedName());
    Luna::ClassBuilder<Concealed> Hidden = Studio.RegisterClass<Concealed>(
        "Concealed", Luna::StableTypeKey("Studio.PathConcealed"));
    static_cast<void>(Hidden.Base<Vehicle>(VehicleKey()).QualifiedName());
    return Studio.Commit();
  }

  if (Which == Scenario::UnavailableBase) {
    Luna::ClassBuilder<Truck> Derived =
        Studio.RegisterClass<Truck>("Truck", TruckKey());
    static_cast<void>(Derived.Base<Vehicle>(VehicleKey()).QualifiedName());
    return Studio.Commit();
  }

  if (Which == Scenario::AmbiguousBasePath) {
    const Luna::StableTypeKey WingKey("Studio.PathWing");
    const Luna::StableTypeKey RotorKey("Studio.PathRotor");
    Luna::ClassBuilder<Vehicle> Base =
        Studio.RegisterClass<Vehicle>("Vehicle", VehicleKey());
    static_cast<void>(Base.QualifiedName());
    Luna::ClassBuilder<Wing> LeftSide =
        Studio.RegisterClass<Wing>("Wing", WingKey);
    static_cast<void>(LeftSide.Base<Vehicle>(VehicleKey()).QualifiedName());
    Luna::ClassBuilder<Rotor> RightSide =
        Studio.RegisterClass<Rotor>("Rotor", RotorKey);
    static_cast<void>(RightSide.Base<Vehicle>(VehicleKey()).QualifiedName());
    Luna::ClassBuilder<Hybrid> Center = Studio.RegisterClass<Hybrid>(
        "Hybrid", Luna::StableTypeKey("Studio.PathHybrid"));
    static_cast<void>(
        Center.Base<Wing>(WingKey).Base<Rotor>(RotorKey).QualifiedName());
    return Studio.Commit();
  }

  Luna::ClassBuilder<Vehicle> Base =
      Studio.RegisterClass<Vehicle>("Vehicle", VehicleKey());
  Luna::ClassBuilder<Vehicle> &WithVehicleMembers =
      Base.Method("WheelCount", &Vehicle::WheelCount)
          .Method("Describe",
                  Luna::Overload<int(int), Vehicle>(&Vehicle::Describe))
          .Method("Describe", Luna::Overload<int(std::string), Vehicle>(
                                  &Vehicle::Describe));
  static_cast<void>(WithVehicleMembers.QualifiedName());

  Luna::ClassBuilder<Painted> Side =
      Studio.RegisterClass<Painted>("Painted", PaintedKey());
  static_cast<void>(
      Side.Method("CoatCount", &Painted::CoatCount).QualifiedName());

  Luna::ClassBuilder<Foreign> Other =
      Studio.RegisterClass<Foreign>("Foreign", ForeignKey());
  static_cast<void>(Other.QualifiedName());

  Luna::ClassBuilder<Truck> Derived =
      Studio.RegisterClass<Truck>("Truck", TruckKey());
  Luna::ClassBuilder<Truck> &WithMembers =
      Derived.Method("BedCount", &Truck::BedCount);

  switch (Which) {
  case Scenario::AcceptedWithoutCast:
    static_cast<void>(WithMembers.Base<Vehicle>(VehicleKey())
                          .Base<Painted>(PaintedKey())
                          .QualifiedName());
    break;
  case Scenario::AcceptedWithCast:
    static_cast<void>(WithMembers.Base<Vehicle>(VehicleKey())
                          .Base<Painted>(PaintedKey())
                          .Cast<Vehicle>(VehicleKey())
                          .QualifiedName());
    break;
  case Scenario::UndeclaredBase:
    static_cast<void>(WithMembers.Base<Foreign>(ForeignKey()).QualifiedName());
    break;
  case Scenario::DuplicateBase:
    static_cast<void>(WithMembers.Base<Vehicle>(VehicleKey())
                          .Base<Vehicle>(VehicleKey())
                          .QualifiedName());
    break;
  case Scenario::UnavailableCastSource:
    static_cast<void>(WithMembers.Cast<Vehicle>(VehicleKey()).QualifiedName());
    break;
  case Scenario::DuplicateCast:
    static_cast<void>(WithMembers.Base<Vehicle>(VehicleKey())
                          .Cast<Vehicle>(VehicleKey())
                          .Cast<Vehicle>(VehicleKey())
                          .QualifiedName());
    break;
  case Scenario::UnsafeCastPolicy:
    static_cast<void>(WithMembers.Base<Painted>(PaintedKey())
                          .Cast<Painted>(PaintedKey())
                          .QualifiedName());
    break;
  default:
    break;
  }
  return Studio.Commit();
}

[[nodiscard]] bool Contains(std::string_view Text, std::string_view Needle) {
  return Needle.empty() || Text.find(Needle) != std::string_view::npos;
}

[[nodiscard]] std::string Expose(Luna::State &Owner, const std::string &Path,
                                 void *Storage,
                                 const std::string &QualifiedName,
                                 const std::uint64_t *Lifetime) {
  Luna::Detail::ClassExposureRequest Request;
  Request.QualifiedName = QualifiedName;
  Request.Path = Path;
  Request.Storage = Storage;
  Request.Ownership = OwnershipModel::Borrowed;
  Request.Access = ConstAccess::Mutable;
  Request.LifetimeGeneration = Lifetime;
  return Hooks::ExposeClassUserdata(Owner, Request).Status;
}

[[nodiscard]] Luna::Detail::ClassAccessObservation
ReadAs(Luna::State &Owner, const std::string &Path,
       const std::string &QualifiedName, const void *Expected) {
  Luna::Detail::ClassAccessRequest Request;
  Request.QualifiedName = QualifiedName;
  Request.Path = Path;
  Request.ExpectedStorage = Expected;
  return Hooks::AccessClassUserdata(Owner, Request);
}

// One accepted model, exercised through the ordinary gate and the real virtual
// machine: every base view of a derived object, one class no path leads to, the
// safe downcast of a generated dynamic type, and the receiver validation and
// overload ranking a base member still goes through.
void ExerciseAcceptedModel(Luna::State &Owner, bool WithCast,
                           ByteCursor &Cursor) {
  std::uint64_t Lifetime = 1;
  const std::optional<int> EntryDepth = Hooks::ObserveRootStackDepth(Owner);

  Truck Value;
  Value.Wheels = 6;
  Value.Coats = 3;
  Value.Beds = 2;
  RC_ASSERT(Expose(Owner, "TruckValue", &Value, "Studio.Truck", &Lifetime) ==
            "created");

  const auto AsSelf = ReadAs(Owner, "TruckValue", "Studio.Truck", &Value);
  RC_ASSERT(AsSelf.ReachedNativeCode);
  RC_ASSERT(AsSelf.DeliveredExpectedObject);

  const auto AsVehicle = ReadAs(Owner, "TruckValue", "Studio.Vehicle",
                                static_cast<Vehicle *>(&Value));
  RC_ASSERT(AsVehicle.ReachedNativeCode);
  RC_ASSERT(AsVehicle.DeliveredExpectedObject);

  // The second base of this value really is at another offset, so this is the
  // check that a path is composed rather than assumed to be the identity.
  RC_ASSERT(!Delivers(static_cast<Painted *>(&Value), &Value));
  const auto AsPainted = ReadAs(Owner, "TruckValue", "Studio.Painted",
                                static_cast<Painted *>(&Value));
  RC_ASSERT(AsPainted.ReachedNativeCode);
  RC_ASSERT(AsPainted.DeliveredExpectedObject);

  const auto AsForeign = ReadAs(Owner, "TruckValue", "Studio.Foreign", &Value);
  RC_ASSERT(!AsForeign.ReachedNativeCode);
  RC_ASSERT(Contains(AsForeign.Diagnostic, "Studio.Foreign"));
  const auto Repeated = ReadAs(Owner, "TruckValue", "Studio.Foreign", &Value);
  RC_ASSERT(Repeated.Failure == AsForeign.Failure);
  RC_ASSERT(Repeated.Diagnostic == AsForeign.Diagnostic);

  RC_ASSERT(Owner.Execute("Result = Studio.Vehicle.WheelCount(TruckValue)")
                .IsSuccess());
  RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "Result") ==
            std::optional<int>(6));
  RC_ASSERT(Owner.Execute("Result = Studio.Painted.CoatCount(TruckValue)")
                .IsSuccess());
  RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "Result") ==
            std::optional<int>(3));

  // A base member reached with a derived receiver still selects its candidate
  // by the ordinary conversion ranking of one canonical overload set.
  RC_ASSERT(Hooks::OverloadCandidateCount(Owner, "Studio.Vehicle.Describe") ==
            2);
  RC_ASSERT(Owner.Execute("Result = Studio.Vehicle.Describe(TruckValue, 3)")
                .IsSuccess());
  RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "Result") ==
            std::optional<int>(603));
  RC_ASSERT(
      Owner.Execute("Result = Studio.Vehicle.Describe(TruckValue, 'abcd')")
          .IsSuccess());
  RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "Result") ==
            std::optional<int>(1004));

  // One object exposed as a value of its base, whose dynamic class is
  // generated.
  const bool ReallyDerived = Cursor.Pick(2) == 0;
  Truck Actual;
  Actual.Wheels = 8;
  Vehicle Plain;
  Plain.Wheels = 2;
  void *const BaseStorage =
      ReallyDerived ? static_cast<void *>(static_cast<Vehicle *>(&Actual))
                    : static_cast<void *>(&Plain);
  RC_ASSERT(Expose(Owner, "BaseValue", BaseStorage, "Studio.Vehicle",
                   &Lifetime) == "created");

  const auto AsDerived =
      ReadAs(Owner, "BaseValue", "Studio.Truck",
             ReallyDerived ? static_cast<const void *>(&Actual) : nullptr);
  if (WithCast && ReallyDerived) {
    RC_ASSERT(AsDerived.ReachedNativeCode);
    RC_ASSERT(AsDerived.DeliveredExpectedObject);
  } else {
    // Either no policy was registered at all, or the registered non-mutating
    // check refused this object before any committing conversion.
    RC_ASSERT(!AsDerived.ReachedNativeCode);
    RC_ASSERT(!AsDerived.Diagnostic.empty());
    RC_ASSERT(AsDerived.Failure == (WithCast ? "incompatible_userdata_object"
                                             : "userdata_type_mismatch"));
  }

  RC_ASSERT(Hooks::ObserveRootStackDepth(Owner) == EntryDepth);
  RC_ASSERT(Owner.IsReady());
  RC_ASSERT(Owner.Execute("Recovered = 5").IsSuccess());
  RC_ASSERT(Hooks::ObserveIntegerGlobal(Owner, "Recovered") ==
            std::optional<int>(5));
}

void VerifyRegistrationSlice(ByteCursor &Cursor) {
  const std::size_t Choice = Cursor.Pick(ScenarioTable.size());
  const Scenario Which = static_cast<Scenario>(Choice);
  const ScenarioTraits &Traits = ScenarioTable[Choice];

  Luna::State Owner;
  const std::optional<int> EntryDepth = Hooks::ObserveRootStackDepth(Owner);
  const Luna::RegistrationResult Result = RegisterScenario(Owner, Which);
  RC_ASSERT(Result.IsSuccess() == Traits.Accepted);
  RC_ASSERT(Hooks::ObserveRootStackDepth(Owner) == EntryDepth);

  if (Traits.Accepted) {
    ExerciseAcceptedModel(Owner, Which == Scenario::AcceptedWithCast, Cursor);
    RC_TAG(std::string("registration: ") + Traits.Name);
    return;
  }

  const Luna::ErrorDiagnostic *const Diagnostic = Result.Diagnostic();
  const bool Reported = Diagnostic != nullptr;
  RC_ASSERT(Reported);
  RC_ASSERT(Contains(Diagnostic->Message(), Traits.Phrase));

  // The refusal is one deterministic diagnostic, not a property of the State it
  // happened in.
  Luna::State Elsewhere;
  const Luna::RegistrationResult Again = RegisterScenario(Elsewhere, Which);
  RC_ASSERT(!Again.IsSuccess());
  const bool ReportedAgain = Again.Diagnostic() != nullptr;
  RC_ASSERT(ReportedAgain);
  RC_ASSERT(Again.Diagnostic()->Message() == Diagnostic->Message());

  // Nothing was published, so the same State still registers the whole accepted
  // model and every conversion it owns.
  RC_ASSERT(RegisterScenario(Owner, Scenario::AcceptedWithCast).IsSuccess());
  ExerciseAcceptedModel(Owner, true, Cursor);
  RC_TAG(std::string("registration: ") + Traits.Name);
}

} // namespace

int RunInheritanceAndCastPathProperties() {
  // clang-format off
  // **Validates: Requirements 14.1, 14.2, 14.3, 14.4, 14.8, 14.9**
  // Feature: reflection-driven-binding-system, Property 29: Inheritance and casts agree with unique accessible paths
  const bool Passed = rc::check(
      // clang-format on
      "Inheritance and casts agree with unique accessible paths",
      [](const std::vector<std::uint8_t> &GraphBytes,
         const std::vector<std::uint8_t> &RegistrationBytes) {
        ByteCursor Graph(GraphBytes);
        VerifyGeneratedRelationshipGraph(Graph);

        ByteCursor Registration(RegistrationBytes);
        VerifyRegistrationSlice(Registration);
      });
  return Passed ? 0 : 1;
}
