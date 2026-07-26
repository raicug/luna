// Focused coverage of the explicit class relationship graph: base edges, safe
// downcast policies, derived-to-base receiver and argument adjustment, and the
// inherited-member ambiguity the graph finally makes reachable.
//
// The graph itself is checked as a value first, because every refusal it
// reports is decidable without a virtual machine at all: a declaration that is
// not a base, an inaccessible one, an unregistered one, a duplicate edge, an
// edge that closes a cycle, and a pair of classes reachable through two paths.
// The same refusals are then taken through real registration, and the accepted
// graph is exercised through the ordinary access gate, where a base view of a
// derived object must arrive as the adjusted pointer and an incompatible object
// must be refused before anything is converted.

// clang-format off
#include <luna/binding/binding_registry.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_relationship.hpp>
#include <luna/binding/namespace_builder.hpp>
#include <luna/reflection/reflection_record.hpp>
#include <luna/reflection/reflection_snapshot.hpp>
#include <luna/state/state.hpp>
#include <luna/type/stable_type_key.hpp>

#include "state/registration/relationship_plan.hpp"
#include "state/testing/test_hooks.hpp"
#include "state/userdata/access.hpp"
#include "state/userdata/class_relationships.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace {

using Hooks = Luna::Detail::StateTestHooks;
using Luna::Detail::ClassConversionKind;
using Luna::Detail::ClassRelationships;
using Luna::Detail::ConstAccess;
using Luna::Detail::MakeCandidateBase;
using Luna::Detail::MakeCandidateCast;
using Luna::Detail::OwnershipModel;
using Luna::Detail::RelationshipCandidate;
using Luna::Detail::RelationshipClass;
using Luna::Detail::RelationshipFailure;

int FailureCount = 0;

void Check(bool Condition, std::string_view Description) {
  if (Condition)
    return;
  ++FailureCount;
  std::cerr << "class relationship check failed: " << Description << '\n';
}

// -- the model under test ---------------------------------------------------

struct Shape {
  virtual ~Shape() = default;
  int Sides = 3;

  [[nodiscard]] int SideCount() const { return Sides; }
};

// The second base of the registered class, so a derived-to-base adjustment of
// it is a real pointer adjustment rather than the identity.
struct Tagged {
  int Tag = 7;

  [[nodiscard]] int TagValue() const { return Tag; }
};

struct Square final : Shape, Tagged {
  int Edge = 4;

  [[nodiscard]] int EdgeLength() const { return Edge; }
};

struct Unrelated final {
  int Value = 11;
};

struct Hidden final : private Shape {
  int Kept = 5;
};

// A diamond: two accessible bases that both reach one further base, so exactly
// two paths connect the pair.
struct Left : Shape {
  int LeftValue = 1;
};

struct Right : Shape {
  int RightValue = 2;
};

struct Diamond final : Left, Right {
  int Center = 3;
};

[[nodiscard]] Luna::StableTypeKey ShapeKey() {
  return Luna::StableTypeKey("Studio.RelationshipShape");
}

[[nodiscard]] Luna::StableTypeKey TaggedKey() {
  return Luna::StableTypeKey("Studio.RelationshipTagged");
}

[[nodiscard]] Luna::StableTypeKey SquareKey() {
  return Luna::StableTypeKey("Studio.RelationshipSquare");
}

[[nodiscard]] Luna::StableTypeKey UnrelatedKey() {
  return Luna::StableTypeKey("Studio.RelationshipUnrelated");
}

// -- the graph as a value ---------------------------------------------------

[[nodiscard]] RelationshipClass DeclaredClass(const Luna::StableTypeKey &Key,
                                              std::string Name,
                                              bool IsPending = true) {
  RelationshipClass Described;
  Described.Type = Luna::Detail::ClassTypeIdentityOf(Key);
  Described.Key = Key;
  Described.QualifiedName = std::move(Name);
  Described.IsPending = IsPending;
  return Described;
}

[[nodiscard]] Luna::TypeId TypeOf(const Luna::StableTypeKey &Key) {
  return Luna::Detail::ClassTypeIdentityOf(Key);
}

void CheckAcceptedGraphIsAcyclicAndUnique() {
  RelationshipCandidate Candidate;
  Candidate.AddClass(DeclaredClass(ShapeKey(), "Studio.Shape"));
  Candidate.AddClass(DeclaredClass(TaggedKey(), "Studio.Tagged"));
  Candidate.AddClass(DeclaredClass(SquareKey(), "Studio.Square"));
  Candidate.AddBase(MakeCandidateBase(
      TypeOf(SquareKey()), "Studio.Square",
      Luna::Detail::MakeBaseRequest<Square, Shape>(ShapeKey())));
  Candidate.AddBase(MakeCandidateBase(
      TypeOf(SquareKey()), "Studio.Square",
      Luna::Detail::MakeBaseRequest<Square, Tagged>(TaggedKey())));

  Check(Luna::Detail::ClassifyRelationshipCandidate(Candidate) ==
            RelationshipFailure::None,
        "two accessible base edges of one class are accepted");
  Check(!Luna::Detail::ValidateRelationshipCandidate(Candidate).has_value(),
        "an acceptable graph produces no diagnostic");
  Check(Candidate.PathCount(TypeOf(SquareKey()), TypeOf(ShapeKey())) == 1,
        "exactly one path leads to each declared base");
  Check(Candidate.PathCount(TypeOf(ShapeKey()), TypeOf(SquareKey())) == 0,
        "a base never reaches its derived class");
  Check(Candidate.ReachableBases(TypeOf(SquareKey())).size() == 2,
        "both bases are reachable from the derived class");
}

