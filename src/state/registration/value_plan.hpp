#pragma once

// Constant and enumeration declarations as canonical plan entries.
//
// A constant is one reflected symbol plus one converted virtual-machine value
// at an exact canonical path. An enumeration is one reflected scope plus one
// Luna-owned immutable table, one reflected enumerator per canonical value, and
// one reflected alias per additional name. Both categories submit the same
// `DescriptorPlanEntry` schema every other category uses, so they join the same
// outermost transaction, the same canonical ordering, the same rollback
// journal, and the same exact stack-restoration guarantees.
//
// Everything a builder collects is staged here first and validated as a whole:
// names, alias targets, duplicate values, declared supported bits, the declared
// C++ underlying range, and the exact-integer domain Luna converts through. A
// staged enumeration that fails any of those checks contributes no plan entry
// at all, so nothing partial can reach installation.

// clang-format off
#include <luna/binding/constant_value.hpp>
#include <luna/binding/enum_builder.hpp>
#include <luna/binding/value.hpp>
#include <luna/core/diagnostics/error_diagnostic.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/stable_type_key.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/module/load.hpp"
#include "state/reflection/storage.hpp"
#include "state/registration/class_plan.hpp"
#include "state/registration/function_plan.hpp"
#include "state/registration/plan.hpp"
#include "state/registration/scope_plan.hpp"
#include "state/type/type_record.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

// One staged constant of a builder plan.
struct StagedConstant final {
  std::string Segment;
  std::string QualifiedName;
  ConstantRequest Request;
  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;
};

// One staged enumerator or alias of a staged enumeration.
struct StagedEnumerator final {
  std::string Segment;
  std::int64_t Numeric = 0;

  // An alias adds one name for the canonical enumerator it targets; it never
  // contributes a second canonical value.
  bool IsAlias = false;
  std::string CanonicalSegment;

  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;
};

// One staged enumeration of a builder plan.
struct StagedEnumeration final {
  std::string Segment;
  std::string QualifiedName;
  StableTypeKey Key;
  EnumerationPolicy Policy;

  // Unscoped exposure and bitflag behavior are opt-ins, never inferences.
  bool UnscopedIsAllowed = false;
  bool IsBitflags = false;
  bool HasDeclaredMask = false;
  std::int64_t SupportedBits = 0;

  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;
  std::vector<StagedEnumerator> Enumerators;
};

// One complete pending plan of a builder chain. A staged module load runs its
// callbacks when the plan is submitted, so a module requested inside a builder
// joins exactly the builder's outermost transaction.
struct BuilderPlan final {
  std::vector<StagedNamespace> Namespaces;
  std::vector<StagedFunction> Functions;
  std::vector<StagedConstant> Constants;
  std::vector<StagedEnumeration> Enumerations;
  std::vector<StagedClass> Classes;
  std::vector<StagedModule> Modules;
};

// Canonical text of one staged value, used by reflection and by generated
// artifacts. It never uses locale-sensitive formatting.
[[nodiscard]] std::string CanonicalValueText(const Value &Staged);

// One constant declaration as a plan entry: the constant symbol, the exact
// reflected path, its reflection record including value availability, and the
// staged value installation converts through the canonical type's writer.
[[nodiscard]] DescriptorPlanEntry
MakeConstantPlanEntry(const StagedConstant &Declaration, SymbolId Parent,
                      const TypeId &Type);

// The canonical enumerator domain of one validated staged enumeration.
[[nodiscard]] EnumerationDomain
MakeEnumerationDomain(const StagedEnumeration &Declaration);

// One enumeration declaration as a plan entry: the enumeration scope symbol,
// its canonical type declaration and reflected type, the exact reflected table
// path, and the immutable table installation publishes.
[[nodiscard]] DescriptorPlanEntry
MakeEnumerationPlanEntry(const StagedEnumeration &Declaration, SymbolId Parent);

// One enumerator or alias as a plan entry. An alias record keeps the canonical
// enumerator as its declaration, so reflection never loses which name is
// canonical.
[[nodiscard]] DescriptorPlanEntry
MakeEnumeratorPlanEntry(const StagedEnumeration &Declaration,
                        const StagedEnumerator &Enumerator,
                        const SymbolId &Enumeration, const TypeId &Type,
                        const SymbolId &CanonicalEnumerator);

// Validates one staged enumeration as a whole and reports the first
// deterministic failure: an empty enumerator set, an unscoped enumeration
// without its opt-in, a duplicate name, a duplicate value without an explicit
// alias, an unknown alias target, a value outside the declared underlying or
// canonical integer range, or a bitflag value outside the declared mask.
[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateStagedEnumeration(const StagedEnumeration &Declaration);

// The canonical enumerator one staged name resolves to, or null when the staged
// enumeration does not declare it.
[[nodiscard]] const StagedEnumerator *
FindCanonicalEnumerator(const StagedEnumeration &Declaration,
                        std::string_view Segment);

} // namespace Luna::Detail
