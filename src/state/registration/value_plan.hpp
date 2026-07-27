#pragma once

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

struct StagedConstant final {
  std::string Segment;
  std::string QualifiedName;
  ConstantRequest Request;
  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;
};

struct StagedEnumerator final {
  std::string Segment;
  std::int64_t Numeric = 0;

  bool IsAlias = false;
  std::string CanonicalSegment;

  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;
};

struct StagedEnumeration final {
  std::string Segment;
  std::string QualifiedName;
  StableTypeKey Key;
  EnumerationPolicy Policy;

  bool UnscopedIsAllowed = false;
  bool IsBitflags = false;
  bool HasDeclaredMask = false;
  std::int64_t SupportedBits = 0;

  std::string Documentation;
  std::vector<ReflectionAttributeFields> Attributes;
  std::vector<std::string> Examples;
  std::vector<StagedEnumerator> Enumerators;
};

struct BuilderPlan final {
  std::vector<StagedNamespace> Namespaces;
  std::vector<StagedFunction> Functions;
  std::vector<StagedConstant> Constants;
  std::vector<StagedEnumeration> Enumerations;
  std::vector<StagedClass> Classes;
  std::vector<StagedModule> Modules;
};

[[nodiscard]] std::string CanonicalValueText(const Value &Staged);

[[nodiscard]] DescriptorPlanEntry
MakeConstantPlanEntry(const StagedConstant &Declaration, SymbolId Parent,
                      const TypeId &Type);

[[nodiscard]] EnumerationDomain
MakeEnumerationDomain(const StagedEnumeration &Declaration);

[[nodiscard]] DescriptorPlanEntry
MakeEnumerationPlanEntry(const StagedEnumeration &Declaration, SymbolId Parent);

[[nodiscard]] DescriptorPlanEntry
MakeEnumeratorPlanEntry(const StagedEnumeration &Declaration,
                        const StagedEnumerator &Enumerator,
                        const SymbolId &Enumeration, const TypeId &Type,
                        const SymbolId &CanonicalEnumerator);

[[nodiscard]] std::optional<ErrorDiagnostic>
ValidateStagedEnumeration(const StagedEnumeration &Declaration);

[[nodiscard]] const StagedEnumerator *
FindCanonicalEnumerator(const StagedEnumeration &Declaration,
                        std::string_view Segment);

} // namespace Luna::Detail