void CheckUndeclaredAndInaccessibleBasesAreRefused() {
  RelationshipCandidate Foreign;
  Foreign.AddClass(DeclaredClass(SquareKey(), "Studio.Square"));
  Foreign.AddClass(DeclaredClass(UnrelatedKey(), "Studio.Unrelated"));
  Foreign.AddBase(MakeCandidateBase(
      TypeOf(SquareKey()), "Studio.Square",
      Luna::Detail::MakeBaseRequest<Square, Unrelated>(UnrelatedKey())));
  Check(Luna::Detail::ClassifyRelationshipCandidate(Foreign) ==
            RelationshipFailure::UndeclaredBase,
        "a class that is not a base at all is refused first");

  const Luna::StableTypeKey HiddenKey("Studio.RelationshipHidden");
  RelationshipCandidate Inaccessible;
  Inaccessible.AddClass(DeclaredClass(ShapeKey(), "Studio.Shape"));
  Inaccessible.AddClass(DeclaredClass(HiddenKey, "Studio.Hidden"));
  Inaccessible.AddBase(MakeCandidateBase(
      TypeOf(HiddenKey), "Studio.Hidden",
      Luna::Detail::MakeBaseRequest<Hidden, Shape>(ShapeKey())));
  Check(Luna::Detail::ClassifyRelationshipCandidate(Inaccessible) ==
            RelationshipFailure::InaccessibleBase,
        "a base that is not publicly reachable is refused");

  RelationshipCandidate Unavailable;
  Unavailable.AddClass(DeclaredClass(SquareKey(), "Studio.Square"));
  Unavailable.AddBase(MakeCandidateBase(
      TypeOf(SquareKey()), "Studio.Square",
      Luna::Detail::MakeBaseRequest<Square, Shape>(ShapeKey())));
  Check(Luna::Detail::ClassifyRelationshipCandidate(Unavailable) ==
            RelationshipFailure::UnavailableBase,
        "a base that is not a registered class is refused");
}

void CheckDuplicateCyclicAndAmbiguousEdgesAreRefused() {
  RelationshipCandidate Duplicate;
  Duplicate.AddClass(DeclaredClass(ShapeKey(), "Studio.Shape"));
  Duplicate.AddClass(DeclaredClass(SquareKey(), "Studio.Square"));
  for (int Repeat = 0; Repeat < 2; ++Repeat)
    Duplicate.AddBase(MakeCandidateBase(
        TypeOf(SquareKey()), "Studio.Square",
        Luna::Detail::MakeBaseRequest<Square, Shape>(ShapeKey())));
  Check(Luna::Detail::ClassifyRelationshipCandidate(Duplicate) ==
            RelationshipFailure::DuplicateBase,
        "one base edge is declared exactly once");

  // A cycle is only expressible by describing two classes as bases of each
  // other, which is why the declared facts of one edge are never enough on
  // their own.
  RelationshipCandidate Cyclic;
  Cyclic.AddClass(DeclaredClass(ShapeKey(), "Studio.Shape"));
  Cyclic.AddClass(DeclaredClass(SquareKey(), "Studio.Square"));
  Cyclic.AddBase(MakeCandidateBase(
      TypeOf(SquareKey()), "Studio.Square",
      Luna::Detail::MakeBaseRequest<Square, Shape>(ShapeKey())));
  Luna::Detail::BaseRequest Reversed =
      Luna::Detail::MakeBaseRequest<Square, Shape>(SquareKey());
  Cyclic.AddBase(
      MakeCandidateBase(TypeOf(ShapeKey()), "Studio.Shape", Reversed));
  Check(Luna::Detail::ClassifyRelationshipCandidate(Cyclic) ==
            RelationshipFailure::CyclicBase,
        "a base edge that closes a cycle is refused");

  const Luna::StableTypeKey LeftKey("Studio.RelationshipLeft");
  const Luna::StableTypeKey RightKey("Studio.RelationshipRight");
  const Luna::StableTypeKey DiamondKey("Studio.RelationshipDiamond");
  RelationshipCandidate Ambiguous;
  Ambiguous.AddClass(DeclaredClass(ShapeKey(), "Studio.Shape"));
  Ambiguous.AddClass(DeclaredClass(LeftKey, "Studio.Left"));
  Ambiguous.AddClass(DeclaredClass(RightKey, "Studio.Right"));
  Ambiguous.AddClass(DeclaredClass(DiamondKey, "Studio.Diamond"));
  Ambiguous.AddBase(MakeCandidateBase(
      TypeOf(LeftKey), "Studio.Left",
      Luna::Detail::MakeBaseRequest<Left, Shape>(ShapeKey())));
  Ambiguous.AddBase(MakeCandidateBase(
      TypeOf(RightKey), "Studio.Right",
      Luna::Detail::MakeBaseRequest<Right, Shape>(ShapeKey())));
  Ambiguous.AddBase(
      MakeCandidateBase(TypeOf(DiamondKey), "Studio.Diamond",
                        Luna::Detail::MakeBaseRequest<Diamond, Left>(LeftKey)));
  Ambiguous.AddBase(MakeCandidateBase(
      TypeOf(DiamondKey), "Studio.Diamond",
      Luna::Detail::MakeBaseRequest<Diamond, Right>(RightKey)));
  Check(Ambiguous.PathCount(TypeOf(DiamondKey), TypeOf(ShapeKey())) == 2,
        "a diamond connects one pair through two paths");
  Check(Luna::Detail::ClassifyRelationshipCandidate(Ambiguous) ==
            RelationshipFailure::AmbiguousBasePath,
        "a pair reachable through more than one path is refused");
}

