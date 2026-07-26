// clang-format off
#include "state/freeze/cache.hpp"

#include "state/identity/identity_registry.hpp"
#include "state/reflection/storage.hpp"
#include "state/registration/record.hpp"
#include "state/registration/scope_plan.hpp"
#include "state/registration/store.hpp"
#include "state/transaction/generation_set.hpp"
#include "state/type/type_generation.hpp"
#include "state/userdata/class_registry.hpp"
#include "state/module/registry.hpp"

#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] bool RequiresReflectedRecord(PlanEntryKind Category) noexcept {
  switch (Category) {
  case PlanEntryKind::Function:
  case PlanEntryKind::Scope:
  case PlanEntryKind::Value:
  case PlanEntryKind::ReflectionRecord:
  case PlanEntryKind::Module:
  case PlanEntryKind::ClassSymbol:
  case PlanEntryKind::ClassMember:
    return true;
  case PlanEntryKind::Type:
  case PlanEntryKind::DispatchTarget:
  case PlanEntryKind::Metatable:
    return false;
  }
  return false;
}

// Two canonical shapes the type registry deliberately never describes: the
// owning public value and value pack belong to the custom conversion boundary,
// so a variadic tail or a dynamic return pack reflects them without ever
// declaring a convertible type for them.
[[nodiscard]] bool IsBoundaryOwnedShape(const TypeDescriptor &Type) noexcept {
  const std::optional<FixedTypeKey> Fixed = Type.FixedKey();
  return Fixed &&
         (*Fixed == FixedTypeKey::Value || *Fixed == FixedTypeKey::ValuePack);
}

// An ordered pack publishes one value per element rather than one aggregate,
// which is exactly what registration validates availability for.
[[nodiscard]] bool IsOrderedPackShape(const TypeDescriptor &Type) noexcept {
  switch (Type.Kind()) {
  case TypeKind::Pair:
  case TypeKind::Tuple:
  case TypeKind::ArgumentPack:
  case TypeKind::ReturnPack:
    return true;
  default:
    return false;
  }
}

// Whether one reflected descriptor resolves against the exact type generation
// frozen with it. A descriptor the registry describes must agree with its
// record; a descriptor the canonical model keeps outside the registry resolves
// through the types it publishes instead, so freeze applies the same rule
// registration applied when it accepted the declaration.
[[nodiscard]] bool ResolvesShape(const TypeGeneration &Types,
                                 const TypeDescriptor &Type) {
  if (!Type.IsValid())
    return false;
  if (const TypeRecord *Record = Types.Find(Type))
    return Record->Descriptor == Type;
  if (IsBoundaryOwnedShape(Type))
    return true;
  if (!IsOrderedPackShape(Type))
    return false;
  for (const TypeDescriptor &Element : Type.Children()) {
    if (!ResolvesShape(Types, Element))
      return false;
  }
  return true;
}

[[nodiscard]] bool HasType(const TypeGeneration &Types, const TypeId &Identity,
                           const TypeDescriptor *Descriptor = nullptr) {
  const bool Described = Descriptor != nullptr && Descriptor->IsValid();
  if (!Identity.IsValid())
    return !Described;
  if (const TypeRecord *Record = Types.Find(Identity))
    return !Described || Record->Descriptor == *Descriptor;

  // No registry entry: the reference may still name one shape outside the
  // registry, but only when it carries the descriptor that produced its
  // identity and every type that shape publishes resolves.
  if (!Described)
    return false;
  const std::optional<TypeId> Computed =
      TypeIdentityRegistry::ComputeIdentity(*Descriptor);
  return Computed && *Computed == Identity && ResolvesShape(Types, *Descriptor);
}

