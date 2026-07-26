#pragma once

// Class declarations as canonical plan entries.
//
// One registered class contributes exactly three staged things, and each one is
// described with the same `DescriptorPlanEntry` schema every other category
// uses: the class symbol with its one canonical class type, and the cached
// metatable identity that type owns in this logical State. Both entries join
// the same outermost transaction, the same canonical ordering, the same
// rollback journal, and the same exact stack-restoration guarantees, so a
// failed class registration publishes no type, no symbol, no reflection record,
// no table, and no metatable identity.
//
// The metatable declaration is Luna's own: it is planned under a reserved
// segment of the class scope, contributes no reflection record, and installs no
// virtual-machine value. It exists so the metatable identity of the class is
// captured, validated, journalled, and published inside the transaction rather
// than invented later by the first exposure.

// clang-format off
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_construction.hpp>
#include <luna/binding/instance_receiver.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/reflection/storage.hpp"
#include "state/registration/member_plan.hpp"
#include "state/registration/operator_plan.hpp"
#include "state/registration/plan.hpp"
#include "state/registration/relationship_plan.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

// The reserved segment Luna plans one class metatable under. It is Luna-owned:
// a member declaration of the same name is a reserved-name collision, never a
// replacement of Luna's own metatable identity.
inline constexpr std::string_view ClassMetatableSegment = "__LunaMetatable";

// One staged construction candidate of a staged class: a constructor, a
// factory, or a singleton accessor. It is an ordinary callable candidate plus
// the ownership result it publishes and the canonical identity of the allocator
// policy behind it, so it joins the same overload grouping, the same conversion
// registry, the same transaction, and the same diagnostics every other
// candidate does.
struct StagedConstruction final {
  std::string Segment;
  std::string QualifiedName;
  SymbolKind Kind = SymbolKind::Constructor;
  ConstructionOwnership Ownership = ConstructionOwnership::LuaOwned;
  std::string AllocatorPolicy;

  // The erased candidate is held through a shared owner because a builder plan
  // is read immutably while it is submitted, and the candidate is moved into
  // its canonical plan entry exactly once at that point.
  std::shared_ptr<ErasedCallableDescriptor> Callable;

  // The first deterministic refusal the declaration itself recorded, such as an
  // explicit ownership policy that contradicts the declared result.
  std::string Refusal;

  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;

  [[nodiscard]] bool HasTarget() const noexcept {
    return Callable != nullptr && Callable->HasTarget();
  }
};

// One staged member candidate of a staged class: one instance method, which
// operates on a value of the class, or one static method, which does not.
//
// Both are ordinary callable candidates: the same overload grouping, the same
// parameter descriptors, the same conversion registry, the same transaction,
// and the same diagnostics. An instance method adds exactly one thing to that -
// the receiver it declares, which is rank position zero of its calls - and a
// static method adds nothing at all, which is what routes it through the
// ordinary function pipeline.
struct StagedMethod final {
  std::string Segment;
  std::string QualifiedName;
  SymbolKind Kind = SymbolKind::Method;

  // The candidate declares the object it operates on, and whether it only reads
  // it. A static method declares neither.
  bool DeclaresReceiver = true;
  bool ReceiverIsConst = false;

  // The erased candidate is held through a shared owner for exactly the reason
  // a construction candidate is: a builder plan is read immutably while it is
  // submitted, and the candidate is moved into its canonical plan entry exactly
  // once at that point.
  std::shared_ptr<ErasedCallableDescriptor> Callable;

  // The first deterministic refusal the declaration itself recorded.
  std::string Refusal;

  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;

  [[nodiscard]] bool HasTarget() const noexcept {
    return Callable != nullptr && Callable->HasTarget();
  }
};

// One staged class of a builder plan.
struct StagedClass final {
  std::string Segment;
  std::string QualifiedName;
  StableTypeKey Key;
  ClassPolicy Policy;

  // The semantic storage protocol this class selected for the values Luna
  // creates of it, and whether it selected one at all. The selection belongs to
  // the class, so it applies to every creating candidate of the class whichever
  // order the declarations were made in, and it is what those candidates
  // reflect as their allocator policy identity.
  bool SelectsStorage = false;
  ClassAllocator Storage;

  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;

  // The construction candidates declared inside this class, in declaration
  // order. Canonical ordering is applied when they are planned, never here.
  std::vector<StagedConstruction> Constructions;

  // The instance and static method candidates declared inside this class, in
  // declaration order. Canonical ordering is applied when they are planned.
  std::vector<StagedMethod> Methods;

  // The properties and fields declared inside this class, in declaration order.
  // Each one is one reflected symbol with generated getter and setter
  // descriptors rather than a callable candidate per direction.
  std::vector<StagedMember> Members;