void CheckCastPoliciesAreRefusedWithoutOneAccessiblePath() {
  RelationshipCandidate Foreign;
  Foreign.AddClass(DeclaredClass(SquareKey(), "Studio.Square"));
  Foreign.AddClass(DeclaredClass(UnrelatedKey(), "Studio.Unrelated"));
  Foreign.AddCast(MakeCandidateCast(
      TypeOf(SquareKey()), "Studio.Square",
      Luna::Detail::MakeRuntimeTypeCastRequest<Square, Unrelated>(
          UnrelatedKey())));
  Check(Luna::Detail::ClassifyRelationshipCandidate(Foreign) ==
            RelationshipFailure::UndeclaredCastSource,
        "a downcast never starts at a class that is not a base");

  RelationshipCandidate Unregistered;
  Unregistered.AddClass(DeclaredClass(SquareKey(), "Studio.Square"));
  Unregistered.AddClass(DeclaredClass(ShapeKey(), "Studio.Shape"));
  Unregistered.AddCast(MakeCandidateCast(
      TypeOf(SquareKey()), "Studio.Square",
      Luna::Detail::MakeRuntimeTypeCastRequest<Square, Shape>(ShapeKey())));
  Check(Luna::Detail::ClassifyRelationshipCandidate(Unregistered) ==
            RelationshipFailure::UnavailableCastSource,
        "a downcast requires the registered accessible base path it mirrors");

  RelationshipCandidate Accepted;
  Accepted.AddClass(DeclaredClass(SquareKey(), "Studio.Square"));
  Accepted.AddClass(DeclaredClass(ShapeKey(), "Studio.Shape"));
  Accepted.AddBase(MakeCandidateBase(
      TypeOf(SquareKey()), "Studio.Square",
      Luna::Detail::MakeBaseRequest<Square, Shape>(ShapeKey())));
  Accepted.AddCast(MakeCandidateCast(
      TypeOf(SquareKey()), "Studio.Square",
      Luna::Detail::MakeRuntimeTypeCastRequest<Square, Shape>(ShapeKey())));
  Check(Luna::Detail::ClassifyRelationshipCandidate(Accepted) ==
            RelationshipFailure::None,
        "a downcast over one registered accessible path is accepted");

  RelationshipCandidate DuplicatePolicy;
  RelationshipCandidate &Duplicate = DuplicatePolicy;
  Duplicate.AddClass(DeclaredClass(SquareKey(), "Studio.Square"));
  Duplicate.AddClass(DeclaredClass(ShapeKey(), "Studio.Shape"));
  Duplicate.AddBase(MakeCandidateBase(
      TypeOf(SquareKey()), "Studio.Square",
      Luna::Detail::MakeBaseRequest<Square, Shape>(ShapeKey())));
  for (int Repeat = 0; Repeat < 2; ++Repeat)
    Duplicate.AddCast(MakeCandidateCast(
        TypeOf(SquareKey()), "Studio.Square",
        Luna::Detail::MakeRuntimeTypeCastRequest<Square, Shape>(ShapeKey())));
  Check(Luna::Detail::ClassifyRelationshipCandidate(Duplicate) ==
            RelationshipFailure::DuplicateCast,
        "one cast policy is declared exactly once");

  // A source Luna cannot decide compatibility of at all: the declaration names
  // no non-mutating check, so nothing about the object could ever be verified.
  RelationshipCandidate Unsafe;
  Unsafe.AddClass(DeclaredClass(TaggedKey(), "Studio.Tagged"));
  Unsafe.AddClass(DeclaredClass(SquareKey(), "Studio.Square"));
  Unsafe.AddBase(MakeCandidateBase(
      TypeOf(SquareKey()), "Studio.Square",
      Luna::Detail::MakeBaseRequest<Square, Tagged>(TaggedKey())));
  Unsafe.AddCast(MakeCandidateCast(
      TypeOf(SquareKey()), "Studio.Square",
      Luna::Detail::MakeRuntimeTypeCastRequest<Square, Tagged>(TaggedKey())));
  Check(Luna::Detail::ClassifyRelationshipCandidate(Unsafe) ==
            RelationshipFailure::UnsafeCastPolicy,
        "a non-polymorphic source has no runtime-type compatibility check");
}

