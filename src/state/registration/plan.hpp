#pragma once

// One canonical descriptor plan. Every registration category submits the same
// entry schema: a function candidate, a scope, a canonical type, a reflection
// record, a module, a dispatch target, a metatable, and the class symbols of a
// later milestone all describe themselves through one `DescriptorPlanEntry`.
// The plan is the pending half of registration; the committed half is the
// current immutable generation set. Both halves expose the same canonical
// schema, so validation never maintains a competing model.

// clang-format off
#include <luna/binding/callable_descriptor.hpp>
#include <luna/binding/callable_metadata.hpp>
#include <luna/binding/class_builder.hpp>
#include <luna/binding/class_member.hpp>
#include <luna/binding/class_operator.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/identity/symbol_descriptor.hpp"
#include "state/reflection/storage.hpp"
#include "state/registration/relationship_plan.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/type_record.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

// Category of one planned declaration. The category selects which optional
// payloads an entry carries; it never changes the canonical identity schema.
enum class PlanEntryKind {
  Function,
  Scope,
  // One installed virtual-machine value that is not a callable and not a scope:
  // a constant today, and every later category that publishes one converted
  // value at an exact canonical path.
  Value,
  Type,
  ReflectionRecord,
  Module,
  DispatchTarget,
  Metatable,
  ClassSymbol,
  ClassMember
};

[[nodiscard]] std::string_view
PlanEntryKindText(PlanEntryKind Category) noexcept;

// One staged virtual-machine value of a declaration: the canonical type it is
// converted through and the complete Luna-owned staged value. Nothing is
// converted before installation, and installation converts through the
// registered writer of exactly this type.
struct PlannedValue final {
  TypeDescriptor Type;
  StructuredValue Staged;
};

// One named field of a planned immutable table.
struct PlannedValueField final {
  std::string Name;
  StructuredValue Staged;
};

// A planned immutable table: every field converts through one canonical type,
// and the installed table is a Luna-owned proxy whose backing storage no script
// can reach, so a supported script write fails deterministically instead of
// mutating raw public storage.
struct PlannedValueTable final {
  TypeDescriptor Type;
  std::vector<PlannedValueField> Fields;
};

// One property or field of a registered class, as the plan carries it: the
// category and policies its reflection record already describes, the canonical
// identity of its declared value type, and the two generated descriptors
// publication hands to the registered class. Nothing here installs a
// virtual-machine value: a member is reached through its class, so it becomes
// reachable only when the transaction publishes.
struct PlannedClassMember final {
  SymbolKind Kind = SymbolKind::Property;
  MemberAccess Access = MemberAccess::ReadOnly;
  PropertyEvaluation Evaluation = PropertyEvaluation::Immediate;
  MemberOwnership Ownership = MemberOwnership::Copied;
  TypeId ValueType;

  // The complete canonical descriptor of that declared value type. Publication
  // records it with the member so a refused write can name the type the member
  // declares through the registry's own public name.
  TypeDescriptor ValueDescriptor;

  bool ReadRequiresMutableReceiver = false;
  MemberReadOperation Read;
  MemberWriteOperation Write;
};

// One operator of a registered class, as the plan carries it: which operator
// the declaration selected and the Luna-owned member segment its candidate is
// published under. Publication records both with the registered class so the
// class metatable can name the candidate without holding it.
struct PlannedClassOperator final {
  ClassOperator Selected = ClassOperator::Call;
  std::string Segment;
};

// One planned declaration. `Symbol` is the complete canonical descriptor the
// identity of the entry derives from, `VmPath` is the canonical dot-separated
// virtual-machine path the installer touches, and every optional payload is
// present only for the categories that require it.
struct DescriptorPlanEntry final {
  PlanEntryKind Category = PlanEntryKind::Function;
  SymbolDescriptor Symbol;
  SymbolId Identity;
  std::string VmPath;
  std::optional<ReflectionRecordFields> Record;

  // The overload set one callable candidate belongs to. Every candidate names
  // its set, but only the declaration that opens the set carries the set's own
  // record, so a candidate joining a published or pending set never publishes a
  // second record for one qualified name.
  std::optional<ReflectionRecordFields> OverloadSetRecord;

  std::optional<ReflectionTypeFields> TypeFields;
  std::optional<ReflectionModuleFields> ModuleFields;

  // The canonical identity of the module whose load contributed this
  // declaration, empty for a declaration no module load owns. Preparation
  // resolves it to the module record of the candidate generation, which is how
  // every symbol a module publishes reports the module identity and version it
  // came from. The entry never carries the module index itself, because the
  // canonical index of a module is only known once the whole candidate
  // generation is assembled.
  std::string ModuleIdentity;

  // The declared C++ storage shape of one class declaration. Only a class
  // carries it, and publication records it with the registered class, because
  // the backend never sees the consumer's type.
  std::optional<ClassPolicy> ClassStorage;

  // The canonical type record a type declaration contributes to the next
  // immutable type generation. A type entry that only reflects an already
  // declared type carries none.
  std::optional<TypeRecord> TypeConversion;

  std::optional<ErasedCallableDescriptor> Callable;
  std::size_t DispatchSlot = 0;

  // The one converted value this declaration installs at its canonical path.
  std::optional<PlannedValue> InstalledValue;

  // The immutable table this declaration installs at its canonical path.
  std::optional<PlannedValueTable> InstalledTable;