[[nodiscard]] bool ValidateRecordTypes(const ReflectionRecordFields &Record,
                                       const TypeGeneration &Types) {
  if (!HasType(Types, Record.Type, &Record.Descriptor) ||
      !HasType(Types, Record.ReceiverType))
    return false;
  for (const ReflectionParameterFields &Parameter : Record.Parameters) {
    if (!HasType(Types, Parameter.Type, &Parameter.Descriptor))
      return false;
  }
  for (const ReflectionReturnFields &Returned : Record.ReturnValues) {
    if (!HasType(Types, Returned.Type, &Returned.Descriptor))
      return false;
  }
  for (const ReflectionRelationFields &Relation : Record.Relations) {
    if (!HasType(Types, Relation.Type))
      return false;
  }
  return true;
}
[[nodiscard]] std::size_t LookupIndexOf(const FreezeCacheStorage &Cache,
                                        const SymbolId &Identity) {
  const auto &Lookups = Cache.SortedLookups();
  for (std::size_t Index = 0; Index < Lookups.size(); ++Index) {
    if (Lookups[Index].Identity == Identity)
      return Index;
  }
  return Lookups.size();
}

[[nodiscard]] const ReflectionModuleFields *
FindReflectedModule(const ReflectionStorage &Reflection,
                    std::string_view Identity) noexcept {
  for (std::size_t Index = 0; Index < Reflection.ModuleCount(); ++Index) {
    const ReflectionModuleFields *Module = Reflection.ModuleAt(Index);
    if (Module != nullptr && Module->Identity == Identity)
      return Module;
  }
  return nullptr;
}

[[nodiscard]] bool MetatablePrecedes(const FrozenMetatableEntry &Left,
                                     const FrozenMetatableEntry &Right) {
  if (Left.QualifiedName != Right.QualifiedName)
    return Left.QualifiedName < Right.QualifiedName;
  return Left.Type < Right.Type;
}

[[nodiscard]] bool CastPathPrecedes(const FrozenCastPath &Left,
                                    const FrozenCastPath &Right) {
  if (Left.Source != Right.Source)
    return Left.Source < Right.Source;
  if (Left.Target != Right.Target)
    return Left.Target < Right.Target;
  return Left.Kind < Right.Kind;
}

} // namespace

std::string_view
FreezePreparationStatusText(FreezePreparationStatus Status) noexcept {
  switch (Status) {
  case FreezePreparationStatus::Prepared:
    return "prepared";
  case FreezePreparationStatus::AllocationFailure:
    return "allocation_failure";
  case FreezePreparationStatus::MissingGeneration:
    return "missing_generation";
  case FreezePreparationStatus::InconsistentSymbol:
    return "inconsistent_symbol";
  case FreezePreparationStatus::InconsistentReflection:
    return "inconsistent_reflection";
  case FreezePreparationStatus::InconsistentType:
    return "inconsistent_type";
  case FreezePreparationStatus::InconsistentDispatch:
    return "inconsistent_dispatch";
  case FreezePreparationStatus::InconsistentClass:
    return "inconsistent_class";
  case FreezePreparationStatus::InconsistentNamespace:
    return "inconsistent_namespace";
  case FreezePreparationStatus::InconsistentModule:
    return "inconsistent_module";
  }
  return "unknown";
}