// The published graph composes a multi-step path rather than assuming one
// adjustment, and it reports every other pair as unrelated.
void CheckPublishedPathsComposeEveryAdjustment() {
  const Luna::StableTypeKey LeftKey("Studio.RelationshipLeft");
  const Luna::StableTypeKey DiamondKey("Studio.RelationshipDiamond");

  ClassRelationships Graph;
  Graph.RecordNode(TypeOf(ShapeKey()), Luna::Detail::MetatableId::FromValue(1),
                   "Studio.Shape");
  Graph.RecordNode(TypeOf(LeftKey), Luna::Detail::MetatableId::FromValue(2),
                   "Studio.Left");
  Graph.RecordNode(TypeOf(DiamondKey), Luna::Detail::MetatableId::FromValue(3),
                   "Studio.Diamond");

  Luna::Detail::ClassBaseEdge ToShape;
  ToShape.Derived = TypeOf(LeftKey);
  ToShape.Base = TypeOf(ShapeKey());
  ToShape.Upcast =
      Luna::Detail::MakeBaseRequest<Left, Shape>(ShapeKey()).Upcast;
  Graph.RecordBase(ToShape);

  Luna::Detail::ClassBaseEdge ToLeft;
  ToLeft.Derived = TypeOf(DiamondKey);
  ToLeft.Base = TypeOf(LeftKey);
  ToLeft.Upcast = Luna::Detail::MakeBaseRequest<Diamond, Left>(LeftKey).Upcast;
  Graph.RecordBase(ToLeft);
  Graph.Rebuild();

  Check(Graph.NodeCount() == 3 && Graph.BaseCount() == 2,
        "the graph records exactly the nodes and edges it was given");
  Check(Graph.PathCount() == 3,
        "three ordered pairs are connected by one path each");
  Check(Graph.MetatableOf(TypeOf(LeftKey)) ==
            Luna::Detail::MetatableId::FromValue(2),
        "each node keeps the metatable identity of its own class");

  Diamond Value;
  const auto Direct = Graph.Resolve(TypeOf(DiamondKey), TypeOf(LeftKey));
  Check(Direct.Kind == ClassConversionKind::Upcast &&
            Luna::Detail::ApplyClassConversion(Direct, &Value) ==
                static_cast<Left *>(&Value),
        "one edge adjusts to the immediate base");

  const auto Composed = Graph.Resolve(TypeOf(DiamondKey), TypeOf(ShapeKey()));
  Check(Composed.Kind == ClassConversionKind::Upcast &&
            Luna::Detail::ApplyClassConversion(Composed, &Value) ==
                static_cast<Shape *>(static_cast<Left *>(&Value)),
        "a two-step path composes both adjustments in order");

  Check(Graph.Resolve(TypeOf(ShapeKey()), TypeOf(DiamondKey)).Kind ==
            ClassConversionKind::Unrelated,
        "no path leads from a base back to its derived class without a cast");
  Check(Graph.Resolve(TypeOf(ShapeKey()), TypeOf(ShapeKey())).Kind ==
            ClassConversionKind::Identity,
        "a class always reaches itself");
}

void CheckInheritedMemberAmbiguityIsCounted() {
  RelationshipCandidate Candidate;
  RelationshipClass ShapeSide = DeclaredClass(ShapeKey(), "Studio.Shape");
  ShapeSide.MemberNames.push_back("Weight");
  RelationshipClass TaggedSide = DeclaredClass(TaggedKey(), "Studio.Tagged");
  TaggedSide.MemberNames.push_back("Weight");
  TaggedSide.MemberNames.push_back("Tag");
  Candidate.AddClass(std::move(ShapeSide));
  Candidate.AddClass(std::move(TaggedSide));
  Candidate.AddClass(DeclaredClass(SquareKey(), "Studio.Square"));
  Candidate.AddBase(MakeCandidateBase(
      TypeOf(SquareKey()), "Studio.Square",
      Luna::Detail::MakeBaseRequest<Square, Shape>(ShapeKey())));
  Candidate.AddBase(MakeCandidateBase(
      TypeOf(SquareKey()), "Studio.Square",
      Luna::Detail::MakeBaseRequest<Square, Tagged>(TaggedKey())));

  Check(Candidate.InheritedDeclarationCount(TypeOf(SquareKey()), "Weight") == 2,
        "a name declared by two bases is inherited twice");
  Check(Candidate.InheritedDeclarationCount(TypeOf(SquareKey()), "Tag") == 1,
        "a name declared by one base is inherited once");
  Check(Candidate.InheritedDeclarationCount(TypeOf(SquareKey()), "Edge") == 0,
        "a name no base declares is not inherited at all");
}

// -- the graph through real registration ------------------------------------

