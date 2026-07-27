#pragma once

// clang-format off
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>

#include "state/registration/plan.hpp"
#include "state/transaction/lifecycle.hpp"
#include "state/type/type_record.hpp"
#include "state/userdata/class_relationships.hpp"
#include "state/userdata/identity.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

class BindingStore;
class ClassRegistry;
class GenerationSet;
class ModuleRegistry;
class NamespaceOwnershipTable;

enum class FreezePreparationStatus {
  Prepared,
  AllocationFailure,
  MissingGeneration,
  InconsistentSymbol,
  InconsistentReflection,
  InconsistentType,
  InconsistentDispatch,
  InconsistentClass,
  InconsistentNamespace,
  InconsistentModule
};

[[nodiscard]] std::string_view
FreezePreparationStatusText(FreezePreparationStatus Status) noexcept;

struct FreezeCacheKey final {
  StateIdentity State;
  std::uint64_t Generation = 0;
  std::uint64_t ReflectionGeneration = 0;
  std::uint64_t TypeGeneration = 0;
  std::uint64_t LifecycleGeneration = 0;
};

struct FrozenLookupEntry final {
  std::string QualifiedName;
  PlanEntryKind Category = PlanEntryKind::Function;
  SymbolKind Kind = SymbolKind::Namespace;
  SymbolId Identity;

  std::optional<std::size_t> ReflectionIndex;
};
struct FrozenOverloadIndex final {
  std::string QualifiedName;
  std::vector<std::size_t> LookupIndices;
};

struct FrozenConversionEntry final {
  TypeId Identity;
  TypeRecord Record;
};

struct FrozenCastPath final {
  TypeId Source;
  TypeId Target;
  ClassConversionKind Kind = ClassConversionKind::Unrelated;
  std::vector<ClassPointerAdjustment> Adjustments;
  std::string Policy;
  bool UsesRuntimeTypeAssistance = false;
  ClassCompatibilityProbe Compatible = nullptr;
  ClassPointerAdjustment Downcast = nullptr;
};

struct FrozenMetatableEntry final {
  TypeId Type;
  SymbolId ClassSymbol;
  MetatableId Metatable;
  std::string QualifiedName;
};

struct FrozenNamespaceEntry final {
  std::string QualifiedName;
  SymbolId Scope;
};

struct FrozenModuleEntry final {
  std::string Identity;
  std::string Version;
  SymbolId Symbol;
  ModuleManifest Manifest;
};

class FreezeCacheStorage final {
public:
  [[nodiscard]] static FreezePreparationStatus
  Prepare(const GenerationSet &Generations, StateIdentity State,
          std::uint64_t LifecycleGeneration, const BindingStore &Bindings,
          const NamespaceOwnershipTable &Namespaces,
          const ClassRegistry &Classes, const ModuleRegistry &Modules,
          std::shared_ptr<const FreezeCacheStorage> &Prepared);

  [[nodiscard]] const FreezeCacheKey &Key() const noexcept { return CacheKey; }
  [[nodiscard]] std::size_t LookupCount() const noexcept {
    return Lookups.size();
  }
  [[nodiscard]] std::size_t OverloadCount() const noexcept {
    return Overloads.size();
  }
  [[nodiscard]] std::size_t ConversionCount() const noexcept {
    return Conversions.size();
  }
  [[nodiscard]] std::size_t CastPathCount() const noexcept {
    return CastPaths.size();
  }
  [[nodiscard]] std::size_t MetatableCount() const noexcept {
    return Metatables.size();
  }
  [[nodiscard]] std::size_t NamespaceCount() const noexcept {
    return NamespaceEntries.size();
  }
  [[nodiscard]] std::size_t ModuleCount() const noexcept {
    return ModuleEntries.size();
  }
  [[nodiscard]] const std::vector<FrozenLookupEntry> &
  SortedLookups() const noexcept {
    return Lookups;
  }
  [[nodiscard]] const std::vector<FrozenOverloadIndex> &
  OverloadIndices() const noexcept {
    return Overloads;
  }
  [[nodiscard]] const std::vector<FrozenConversionEntry> &
  ConversionTable() const noexcept {
    return Conversions;
  }
  [[nodiscard]] const std::vector<FrozenCastPath> &
  ClassCastPaths() const noexcept {
    return CastPaths;
  }
  [[nodiscard]] const std::vector<FrozenMetatableEntry> &
  MetatableMap() const noexcept {
    return Metatables;
  }
  [[nodiscard]] const std::vector<FrozenNamespaceEntry> &
  NamespaceCache() const noexcept {
    return NamespaceEntries;
  }
  [[nodiscard]] const std::vector<FrozenModuleEntry> &
  ModuleCache() const noexcept {
    return ModuleEntries;
  }

private:
  FreezeCacheKey CacheKey;
  std::vector<FrozenLookupEntry> Lookups;
  std::vector<FrozenOverloadIndex> Overloads;
  std::vector<FrozenConversionEntry> Conversions;
  std::vector<FrozenCastPath> CastPaths;
  std::vector<FrozenMetatableEntry> Metatables;
  std::vector<FrozenNamespaceEntry> NamespaceEntries;
  std::vector<FrozenModuleEntry> ModuleEntries;
};

} // namespace Luna::Detail