  // The operators declared inside this class, in declaration order. Each one is
  // an ordinary member candidate published under the Luna-owned segment its
  // operator names.
  std::vector<StagedOperator> Operators;

  // The base edges and safe downcast policies declared inside this class, in
  // declaration order. They are only ever accepted as part of the whole
  // candidate graph of the attempt, so nothing about them is decided here.
  PlannedClassRelationships Relationships;
};

// The canonical class type of one staged class.
[[nodiscard]] TypeDescriptor ClassTypeOf(const StagedClass &Declaration);

// One class declaration as a plan entry: the class symbol, its canonical type
// declaration and reflected type, the reflection record of the class, and the
// exact Luna-owned table path the class scope occupies.
[[nodiscard]] DescriptorPlanEntry
MakeClassPlanEntry(const StagedClass &Declaration, SymbolId Parent);

// The metatable identity of one class as a plan entry. It publishes no
// reflection record and installs no virtual-machine value: it carries the
// per-State metatable identity of the class through validation, the journal,
// and publication.
[[nodiscard]] DescriptorPlanEntry
MakeClassMetatablePlanEntry(const StagedClass &Declaration,
                            const SymbolId &ClassSymbol);

// One construction candidate of a class as a plan entry: exactly the canonical
// callable candidate a function declaration produces, parented at the class
// symbol, plus the ownership result and allocator policy identity its
// reflection record carries. The candidate is moved out of `Declaration`, so
// one staged construction contributes one plan entry exactly once.
[[nodiscard]] DescriptorPlanEntry
MakeConstructionPlanEntry(const StagedClass &Class,
                          const StagedConstruction &Declaration,
                          const SymbolId &ClassSymbol);

// One member candidate of a class as a plan entry: exactly the canonical
// callable candidate a function declaration produces, parented at the class
// symbol. An instance method additionally carries its receiver in the canonical
// signature, so it can never reflect, join, or resolve as if it were static.
// The candidate is moved out of `Declaration`, so one staged member contributes
// one plan entry exactly once.
[[nodiscard]] DescriptorPlanEntry
MakeMethodPlanEntry(const StagedClass &Class, const StagedMethod &Declaration,
                    const SymbolId &ClassSymbol);

// The staged construction candidate one member name resolves to, or null when
// the staged class declares none with that name.
[[nodiscard]] StagedConstruction *
FindStagedConstruction(StagedClass &Declaration, std::string_view Segment);

// The staged method candidate one member name resolves to, or null when the
// staged class declares none with that name.
[[nodiscard]] StagedMethod *FindStagedMethod(StagedClass &Declaration,
                                             std::string_view Segment);

// One operator candidate of a class as a plan entry: exactly the canonical
// callable candidate a member declaration produces, parented at the class
// symbol, plus the operator identity publication records with the registered
// class. The candidate is moved out of `Declaration`.
[[nodiscard]] DescriptorPlanEntry
MakeOperatorPlanEntry(const StagedClass &Class,
                      const StagedOperator &Declaration,
                      const SymbolId &ClassSymbol);

// Validates one staged class as a whole and reports the first deterministic
// failure: an invalid stable type key, a storage shape Luna cannot allocate, or
// a class Luna could never destroy.
[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateStagedClass(const StagedClass &Declaration);

// Validates one storage protocol a class selects for the values Luna creates of
// it, and reports the first deterministic failure: a protocol that names
// nothing at all, one without the policy identity its candidates would reflect,
// one missing a step Luna needs to create or release a value, or one whose
// storage does not cover the declared storage shape of the class.
[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateSelectedClassStorage(const StagedClass &Declaration,
                             const ClassAllocator &Storage);

// The allocator policy identity one construction candidate of one class
// reflects: the class's own selection when the candidate creates its object
// through it, and otherwise exactly what the declaration named.
[[nodiscard]] std::string
ReflectedAllocatorPolicy(const StagedClass &Class,
                         const StagedConstruction &Declaration);

// Validates one staged construction candidate and reports the first
// deterministic failure: a refusal the declaration recorded, a missing target,
// or an ownership result the class's declared storage shape cannot honor.
[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateStagedConstruction(const StagedClass &Class,
                           const StagedConstruction &Declaration);

// Validates one staged member candidate and reports the first deterministic
// failure: a refusal the declaration recorded, a missing target, a receiver
// declaration that contradicts the class it was declared in, or one member name
// declared both with and without a receiver.
[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateStagedMethod(const StagedClass &Class, const StagedMethod &Declaration);

} // namespace Luna::Detail