// The lifetime a borrowed exposure requires, modelled as one generation counter
// the test owns.
std::uint64_t Lifetime = 1;

[[nodiscard]] std::string ExposeBorrowed(Luna::State &Owner,
                                         const std::string &Path, void *Storage,
                                         std::string_view QualifiedName) {
  Luna::Detail::ClassExposureRequest Request;
  Request.QualifiedName = std::string(QualifiedName);
  Request.Path = Path;
  Request.Storage = Storage;
  Request.Ownership = OwnershipModel::Borrowed;
  Request.Access = ConstAccess::Mutable;
  Request.LifetimeGeneration = &Lifetime;
  return Hooks::ExposeClassUserdata(Owner, Request).Status;
}

[[nodiscard]] Luna::Detail::ClassAccessObservation
ReadAs(Luna::State &Owner, const std::string &Path,
       std::string_view QualifiedName, const void *Expected) {
  Luna::Detail::ClassAccessRequest Request;
  Request.QualifiedName = std::string(QualifiedName);
  Request.Path = Path;
  Request.ExpectedStorage = Expected;
  return Hooks::AccessClassUserdata(Owner, Request);
}

[[nodiscard]] bool Contains(std::string_view Text, std::string_view Needle) {
  return Text.find(Needle) != std::string_view::npos;
}

// One plan carrying the whole accepted graph: two bases and the derived class
// that names both of them.
[[nodiscard]] Luna::RegistrationResult RegisterAcceptedModel(Luna::State &Owner,
                                                             bool WithCast) {
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Shape> Base =
      Studio.RegisterClass<Shape>("Shape", ShapeKey());
  static_cast<void>(
      Base.Method("SideCount", &Shape::SideCount).QualifiedName());
  Luna::ClassBuilder<Tagged> Side =
      Studio.RegisterClass<Tagged>("Tagged", TaggedKey());
  static_cast<void>(Side.Method("TagValue", &Tagged::TagValue).QualifiedName());
  Luna::ClassBuilder<Unrelated> Other =
      Studio.RegisterClass<Unrelated>("Unrelated", UnrelatedKey());
  static_cast<void>(Other.QualifiedName());

  Luna::ClassBuilder<Square> Derived =
      Studio.RegisterClass<Square>("Square", SquareKey());
  Luna::ClassBuilder<Square> &WithBases =
      Derived.Constructor<>()
          .Method("EdgeLength", &Square::EdgeLength)
          .Base<Shape>(ShapeKey())
          .Base<Tagged>(TaggedKey());
  if (WithCast)
    static_cast<void>(WithBases.Cast<Shape>(ShapeKey()).QualifiedName());
  else
    static_cast<void>(WithBases.QualifiedName());
  return Studio.Commit();
}

// A derived value is received as either of its bases, as the adjusted pointer
// each base view sees, and never as a class no path leads to.
void CheckDerivedValueReachesEveryRegisteredBase() {
  Luna::State Owner;
  Check(RegisterAcceptedModel(Owner, false).IsSuccess(),
        "one plan publishes two bases and the class that names them");

  Square Value;
  Value.Sides = 4;
  Value.Tag = 9;
  Check(ExposeBorrowed(Owner, "SquareValue", &Value, "Studio.Square") ==
            "created",
        "the derived value is exposed as its own class");

  const auto AsSelf = ReadAs(Owner, "SquareValue", "Studio.Square", &Value);
  Check(AsSelf.ReachedNativeCode && AsSelf.DeliveredExpectedObject,
        "the derived class receives its own value unadjusted");

  const auto AsShape = ReadAs(Owner, "SquareValue", "Studio.Shape",
                              static_cast<Shape *>(&Value));
  Check(AsShape.ReachedNativeCode && AsShape.DeliveredExpectedObject,
        "the first base receives the adjusted pointer of the derived value");

  // The second base of a value with two of them starts at a different address
  // than the object itself, so this is the check that a path is composed rather
  // than assumed to be the identity.
  const void *const TaggedView = static_cast<Tagged *>(&Value);
  Check(TaggedView != static_cast<const void *>(&Value),
        "the second base of this value really is at another offset");
  const auto AsTagged = ReadAs(Owner, "SquareValue", "Studio.Tagged",
                               static_cast<Tagged *>(&Value));
  Check(AsTagged.ReachedNativeCode && AsTagged.DeliveredExpectedObject,
        "the second base receives its own adjusted pointer");

  const auto AsUnrelated =
      ReadAs(Owner, "SquareValue", "Studio.Unrelated", &Value);
  Check(!AsUnrelated.ReachedNativeCode,
        "a registered class no path leads to never receives the value");
  Check(Contains(AsUnrelated.Diagnostic, "Studio.Unrelated"),
        "the refusal names the class that was requested");

  // The same adjustment through the real virtual machine: a base member called
  // with a derived receiver operates on the base view of that object.
  Check(
      Owner.Execute("Result = Studio.Shape.SideCount(SquareValue)").IsSuccess(),
      "a base method accepts a derived receiver from script");
  const auto Sides = Hooks::ObserveIntegerGlobal(Owner, "Result");
  Check(Sides && *Sides == 4, "the base member read the derived object");

  Check(
      Owner.Execute("Result = Studio.Tagged.TagValue(SquareValue)").IsSuccess(),
      "the second base also accepts the derived receiver");
  const auto Tag = Hooks::ObserveIntegerGlobal(Owner, "Result");
  Check(Tag && *Tag == 9, "the second base read its own adjusted view");
}