FreezePreparationStatus FreezeCacheStorage::Prepare(
    const GenerationSet &Generations, StateIdentity State,
    std::uint64_t LifecycleGeneration, const BindingStore &Bindings,
    const NamespaceOwnershipTable &Namespaces, const ClassRegistry &Classes,
    const ModuleRegistry &Modules,
    std::shared_ptr<const FreezeCacheStorage> &Prepared) {
  Prepared.reset();
  const std::shared_ptr<const ReflectionStorage> Reflection =
      Generations.Reflection();
  const std::shared_ptr<const TypeGeneration> Types = Generations.Types();
  if (!State.IsValid() || !Reflection || !Types)
    return FreezePreparationStatus::MissingGeneration;

  try {
    std::shared_ptr<FreezeCacheStorage> Cache(new FreezeCacheStorage());
    Cache->CacheKey.State = State;
    Cache->CacheKey.Generation = Generations.Generation();
    Cache->CacheKey.ReflectionGeneration = Reflection->Generation();
    Cache->CacheKey.TypeGeneration = Types->Generation();
    Cache->CacheKey.LifecycleGeneration = LifecycleGeneration;

    // The committed table is already canonical. Copying its order gives the
    // frozen lookup array stable indices owned entirely by this cache.
    for (std::size_t Index = 0; Index < Generations.Symbols().Size(); ++Index) {
      const CommittedSymbol *Symbol = Generations.Symbols().At(Index);
      if (Symbol == nullptr || !Symbol->IsValid())
        return FreezePreparationStatus::InconsistentSymbol;
      const auto RecordIndex = Reflection->IndexOf(Symbol->Identity);
      const ReflectionRecordFields *Record =
          RecordIndex ? Reflection->RecordAt(*RecordIndex) : nullptr;
      if (RequiresReflectedRecord(Symbol->Category)) {
        if (Record == nullptr || Record->Kind != Symbol->Symbol.Kind ||
            Record->QualifiedName != Symbol->Symbol.QualifiedName)
          return FreezePreparationStatus::InconsistentReflection;
      } else if (Record != nullptr) {
        // A private identity cannot silently alias a public reflected record.
        return FreezePreparationStatus::InconsistentReflection;
      }
      Cache->Lookups.push_back(FrozenLookupEntry{
          Symbol->Symbol.QualifiedName, Symbol->Category, Symbol->Symbol.Kind,
          Symbol->Identity, RecordIndex});
    }
    // Reflection may contain synthetic overload-set records in addition to the
    // committed candidates. Every type reference still has to resolve against
    // the exact type generation frozen with it.
    for (std::size_t Index = 0; Index < Reflection->RecordCount(); ++Index) {
      const ReflectionRecordFields *Record = Reflection->RecordAt(Index);
      if (Record == nullptr || !ValidateRecordTypes(*Record, *Types))
        return FreezePreparationStatus::InconsistentType;
      if (!Record->Scope.IsRoot() &&
          !Reflection->IndexOf(Record->Scope.Owner()).has_value())
        return FreezePreparationStatus::InconsistentReflection;
      if (Record->OverloadSet.IsValid()) {
        const auto SetIndex = Reflection->IndexOf(Record->OverloadSet);
        const ReflectionRecordFields *Set =
            SetIndex ? Reflection->RecordAt(*SetIndex) : nullptr;
        if (Set == nullptr || Set->Kind != SymbolKind::OverloadSet)
          return FreezePreparationStatus::InconsistentReflection;
      }
      if (Record->Module && *Record->Module >= Reflection->ModuleCount())
        return FreezePreparationStatus::InconsistentModule;
    }

    for (std::size_t Index = 0; Index < Reflection->TypeCount(); ++Index) {
      const ReflectionTypeFields *Reflected = Reflection->TypeAt(Index);
      if (Reflected == nullptr ||
          !HasType(*Types, Reflected->Id, &Reflected->Descriptor))
        return FreezePreparationStatus::InconsistentType;
    }

    for (std::size_t Index = 0; Index < Types->Size(); ++Index) {
      const TypeRecord *Record = Types->At(Index);
      if (Record == nullptr || !Record->IsComplete())
        return FreezePreparationStatus::InconsistentType;
      Cache->Conversions.push_back(
          FrozenConversionEntry{Record->Identity, *Record});
    }

    // One immutable overload index per callable path. Candidate order is the
    // canonical order already used by dispatch, and every index points into the
    // cache-owned lookup array above.
    std::vector<std::string> CallablePaths;
    for (const FrozenLookupEntry &Lookup : Cache->Lookups) {
      if (Lookup.Category == PlanEntryKind::Function)
        CallablePaths.push_back(Lookup.QualifiedName);
    }
    std::sort(CallablePaths.begin(), CallablePaths.end());
    CallablePaths.erase(std::unique(CallablePaths.begin(), CallablePaths.end()),
                        CallablePaths.end());
    if (CallablePaths.size() != Bindings.Count())
      return FreezePreparationStatus::InconsistentDispatch;
    for (const std::string &Path : CallablePaths) {
      const BindingRecord *Record = Bindings.Find(Path);
      if (Record == nullptr || !Record->IsCommitted() ||
          Record->HasStagedCandidate())
        return FreezePreparationStatus::InconsistentDispatch;
      FrozenOverloadIndex Indexed;
      Indexed.QualifiedName = Path;
      for (std::size_t Position = 0; Position < Record->CandidateCount();
           ++Position) {
        const OverloadCandidate *Overload = Record->CandidateAt(Position);
        if (Overload == nullptr || !Overload->IsCommitted)
          return FreezePreparationStatus::InconsistentDispatch;
        const std::size_t Lookup = LookupIndexOf(*Cache, Overload->Identity);
        if (Lookup == Cache->LookupCount())
          return FreezePreparationStatus::InconsistentDispatch;
        const CommittedSymbol *Symbol =
            Generations.Symbols().Find(Overload->Identity);
        if (Symbol == nullptr || !Symbol->Symbol.Signature ||
            !(*Symbol->Symbol.Signature == Overload->Signature) ||
            Symbol->VmPath != Path)
          return FreezePreparationStatus::InconsistentDispatch;
        Indexed.LookupIndices.push_back(Lookup);
      }
      Cache->Overloads.push_back(std::move(Indexed));
    }

    const ClassRelationships &Relationships = Classes.Relationships();
    if (Relationships.NodeCount() != Classes.Size())
      return FreezePreparationStatus::InconsistentClass;
    for (const RegisteredClass &Registered : Classes.Registered()) {
      if (!Registered.IsComplete() ||
          !ClassRegistry::Matches(Registered, State, Registered.Type,
                                  Registered.ClassSymbol) ||
          !ClassRegistry::IsCurrent(Registered, LifecycleGeneration) ||
          Types->Find(Registered.Type) == nullptr ||
          !Relationships.Contains(Registered.Type) ||
          Relationships.MetatableOf(Registered.Type) != Registered.Metatable)
        return FreezePreparationStatus::InconsistentClass;
      const CommittedSymbol *Symbol =
          Generations.Symbols().Find(Registered.ClassSymbol);
      const auto ReflectionIndex = Reflection->IndexOf(Registered.ClassSymbol);
      const ReflectionRecordFields *Reflected =
          ReflectionIndex ? Reflection->RecordAt(*ReflectionIndex) : nullptr;
      if (Symbol == nullptr || Symbol->Category != PlanEntryKind::ClassSymbol ||
          Reflected == nullptr || Reflected->Kind != SymbolKind::Class ||
          Reflected->Type != Registered.Type ||
          Registered.QualifiedName != Reflected->QualifiedName)
        return FreezePreparationStatus::InconsistentClass;
      for (const FrozenMetatableEntry &Existing : Cache->Metatables) {
        if (Existing.Type == Registered.Type ||
            Existing.Metatable == Registered.Metatable)
          return FreezePreparationStatus::InconsistentClass;
      }
      Cache->Metatables.push_back(
          FrozenMetatableEntry{Registered.Type, Registered.ClassSymbol,
                               Registered.Metatable, Registered.QualifiedName});
    }
    std::sort(Cache->Metatables.begin(), Cache->Metatables.end(),
              MetatablePrecedes);

    for (const RegisteredClass &Source : Classes.Registered()) {
      for (const RegisteredClass &Target : Classes.Registered()) {
        if (Source.Type == Target.Type)
          continue;
        const ClassConversion Resolved =
            Relationships.Resolve(Source.Type, Target.Type);
        if (!Resolved.IsViable())
          continue;
        FrozenCastPath Path;
        Path.Source = Source.Type;
        Path.Target = Target.Type;
        Path.Kind = Resolved.Kind;
        if (Resolved.Kind == ClassConversionKind::Upcast) {
          if (Resolved.Path == nullptr || Resolved.Path->Adjustments.empty())
            return FreezePreparationStatus::InconsistentClass;
          Path.Adjustments = Resolved.Path->Adjustments;
        } else if (Resolved.Kind == ClassConversionKind::SafeDowncast) {
          if (Resolved.Cast == nullptr ||
              Resolved.Cast->Compatible == nullptr ||
              Resolved.Cast->Downcast == nullptr)
            return FreezePreparationStatus::InconsistentClass;
          Path.Policy = Resolved.Cast->Policy;
          Path.UsesRuntimeTypeAssistance =
              Resolved.Cast->UsesRuntimeTypeAssistance;
          Path.Compatible = Resolved.Cast->Compatible;
          Path.Downcast = Resolved.Cast->Downcast;
        }
        Cache->CastPaths.push_back(std::move(Path));
      }
    }
    std::sort(Cache->CastPaths.begin(), Cache->CastPaths.end(),
              CastPathPrecedes);

    for (const NamespaceOwnership &Owned : Namespaces.All()) {
      if (!NamespaceOwnershipTable::Matches(Owned, State, Owned.QualifiedName,
                                            Owned.Scope, Owned.Table) ||
          !NamespaceOwnershipTable::IsCurrent(Owned, LifecycleGeneration))
        return FreezePreparationStatus::InconsistentNamespace;
      const CommittedSymbol *Symbol = Generations.Symbols().Find(Owned.Scope);
      const auto ReflectionIndex = Reflection->IndexOf(Owned.Scope);
      const ReflectionRecordFields *Reflected =
          ReflectionIndex ? Reflection->RecordAt(*ReflectionIndex) : nullptr;
      if (Symbol == nullptr || Symbol->Category != PlanEntryKind::Scope ||
          Symbol->Symbol.QualifiedName != Owned.QualifiedName ||
          Reflected == nullptr || Reflected->Kind != SymbolKind::Namespace ||
          Reflected->QualifiedName != Owned.QualifiedName)
        return FreezePreparationStatus::InconsistentNamespace;
      Cache->NamespaceEntries.push_back(
          FrozenNamespaceEntry{Owned.QualifiedName, Owned.Scope});
    }
    std::sort(Cache->NamespaceEntries.begin(), Cache->NamespaceEntries.end(),
              [](const FrozenNamespaceEntry &Left,
                 const FrozenNamespaceEntry &Right) {
                if (Left.QualifiedName != Right.QualifiedName)
                  return Left.QualifiedName < Right.QualifiedName;
                return Left.Scope < Right.Scope;
              });

    const std::vector<const ModuleManifest *> Loaded = Modules.LoadedModules();
    if (Loaded.size() != Reflection->ModuleCount())
      return FreezePreparationStatus::InconsistentModule;
    for (const ModuleManifest *Manifest : Loaded) {
      if (Manifest == nullptr || !Manifest->IsValid())
        return FreezePreparationStatus::InconsistentModule;
      const ReflectionModuleFields *Reflected =
          FindReflectedModule(*Reflection, Manifest->Identity());
      if (Reflected == nullptr ||
          Reflected->Version != Manifest->Version().ToString())
        return FreezePreparationStatus::InconsistentModule;
      const CommittedSymbol *Symbol =
          Generations.Symbols().Find(Reflected->Symbol);
      const auto RecordIndex = Reflection->IndexOf(Reflected->Symbol);
      const ReflectionRecordFields *Record =
          RecordIndex ? Reflection->RecordAt(*RecordIndex) : nullptr;
      if (Symbol == nullptr || Symbol->Category != PlanEntryKind::Module ||
          Record == nullptr || Record->Kind != SymbolKind::Module)
        return FreezePreparationStatus::InconsistentModule;
      Cache->ModuleEntries.push_back(FrozenModuleEntry{
          Manifest->Identity(), Manifest->Version().ToString(),
          Reflected->Symbol, *Manifest});
    }

    Prepared = std::shared_ptr<const FreezeCacheStorage>(std::move(Cache));
    return FreezePreparationStatus::Prepared;
  } catch (const std::exception &) {
    Prepared.reset();
    return FreezePreparationStatus::AllocationFailure;
  } catch (...) {
    Prepared.reset();
    return FreezePreparationStatus::AllocationFailure;
  }
}

} // namespace Luna::Detail