  // The property or field this declaration adds to its class. Only a class
  // member carries it, and publication records it with the registered class,
  // because the backend never sees the consumer's accessors.
  std::optional<PlannedClassMember> ClassMember;

  // The base edges and safe downcast policies one class declaration contributes
  // to the relationship graph of its State. Only a class symbol carries them,
  // and publication records them after every class of the attempt is
  // registered.
  std::optional<PlannedClassRelationships> Relationships;

  // The operator this candidate answers. Only an operator candidate carries it.
  std::optional<PlannedClassOperator> OperatorFields;

  DescriptorPlanEntry() = default;

  DescriptorPlanEntry(const DescriptorPlanEntry &) = delete;
  DescriptorPlanEntry &operator=(const DescriptorPlanEntry &) = delete;
  DescriptorPlanEntry(DescriptorPlanEntry &&) noexcept = default;
  DescriptorPlanEntry &operator=(DescriptorPlanEntry &&) noexcept = default;
  ~DescriptorPlanEntry() = default;

  // An entry is complete when its canonical descriptor is complete and every
  // payload its category requires is present.
  [[nodiscard]] bool IsValid() const;
};

// Canonical signature of one foundation callable. The foundation accepts only
// required scalar parameters and a value or void return, so the canonical
// signature is fully determined by the callable's metadata.
[[nodiscard]] CallableSignatureDescriptor
CanonicalFoundationSignature(const CallableMetadata &Metadata);

// One function declaration as a plan entry: one callable candidate whose
// virtual-machine path is its canonical qualified name. A root-scope `Register`
// or `RegisterFunction` request names the root scope as its parent, and a
// scoped `RegisterFunction` request names its namespace, so both spellings
// share one canonical descriptor builder.
[[nodiscard]] DescriptorPlanEntry
MakeFunctionPlanEntry(std::string QualifiedName,
                      ErasedCallableDescriptor Callable,
                      SymbolId Parent = SymbolId());

// Copies the declared documentation surface of one declaration - its
// documentation text, its attributes, and its usage examples - onto the
// reflection record of one planned entry. A category whose plan-entry builder
// already reads its staged declaration copies them there instead; this is the
// shared path for the categories whose canonical descriptor builder is the same
// one root-scope registration uses and therefore takes no annotations.
void ApplyDeclaredAnnotations(DescriptorPlanEntry &Entry,
                              std::string_view Documentation,
                              std::vector<ReflectionAttributeFields> Attributes,
                              std::vector<std::string> Examples);

// Drops the overload-set record of one planned candidate, because the set it
// joins is already described by the captured generation or by an earlier
// declaration of the same plan.
void JoinPlannedOverloadSet(DescriptorPlanEntry &Entry) noexcept;

// One canonical type declaration as a plan entry: the reflected type fields and
// the canonical type record whose converters the next type generation adopts.
[[nodiscard]] DescriptorPlanEntry MakeTypePlanEntry(std::string QualifiedName,
                                                    TypeRecord Type);

class DescriptorPlan final {
public:
  DescriptorPlan() = default;

  DescriptorPlan(const DescriptorPlan &) = delete;
  DescriptorPlan &operator=(const DescriptorPlan &) = delete;
  DescriptorPlan(DescriptorPlan &&) noexcept = default;
  DescriptorPlan &operator=(DescriptorPlan &&) noexcept = default;
  ~DescriptorPlan() = default;

  std::size_t Append(DescriptorPlanEntry Entry);

  [[nodiscard]] std::size_t Size() const noexcept { return Entries.size(); }
  [[nodiscard]] bool IsEmpty() const noexcept { return Entries.empty(); }

  [[nodiscard]] DescriptorPlanEntry *At(std::size_t Index) noexcept;
  [[nodiscard]] const DescriptorPlanEntry *At(std::size_t Index) const noexcept;

  [[nodiscard]] std::span<const DescriptorPlanEntry>
  PlannedEntries() const noexcept {
    return Entries;
  }

  [[nodiscard]] const DescriptorPlanEntry *
  Find(std::string_view QualifiedName) const noexcept;
  [[nodiscard]] const DescriptorPlanEntry *
  Find(const SymbolId &Identity) const noexcept;
  [[nodiscard]] bool Contains(std::string_view QualifiedName) const noexcept;

  [[nodiscard]] std::size_t CountOf(PlanEntryKind Category) const noexcept;

  // Canonical order of the planned entries: qualified name, symbol kind,
  // canonical signature, and finally symbol identity. Insertion order never
  // participates.
  [[nodiscard]] std::vector<std::size_t> CanonicalOrder() const;

  void Clear() noexcept { Entries.clear(); }

private:
  std::vector<DescriptorPlanEntry> Entries;
};

// True when a plan contributes reflection content - a record, a canonical type,
// or a module. A plan that contributes none describes the same reflection
// generation it captured, so publication has no new generation to publish.
[[nodiscard]] bool
PlanContributesReflection(const DescriptorPlan &Plan) noexcept;

// Number of planned entries that contribute one reflection record.
[[nodiscard]] std::size_t
PlannedReflectionRecordCount(const DescriptorPlan &Plan) noexcept;

// Canonical precedence of two planned or committed declarations.
[[nodiscard]] bool PlanEntryPrecedes(const SymbolDescriptor &Left,
                                     const SymbolId &LeftIdentity,
                                     const SymbolDescriptor &Right,
                                     const SymbolId &RightIdentity);

} // namespace Luna::Detail