// A base value is received as the derived class only through an explicitly
// registered safe cast policy, and only when the object really is one.
void CheckSafeDowncastsRequireAnExplicitPolicy() {
  Luna::State Refusing;
  Check(RegisterAcceptedModel(Refusing, false).IsSuccess(),
        "the model without a cast policy registers");

  Square Actual;
  Check(ExposeBorrowed(Refusing, "ShapeValue", static_cast<Shape *>(&Actual),
                       "Studio.Shape") == "created",
        "a derived object is exposed as a value of its base");
  const auto Refused = ReadAs(Refusing, "ShapeValue", "Studio.Square", &Actual);
  Check(!Refused.ReachedNativeCode,
        "without a cast policy a base value is not a value of the derived "
        "class");
  Check(Refused.Failure == "userdata_type_mismatch",
        "the refusal is an ordinary type mismatch");

  Luna::State Casting;
  Check(RegisterAcceptedModel(Casting, true).IsSuccess(),
        "the model with a cast policy registers");

  Square Compatible;
  Check(ExposeBorrowed(Casting, "ShapeValue", static_cast<Shape *>(&Compatible),
                       "Studio.Shape") == "created",
        "the compatible object is exposed as a value of its base");
  const auto Accepted =
      ReadAs(Casting, "ShapeValue", "Studio.Square", &Compatible);
  Check(Accepted.ReachedNativeCode && Accepted.DeliveredExpectedObject,
        "one registered safe downcast delivers the derived object");

  Shape Incompatible;
  Check(ExposeBorrowed(Casting, "PlainValue", &Incompatible, "Studio.Shape") ==
            "created",
        "the incompatible object is exposed as a value of its own class");
  const auto Rejected =
      ReadAs(Casting, "PlainValue", "Studio.Square", &Incompatible);
  Check(!Rejected.ReachedNativeCode,
        "an object that is not a value of the derived class is refused");
  Check(Rejected.Failure == "incompatible_userdata_object",
        "the refusal names the compatibility check rather than the path");
  Check(Contains(Rejected.Diagnostic, "registered safe cast refused"),
        "the diagnostic explains that the object is not a value of the class");
}

// Every family of refused relationship, taken through real registration, leaves
// the State reusable.
void CheckRefusedRelationshipsAreTransactional() {
  Luna::State Owner;
  {
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::ClassBuilder<Square> Derived =
        Registry.RegisterClass<Square>("Square", SquareKey());
    const auto Result = Derived.Base<Shape>(ShapeKey()).Commit();
    Check(!Result.IsSuccess(), "a base that is not registered is refused");
    if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
      Check(Contains(Diagnostic->Message(), "not a registered class"),
            "the refusal names the unavailable base");
  }
  {
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    Luna::ClassBuilder<Unrelated> Other =
        Studio.RegisterClass<Unrelated>("Unrelated", UnrelatedKey());
    static_cast<void>(Other.QualifiedName());
    Luna::ClassBuilder<Square> Derived =
        Studio.RegisterClass<Square>("Square", SquareKey());
    static_cast<void>(Derived.Base<Unrelated>(UnrelatedKey()).QualifiedName());
    const auto Result = Studio.Commit();
    Check(!Result.IsSuccess(), "a class that is not a base at all is refused");
    if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
      Check(Contains(Diagnostic->Message(), "is not a base of this class"),
            "the refusal names the missing relationship");
  }
  {
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    Luna::ClassBuilder<Shape> Base =
        Studio.RegisterClass<Shape>("Shape", ShapeKey());
    static_cast<void>(Base.QualifiedName());
    Luna::ClassBuilder<Square> Derived =
        Studio.RegisterClass<Square>("Square", SquareKey());
    static_cast<void>(Derived.Base<Shape>(ShapeKey())
                          .Base<Shape>(ShapeKey())
                          .QualifiedName());
    const auto Result = Studio.Commit();
    Check(!Result.IsSuccess(), "a duplicate base edge is refused");
    if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
      Check(Contains(Diagnostic->Message(), "already declared by this class"),
            "the refusal names the duplicate edge");
  }
  {
    const Luna::StableTypeKey LeftKey("Studio.RelationshipLeft");
    const Luna::StableTypeKey RightKey("Studio.RelationshipRight");
    const Luna::StableTypeKey DiamondKey("Studio.RelationshipDiamond");
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    Luna::ClassBuilder<Shape> Base =
        Studio.RegisterClass<Shape>("Shape", ShapeKey());
    static_cast<void>(Base.QualifiedName());
    Luna::ClassBuilder<Left> LeftSide =
        Studio.RegisterClass<Left>("Left", LeftKey);
    static_cast<void>(LeftSide.Base<Shape>(ShapeKey()).QualifiedName());
    Luna::ClassBuilder<Right> RightSide =
        Studio.RegisterClass<Right>("Right", RightKey);
    static_cast<void>(RightSide.Base<Shape>(ShapeKey()).QualifiedName());
    Luna::ClassBuilder<Diamond> Center =
        Studio.RegisterClass<Diamond>("Diamond", DiamondKey);
    static_cast<void>(
        Center.Base<Left>(LeftKey).Base<Right>(RightKey).QualifiedName());
    const auto Result = Studio.Commit();
    Check(!Result.IsSuccess(),
          "a pair reachable through two accessible paths is refused");
    if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
      Check(Contains(Diagnostic->Message(), "more than one accessible base "
                                            "path"),
            "the refusal names the ambiguous pair");
  }
  {
    Luna::BindingRegistry Registry = Owner.Bindings();
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    Luna::ClassBuilder<Shape> Base =
        Studio.RegisterClass<Shape>("Shape", ShapeKey());
    static_cast<void>(Base.QualifiedName());
    Luna::ClassBuilder<Square> Derived =
        Studio.RegisterClass<Square>("Square", SquareKey());
    static_cast<void>(Derived.Cast<Shape>(ShapeKey()).QualifiedName());
    const auto Result = Studio.Commit();
    Check(!Result.IsSuccess(),
          "a downcast without the registered base path it mirrors is refused");
  }

  // Nothing was published by any refusal, so the State still registers.
  Check(RegisterAcceptedModel(Owner, true).IsSuccess(),
        "the State remains reusable after every refused relationship");
  Check(Hooks::ClassMemberIsRegistered(Owner, "Studio.Square", "EdgeLength") ==
            false,
        "a method is a callable candidate rather than a typed accessor");
}

// The identity of a related class never depends on a runtime type name, a
// runtime type address, or the order the relationships were declared in, even
// though the cast policy of one of them decides compatibility with runtime type
// assistance.
void CheckPersistentIdentityIsIndependentOfRuntimeTypes() {
  Luna::State First;
  Check(RegisterAcceptedModel(First, true).IsSuccess(),
        "the related classes register with a cast policy");

  Luna::State Second;
  {
    Luna::BindingRegistry Registry = Second.Bindings();
    Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
    Luna::ClassBuilder<Unrelated> Other =
        Studio.RegisterClass<Unrelated>("Unrelated", UnrelatedKey());
    static_cast<void>(Other.QualifiedName());
    Luna::ClassBuilder<Tagged> Side =
        Studio.RegisterClass<Tagged>("Tagged", TaggedKey());
    static_cast<void>(Side.QualifiedName());
    Luna::ClassBuilder<Shape> Base =
        Studio.RegisterClass<Shape>("Shape", ShapeKey());
    static_cast<void>(Base.QualifiedName());
    Luna::ClassBuilder<Square> Derived =
        Studio.RegisterClass<Square>("Square", SquareKey());
    static_cast<void>(Derived.Cast<Shape>(ShapeKey())
                          .Base<Tagged>(TaggedKey())
                          .Base<Shape>(ShapeKey())
                          .QualifiedName());
    Check(Studio.Commit().IsSuccess(),
          "the same relationships register in another declaration order");
  }

  const auto FirstDerived = Hooks::ClassTypeOf(First, "Studio.Square");
  const auto SecondDerived = Hooks::ClassTypeOf(Second, "Studio.Square");
  const auto FirstBase = Hooks::ClassTypeOf(First, "Studio.Shape");
  const auto SecondBase = Hooks::ClassTypeOf(Second, "Studio.Shape");
  Check(FirstDerived && SecondDerived && FirstBase && SecondBase,
        "both States registered both classes");
  if (!FirstDerived || !SecondDerived || !FirstBase || !SecondBase)
    return;

  Check(*FirstDerived == *SecondDerived && *FirstBase == *SecondBase,
        "the canonical identity of a related class is the same in both States");
  Check(*FirstDerived == TypeOf(SquareKey()) &&
            *FirstBase == TypeOf(ShapeKey()),
        "a class identity follows its stable key alone");
  Check(!(*FirstDerived == *FirstBase),
        "a derived class and its base keep distinct identities");
}

// The inherited-member ambiguity arm of the member collision order, reachable
// only once base edges exist.
void CheckInheritedMemberAmbiguityIsRefused() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Shape> Base =
      Studio.RegisterClass<Shape>("Shape", ShapeKey());
  static_cast<void>(Base.Field("Weight", &Shape::Sides).QualifiedName());
  Luna::ClassBuilder<Tagged> Side =
      Studio.RegisterClass<Tagged>("Tagged", TaggedKey());
  static_cast<void>(Side.Field("Weight", &Tagged::Tag).QualifiedName());

  Luna::ClassBuilder<Square> Derived =
      Studio.RegisterClass<Square>("Square", SquareKey());
  static_cast<void>(Derived.Base<Shape>(ShapeKey())
                        .Base<Tagged>(TaggedKey())
                        .Field("Weight", &Square::Edge)
                        .QualifiedName());

  const auto Result = Studio.Commit();
  Check(!Result.IsSuccess(),
        "a member name reachable from two bases is refused");
  if (const Luna::ErrorDiagnostic *Diagnostic = Result.Diagnostic())
    Check(Contains(Diagnostic->Message(),
                   "reachable from more than one base of this class"),
          "the refusal is the inherited-ambiguity arm of the collision order");
}

void CheckInheritedViewsRetainDeclarationOwnership() {
  Luna::State Owner;
  Check(RegisterAcceptedModel(Owner, true).IsSuccess(),
        "the inherited-view model registers");

  const auto Bases = Hooks::ClassBases(Owner, "Studio.Square");
  Check(Bases.size() == 2 && Bases[0].QualifiedName == "Studio.Shape" &&
            Bases[1].QualifiedName == "Studio.Tagged",
        "accessible bases enumerate by canonical qualified name");
  Check(Bases.size() == 2 && Bases[0].IsDirect && Bases[1].IsDirect &&
            Bases[0].IsAccessible && Bases[1].IsAccessible,
        "base views expose directness and accessibility");

  const auto Casts = Hooks::ClassCasts(Owner, "Studio.Square");
  Check(Casts.size() == 1 && Casts[0].QualifiedName == "Studio.Shape" &&
            !Casts[0].Policy.empty(),
        "safe casts enumerate with their canonical policy identity");

  const auto Inherited = Hooks::ClassInheritedMembers(Owner, "Studio.Square");
  Check(Inherited.size() == 2 && Inherited[0].Segment == "SideCount" &&
            Inherited[1].Segment == "TagValue",
        "inherited methods enumerate canonically");
  for (const auto &View : Inherited) {
    Check(View.Declaration.IsValid(),
          "an inherited view retains the original declaration identity");
    Check(!View.IsAmbiguous,
          "one declaring base produces an unambiguous inherited view");
  }

  const Luna::ReflectionRecord Reflected =
      Owner.Bindings().Reflection().Find("Studio.Square");
  std::size_t ReflectedInherited = 0;
  for (std::size_t Index = 0; Index < Reflected.RelationCount(); ++Index) {
    const Luna::TypeRelation Relation = Reflected.Relation(Index);
    if (Relation.Kind() != Luna::TypeRelationKind::Inherited)
      continue;
    ++ReflectedInherited;
    Check(Relation.Declaration().IsValid(),
          "reflection points at the original inherited declaration");
    Check(Contains(Relation.Note(), "unambiguous"),
          "reflection reports inherited ambiguity state explicitly");
  }
  Check(ReflectedInherited == 2,
        "reflection exposes every inherited method without cloning it");
}

void CheckAmbiguousInheritedViewsAreReported() {
  Luna::State Owner;
  Luna::BindingRegistry Registry = Owner.Bindings();
  Luna::NamespaceBuilder Studio = Registry.RegisterNamespace("Studio");
  Luna::ClassBuilder<Shape> Base =
      Studio.RegisterClass<Shape>("Shape", ShapeKey());
  Base.Field("Weight", &Shape::Sides);
  Luna::ClassBuilder<Tagged> Side =
      Studio.RegisterClass<Tagged>("Tagged", TaggedKey());
  Side.Field("Weight", &Tagged::Tag);
  Luna::ClassBuilder<Square> Derived =
      Studio.RegisterClass<Square>("Square", SquareKey());
  Derived.Base<Shape>(ShapeKey()).Base<Tagged>(TaggedKey());
  Check(Studio.Commit().IsSuccess(),
        "a class may expose an ambiguous inherited name without selecting it");

  const auto Inherited = Hooks::ClassInheritedMembers(Owner, "Studio.Square");
  Check(Inherited.size() == 2,
        "both original declarations remain visible for an ambiguous name");
  for (const auto &View : Inherited)
    Check(View.Segment == "Weight" && View.IsAmbiguous &&
              View.Declaration.IsValid(),
          "every ambiguous view reports ambiguity and declaration ownership");
}

} // namespace

int RunClassRelationshipTests() {
  FailureCount = 0;
  CheckAcceptedGraphIsAcyclicAndUnique();
  CheckUndeclaredAndInaccessibleBasesAreRefused();
  CheckDuplicateCyclicAndAmbiguousEdgesAreRefused();
  CheckCastPoliciesAreRefusedWithoutOneAccessiblePath();
  CheckPublishedPathsComposeEveryAdjustment();
  CheckInheritedMemberAmbiguityIsCounted();
  CheckPersistentIdentityIsIndependentOfRuntimeTypes();
  CheckDerivedValueReachesEveryRegisteredBase();
  CheckSafeDowncastsRequireAnExplicitPolicy();
  CheckRefusedRelationshipsAreTransactional();
  CheckInheritedMemberAmbiguityIsRefused();
  CheckInheritedViewsRetainDeclarationOwnership();
  CheckAmbiguousInheritedViewsAreReported();
  return FailureCount == 0 ? 0 : 1;
}
