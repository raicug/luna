// clang-format off
#include "state/testing/test_hooks.hpp"

#include <luna/module/module_manifest.hpp>
#include <luna/state/state.hpp>

#include "state/dispatch/generation.hpp"
#include "state/freeze/cache.hpp"
#include "state/impl.hpp"
#include "state/module/registry.hpp"
#include "state/invocation/conversion/argument_reader.hpp"
#include "state/registration/record.hpp"
#include "state/registration/scope_plan.hpp"
#include "state/registration/submission.hpp"
#include "state/transaction/capture.hpp"
#include "state/transaction/installation.hpp"
#include "state/type/structured_conversion.hpp"
#include "state/type/type_generation.hpp"
#include "state/type/type_record.hpp"
#include "state/userdata/access.hpp"
#include "state/userdata/class_registry.hpp"
#include "state/userdata/collection.hpp"
#include "state/userdata/exposure.hpp"
#include "state/userdata/header.hpp"
#include "state/userdata/identity.hpp"
#include "state/userdata/identity_cache.hpp"
#include "state/userdata/lazy_cache.hpp"
#include "state/userdata/member_access.hpp"
#include "state/userdata/ownership.hpp"
#include "state/userdata/value_exposure.hpp"
#include "state/vm/namespace_table.hpp"
#include "state/vm/saved_value.hpp"
#include "state/vm/stack_checkpoint.hpp"

#include <lua.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace Luna::Detail {

void StateTestHooks::ResetLifecycle() noexcept {
  StateTestControl::ResetLifecycle();
}

void StateTestHooks::FailNextCreations(std::size_t Count) noexcept {
  StateTestControl::FailNextCreations(Count);
}

StateLifecycleCounters StateTestHooks::Lifecycle() noexcept {
  return StateTestControl::Counters();
}

std::optional<int>
StateTestHooks::ObserveRootStackDepth(const State &Owner) noexcept {
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return std::nullopt;
  return Owner.Implementation->VirtualMachine.StackDepth();
}

bool StateTestHooks::SetRootStackDepth(State &Owner, int Depth) noexcept {
  return Owner.Implementation && Owner.Implementation->IsReady() &&
         Owner.Implementation->VirtualMachine.SetStackDepth(Depth);
}

std::size_t StateTestHooks::BindingCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->Bindings.Count();
}

std::size_t StateTestHooks::PendingBindingCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->Bindings.PendingCount();
}

bool StateTestHooks::BindingIsCommitted(const State &Owner,
                                        std::string_view GlobalName) noexcept {
  if (!Owner.Implementation)
    return false;
  const auto *Record = Owner.Implementation->Bindings.Find(GlobalName);
  return Record && Record->IsCommitted();
}

std::optional<std::uintptr_t>
StateTestHooks::BindingRecordAddress(const State &Owner,
                                     std::string_view GlobalName) noexcept {
  if (!Owner.Implementation)
    return std::nullopt;
  const auto *Record = Owner.Implementation->Bindings.Find(GlobalName);
  if (!Record)
    return std::nullopt;
  return reinterpret_cast<std::uintptr_t>(Record);
}

std::optional<std::uintptr_t> StateTestHooks::InstalledBindingRecordAddress(
    const State &Owner, std::string_view GlobalName) noexcept {
  if (!Owner.Implementation)
    return std::nullopt;
  const auto *Record = Owner.Implementation->Bindings.Find(GlobalName);
  if (!Record)
    return std::nullopt;
  const auto *Installed =
      Owner.Implementation->VirtualMachine.ObserveInstalledBinding(
          Record->GlobalName());
  if (!Installed)
    return std::nullopt;
  return reinterpret_cast<std::uintptr_t>(Installed);
}

std::optional<std::uint64_t>
StateTestHooks::DispatchSlotOf(const State &Owner,
                               std::string_view GlobalName) noexcept {
  if (!Owner.Implementation)
    return std::nullopt;
  const std::optional<DispatchSlotId> Slot =
      Owner.Implementation->Bindings.Dispatch().FindSlot(GlobalName);
  if (!Slot)
    return std::nullopt;
  return Slot->Value;
}

std::optional<std::uint64_t>
StateTestHooks::InstalledDispatchSlotOf(const State &Owner,
                                        std::string_view GlobalName) noexcept {
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return std::nullopt;
  const DispatchSlotId Slot =
      Owner.Implementation->VirtualMachine.ObserveInstalledDispatchSlot(
          std::string(GlobalName));
  if (!Slot.IsValid())
    return std::nullopt;
  return Slot.Value;
}

bool StateTestHooks::DispatchSlotIsAvailable(
    const State &Owner, std::string_view GlobalName) noexcept {
  if (!Owner.Implementation)
    return false;
  const DispatchTable &Dispatch = Owner.Implementation->Bindings.Dispatch();
  const std::optional<DispatchSlotId> Slot = Dispatch.FindSlot(GlobalName);
  return Slot.has_value() && Dispatch.Resolve(*Slot) != nullptr;
}

std::size_t
StateTestHooks::IssuedDispatchSlotCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->Bindings.Dispatch().IssuedSlotCount();
}

std::uint64_t
StateTestHooks::DispatchGenerationOf(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->Bindings.Dispatch().Generation();
}

std::size_t
StateTestHooks::DispatchInvocationRetainerCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->Bindings.Dispatch().RetainerCount(
      DispatchRetainer::Invocation);
}

std::size_t
StateTestHooks::SupersededDispatchGenerationCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->Bindings.Dispatch().SupersededGenerationCount();
}

std::size_t
StateTestHooks::RetainedDispatchGenerationCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->Bindings.Dispatch().RetainedGenerationCount();
}

std::size_t StateTestHooks::ReclaimDispatchGenerations(State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->Bindings.Dispatch().ReclaimUnretained();
}

bool StateTestHooks::RetireDispatchSlot(State &Owner,
                                        std::string_view GlobalName) {
  if (!Owner.Implementation)
    return false;
  DispatchTable &Dispatch = Owner.Implementation->Bindings.Dispatch();
  const std::optional<DispatchSlotId> Slot = Dispatch.FindSlot(GlobalName);
  if (!Slot)
    return false;
  Dispatch.Retire(*Slot);
  return true;
}

bool StateTestHooks::RetargetDispatchSlot(State &Owner,
                                          std::string_view SlotName,
                                          std::string_view TargetName) {
  if (!Owner.Implementation)
    return false;
  BindingStore &Bindings = Owner.Implementation->Bindings;
  DispatchTable &Dispatch = Bindings.Dispatch();
  const std::optional<DispatchSlotId> Slot = Dispatch.FindSlot(SlotName);
  BindingRecord *Target = Bindings.Find(TargetName);
  if (!Slot || !Target)
    return false;

  Dispatch.Bind(*Slot, std::string(SlotName), Target, Target->Faults(),
                &Bindings.Types());
  return true;
}

DispatchRetention
StateTestHooks::RetainDispatchGeneration(const State &Owner,
                                         DispatchRetainer Retainer) {
  if (!Owner.Implementation)
    return DispatchRetention{};
  return Owner.Implementation->Bindings.Dispatch().Retain(Retainer);
}

std::size_t
StateTestHooks::OverloadCandidateCount(const State &Owner,
                                       std::string_view GlobalName) noexcept {
  if (!Owner.Implementation)
    return 0;
  const auto *Record = Owner.Implementation->Bindings.Find(GlobalName);
  return Record ? Record->CommittedCandidateCount() : 0;
}

std::size_t StateTestHooks::StagedOverloadCandidateCount(
    const State &Owner, std::string_view GlobalName) noexcept {
  if (!Owner.Implementation)
    return 0;
  const auto *Record = Owner.Implementation->Bindings.Find(GlobalName);
  if (!Record)
    return 0;
  return Record->CandidateCount() - Record->CommittedCandidateCount();
}

std::vector<std::string>
StateTestHooks::OverloadCandidateSignatures(const State &Owner,
                                            std::string_view GlobalName) {
  std::vector<std::string> Signatures;
  if (!Owner.Implementation)
    return Signatures;
  const auto *Record = Owner.Implementation->Bindings.Find(GlobalName);
  if (!Record)
    return Signatures;

  const std::shared_ptr<const TypeGeneration> Types =
      Record->CaptureTypeGeneration();
  for (std::size_t Index = 0; Index < Record->CandidateCount(); ++Index) {
    const OverloadCandidate *Candidate = Record->CandidateAt(Index);
    if (!Candidate || !Candidate->IsCommitted)
      continue;

    std::string Text = "(";
    const auto &Parameters = Candidate->Signature.ParameterTypes;
    for (std::size_t Position = 0; Position < Parameters.size(); ++Position) {
      if (Position != 0)
        Text += ", ";
      const std::string_view Public =
          Types ? Types->PublicNameOf(Parameters[Position])
                : std::string_view();
      Text += Public.empty() ? CanonicalTypeText(Parameters[Position])
                             : std::string(Public);
      if (Position >= Candidate->Signature.RequiredParameterCount)
        Text += " (optional)";
    }
    if (Candidate->Signature.IsVariadic) {
      if (!Parameters.empty())
        Text += ", ";
      Text += "...";
    }
    Text += ")";
    Signatures.push_back(std::move(Text));
  }
  return Signatures;
}

bool StateTestHooks::SetIntegerGlobal(State &Owner,
                                      const std::string &GlobalName,
                                      int Value) noexcept {
  return Owner.Implementation && Owner.Implementation->IsReady() &&
         Owner.Implementation->VirtualMachine.SetIntegerGlobal(GlobalName,
                                                               Value);
}

std::optional<int>
StateTestHooks::ObserveIntegerGlobal(const State &Owner,
                                     const std::string &GlobalName) noexcept {
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return std::nullopt;
  return Owner.Implementation->VirtualMachine.ObserveIntegerGlobal(GlobalName);
}

NativeInvocationObservation
StateTestHooks::InvokeBinding(State &Owner, std::string_view GlobalName,
                              const std::vector<Value> &Arguments) {
  NativeInvocationObservation Observation;
  if (!Owner.Implementation || !Owner.Implementation->IsReady()) {
    Observation.ErrorMessage = "State is not ready.";
    return Observation;
  }

  auto *Record = Owner.Implementation->Bindings.Find(GlobalName);
  lua_State *Vm = Owner.Implementation->VirtualMachine.Handle;
  if (!Record || !Record->IsCommitted() || !Vm) {
    Observation.ErrorMessage = "Binding is not available.";
    return Observation;
  }

  StackCheckpoint Checkpoint(Vm);
  Observation.EntryStackDepth = Checkpoint.EntryDepth();
  if (!lua_checkstack(Vm, static_cast<int>(Arguments.size()) + 1)) {
    Observation.ErrorMessage = "Could not reserve invocation stack.";
    Observation.FinalStackDepth = Observation.EntryStackDepth;
    return Observation;
  }

  lua_getglobal(Vm, Record->GlobalName().c_str());
  for (const auto &Argument : Arguments) {
    std::visit(
        [Vm](const auto &TypedValue) {
          using Type = std::decay_t<decltype(TypedValue)>;
          if constexpr (std::is_same_v<Type, bool>)
            lua_pushboolean(Vm, TypedValue ? 1 : 0);
          else if constexpr (std::is_same_v<Type, int>)
            lua_pushinteger(Vm, TypedValue);
          else if constexpr (std::is_same_v<Type, double>)
            lua_pushnumber(Vm, TypedValue);
          else
            lua_pushlstring(Vm, TypedValue.data(), TypedValue.size());
        },
        Argument);
  }

  const int Status =
      lua_pcall(Vm, static_cast<int>(Arguments.size()), LUA_MULTRET, 0);
  if (Status == LUA_OK) {
    Observation.Succeeded = true;
    Observation.ReturnCount = lua_gettop(Vm) - Observation.EntryStackDepth;
    const OverloadCandidate *Primary = Record->PrimaryCandidate();
    const ReturnMetadata *Return =
        Primary ? &Primary->Descriptor.Metadata().ReturnType() : nullptr;
    if (Observation.ReturnCount == 1 && Return && Return->Kind()) {
      auto Read = ReadArgument(Vm, -1, *Return->Kind());
      if (Read.IsSuccess())
        Observation.ReturnedValue = std::move(*Read.ConvertedValue);
    }
  } else {
    std::size_t Length = 0;
    const char *Message = lua_tolstring(Vm, -1, &Length);
    Observation.ErrorMessage.assign(Message ? Message : "Luau error.",
                                    Message ? Length : 11);
  }

  Observation.CompletionStackDepth = lua_gettop(Vm);
  lua_settop(Vm, Observation.EntryStackDepth);
  Observation.FinalStackDepth = lua_gettop(Vm);
  return Observation;
}

std::optional<CallbackStackRestorationObservation>
StateTestHooks::ObserveLastCallbackStackRestoration(
    const State &Owner) noexcept {
  if (!Owner.Implementation)
    return std::nullopt;
  return Owner.Implementation->Faults.LastCallbackStackRestoration();
}

ReflectionDatabase *
StateTestHooks::ReflectionDatabaseOf(State &Owner) noexcept {
  if (!Owner.Implementation)
    return nullptr;
  return &Owner.Implementation->Reflection;
}

std::uint64_t
StateTestHooks::ReflectionGeneration(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->Reflection.Generation();
}

std::shared_ptr<const GenerationSet>
StateTestHooks::GenerationsOf(const State &Owner) {
  if (!Owner.Implementation)
    return GenerationSet::Initial();
  return Owner.Implementation->CurrentGenerations();
}

std::optional<StateIdentity>
StateTestHooks::LogicalIdentityOf(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return std::nullopt;
  return Owner.Implementation->LogicalIdentity();
}

std::optional<std::uint64_t>
StateTestHooks::OwnerEpochOf(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return std::nullopt;
  return Owner.Implementation->OwnerEpoch();
}

std::optional<std::uint64_t>
StateTestHooks::LifecycleGenerationOf(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return std::nullopt;
  return Owner.Implementation->LifecycleGeneration();
}

bool StateTestHooks::HasActiveTransaction(const State &Owner) noexcept {
  return Owner.Implementation &&
         Owner.Implementation->ActiveTransaction != nullptr;
}

std::optional<TransactionCapture>
StateTestHooks::CaptureTransactionEntryOf(const State &Owner) {
  if (!Owner.Implementation)
    return std::nullopt;
  return Owner.Implementation->CaptureTransactionEntry();
}

bool StateTestHooks::MarkFrozen(State &Owner) noexcept {
  if (!Owner.Implementation)
    return false;
  Owner.Implementation->Lifecycle.Freeze();
  return true;
}

bool StateTestHooks::IsFrozen(const State &Owner) noexcept {
  return Owner.Implementation && Owner.Implementation->Lifecycle.IsFrozen();
}

FreezeCacheObservation StateTestHooks::ObserveFreezeCache(const State &Owner) {
  FreezeCacheObservation Observed;
  if (!Owner.Implementation)
    return Observed;
  const std::shared_ptr<const FreezeCacheStorage> Cache =
      Owner.Implementation->FrozenCache();
  if (!Cache)
    return Observed;
  Observed.Published = true;
  Observed.Address = reinterpret_cast<std::uintptr_t>(Cache.get());
  Observed.Key = Cache->Key();
  Observed.Lookups = Cache->LookupCount();
  Observed.Overloads = Cache->OverloadCount();
  Observed.Conversions = Cache->ConversionCount();
  Observed.CastPaths = Cache->CastPathCount();
  Observed.Metatables = Cache->MetatableCount();
  Observed.Namespaces = Cache->NamespaceCount();
  Observed.Modules = Cache->ModuleCount();
  for (const FrozenLookupEntry &Lookup : Cache->SortedLookups()) {
    Observed.OrderedLookups.push_back(Lookup.QualifiedName + ":" +
                                      Lookup.Identity.ToString());
    Observed.LookupDetails.push_back(
        Lookup.QualifiedName + "|" +
        std::string(PlanEntryKindText(Lookup.Category)) + "|" +
        std::string(SymbolKindText(Lookup.Kind)) + "|" +
        Lookup.Identity.ToString() + "|" +
        (Lookup.ReflectionIndex ? std::to_string(*Lookup.ReflectionIndex)
                                : std::string("-")));
  }
  for (const FrozenOverloadIndex &Overload : Cache->OverloadIndices()) {
    std::string Text = Overload.QualifiedName + "|" +
                       std::to_string(Overload.LookupIndices.size()) + "|";
    for (std::size_t Position = 0; Position < Overload.LookupIndices.size();
         ++Position) {
      if (Position != 0)
        Text.push_back(',');
      Text.append(std::to_string(Overload.LookupIndices[Position]));
    }
    Observed.OrderedOverloads.push_back(std::move(Text));
  }
  for (const FrozenConversionEntry &Conversion : Cache->ConversionTable())
    Observed.OrderedConversions.push_back(Conversion.Identity.ToString());
  for (const FrozenCastPath &Path : Cache->ClassCastPaths()) {
    Observed.OrderedCastPaths.push_back(
        Path.Source.ToString() + "|" + Path.Target.ToString() + "|" +
        std::string(ClassConversionKindText(Path.Kind)));
  }
  for (const FrozenMetatableEntry &Metatable : Cache->MetatableMap()) {
    Observed.OrderedMetatables.push_back(Metatable.QualifiedName + "|" +
                                         Metatable.Type.ToString() + "|" +
                                         Metatable.ClassSymbol.ToString());
    Observed.MetatableIdentities.push_back(Metatable.Metatable.Value());
  }
  for (const FrozenNamespaceEntry &Scope : Cache->NamespaceCache())
    Observed.OrderedNamespaces.push_back(Scope.QualifiedName + "|" +
                                         Scope.Scope.ToString());
  for (const FrozenModuleEntry &Module : Cache->ModuleCache())
    Observed.OrderedModules.push_back(Module.Identity + "|" + Module.Version +
                                      "|" + Module.Symbol.ToString());
  return Observed;
}

bool StateTestHooks::AdvanceLifecycleGeneration(State &Owner) noexcept {
  if (!Owner.Implementation)
    return false;
  Owner.Implementation->AdvanceLifecycleGeneration();
  return true;
}

std::size_t StateTestHooks::LoadedModuleCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->LoadedModules().Count();
}

bool StateTestHooks::ModuleIsLoaded(const State &Owner,
                                    std::string_view Identity) noexcept {
  if (!Owner.Implementation)
    return false;
  return Owner.Implementation->LoadedModules().IsLoaded(Identity);
}

std::optional<std::string>
StateTestHooks::LoadedModuleVersion(const State &Owner,
                                    std::string_view Identity) {
  if (!Owner.Implementation)
    return std::nullopt;
  const ModuleManifest *Loaded =
      Owner.Implementation->LoadedModules().Find(Identity);
  if (!Loaded)
    return std::nullopt;
  return Loaded->Version().ToString();
}

std::size_t StateTestHooks::AvailableModuleCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->AvailableModuleCount();
}

std::size_t
StateTestHooks::NamespaceOwnershipCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->NamespaceOwnerships().Size();
}

bool StateTestHooks::NamespaceIsOwned(const State &Owner,
                                      std::string_view QualifiedName) {
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return false;

  const NamespaceOwnership *Ownership =
      Owner.Implementation->NamespaceOwnerships().Find(QualifiedName);
  if (!Ownership)
    return false;

  const VmPathObservation Observed =
      Owner.Implementation->VirtualMachine.ObserveVmPath(
          std::string(QualifiedName));
  return NamespaceOwnershipTable::Matches(
             *Ownership, Owner.Implementation->LogicalIdentity(), QualifiedName,
             Ownership->Scope, Observed.Table) &&
         NamespaceOwnershipTable::IsCurrent(
             *Ownership, Owner.Implementation->LifecycleGeneration());
}

std::size_t StateTestHooks::RegisteredClassCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->RegisteredClasses().Size();
}

bool StateTestHooks::ClassIsRegistered(const State &Owner,
                                       std::string_view QualifiedName) {
  if (!Owner.Implementation)
    return false;

  const RegisteredClass *Registered =
      Owner.Implementation->RegisteredClasses().Find(QualifiedName);
  if (!Registered || !Registered->IsComplete())
    return false;
  return ClassRegistry::Matches(*Registered,
                                Owner.Implementation->LogicalIdentity(),
                                Registered->Type, Registered->ClassSymbol) &&
         ClassRegistry::IsCurrent(*Registered,
                                  Owner.Implementation->LifecycleGeneration());
}

std::optional<TypeId>
StateTestHooks::ClassTypeOf(const State &Owner,
                            std::string_view QualifiedName) {
  if (!Owner.Implementation)
    return std::nullopt;

  const RegisteredClass *Registered =
      Owner.Implementation->RegisteredClasses().Find(QualifiedName);
  if (!Registered)
    return std::nullopt;
  return Registered->Type;
}

std::optional<std::uint64_t>
StateTestHooks::ClassMetatableIdentityOf(const State &Owner,
                                         std::string_view QualifiedName) {
  if (!Owner.Implementation)
    return std::nullopt;

  const RegisteredClass *Registered =
      Owner.Implementation->RegisteredClasses().Find(QualifiedName);
  if (!Registered || !Registered->Metatable.IsValid())
    return std::nullopt;
  return Registered->Metatable.Value();
}

std::size_t
StateTestHooks::IssuedMetatableIdentityCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return static_cast<std::size_t>(
      Owner.Implementation->RegisteredClasses().IssuedMetatableIdentities());
}

bool StateTestHooks::ClassMetatableIsCreated(const State &Owner,
                                             std::string_view QualifiedName) {
  if (!Owner.Implementation)
    return false;

  const RegisteredClass *Registered =
      Owner.Implementation->RegisteredClasses().Find(QualifiedName);
  return Registered != nullptr && Registered->Table != nullptr &&
         Registered->MetatableCreations != 0;
}

std::size_t
StateTestHooks::ClassMetatableCreationCount(const State &Owner,
                                            std::string_view QualifiedName) {
  if (!Owner.Implementation)
    return 0;

  const RegisteredClass *Registered =
      Owner.Implementation->RegisteredClasses().Find(QualifiedName);
  if (!Registered)
    return 0;
  return static_cast<std::size_t>(Registered->MetatableCreations);
}

bool StateTestHooks::CollectGarbage(State &Owner) noexcept {
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return false;
  return Owner.Implementation->VirtualMachine.CollectGarbage();
}

bool StateTestHooks::UserdataCollectorIsInstalled(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return false;
  return Owner.Implementation->VirtualMachine.HasUserdataCollector();
}

UserdataCollectionCounters
StateTestHooks::ObserveUserdataCollections() noexcept {
  return Luna::Detail::ObserveUserdataCollections();
}

void StateTestHooks::ResetUserdataCollections() noexcept {
  Luna::Detail::ResetUserdataCollections();
}

StateDestructionObservation
StateTestHooks::ObserveLastStateDestruction() noexcept {
  return Luna::Detail::ObserveLastStateDestruction();
}

std::optional<UserdataHeader> StateTestHooks::DescribeClassUserdata(
    const State &Owner, std::string_view QualifiedName,
    OwnershipModel Ownership, ConstAccess Access) {
  if (!Owner.Implementation)
    return std::nullopt;

  const RegisteredClass *Registered =
      Owner.Implementation->RegisteredClasses().Find(QualifiedName);
  if (!Registered)
    return std::nullopt;

  UserdataHeaderRequest Request;
  Request.Origin = Registered->Origin;
  Request.DynamicType = Registered->Type;
  Request.DeclaredViewType = Registered->Type;
  Request.ClassSymbol = Registered->ClassSymbol;
  Request.Metatable = Registered->Metatable;
  Request.Ownership = Ownership;
  Request.Access = Access;
  return MakeUserdataHeader(Request);
}

namespace {

[[nodiscard]] std::uint64_t
ObserveModelledHandleGeneration(const void *Record) noexcept {
  return Record != nullptr ? *static_cast<const std::uint64_t *>(Record) : 0;
}

struct MemberAccessTarget final {
  const RegisteredMember *Member = nullptr;
  bool ClassIsRegistered = false;
  MemberAccessContext Context;

  std::shared_ptr<const TypeGeneration> Types;
};

[[nodiscard]] MemberAccessTarget
ResolveMemberAccess(const RegisteredClass *Registered,
                    const UserdataAccessContext &Access,
                    LazyPropertyCache &Cache, std::uint64_t Generation,
                    std::string_view Member) {
  MemberAccessTarget Target;
  if (!Registered)
    return Target;
  Target.ClassIsRegistered = true;

  const RegisteredMember *Declared = Registered->FindMember(Member);
  if (!Declared)
    return Target;

  Target.Types = Access.Types != nullptr ? Access.Types->Capture()
                                         : TypeGeneration::Foundation();
  Target.Context.Receiver.Origin = Access.Origin;
  Target.Context.Receiver.Metatable = Registered->Metatable;
  Target.Context.Receiver.RequestedType = Registered->Type;
  Target.Context.Receiver.HandleProbe = Access.HandleProbe;
  if (Access.Classes != nullptr)
    Target.Context.Receiver.Relationships = &Access.Classes->Relationships();
  Target.Context.Lazy = &Cache;
  Target.Context.Types = Target.Types.get();
  Target.Context.DispatchGeneration = Generation;
  Target.Member = Declared;
  return Target;
}

} // namespace

std::size_t StateTestHooks::CachedIdentityCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->NativeIdentityCache().Size();
}

std::size_t
StateTestHooks::LiveCachedIdentityCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->NativeIdentityCache().ActiveCount();
}

ClassExposureObservation
StateTestHooks::ExposeClassUserdata(State &Owner,
                                    const ClassExposureRequest &Request) {
  ClassExposureObservation Observed;
  Observed.Status = std::string(
      UserdataExposureStatusText(UserdataExposureStatus::UnavailableRequest));
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return Observed;

  const RegisteredClass *Declared =
      Owner.Implementation->Classes.Find(Request.QualifiedName);
  RegisteredClass *Registered =
      Declared ? Owner.Implementation->Classes.FindForUpdate(Declared->Type)
               : nullptr;
  if (!Registered)
    return Observed;

  UserdataAccessContext &Context = Owner.Implementation->UserdataAccess();
  if (Request.LifetimeGeneration != nullptr)
    Context.HandleProbe = &ObserveModelledHandleGeneration;

  UserdataExposureRequest Exposure;
  Exposure.Storage = Request.Storage;
  Exposure.Ownership = Request.Ownership;
  Exposure.Access = Request.Access;
  Exposure.Path = Request.Path;
  if (Request.LifetimeGeneration != nullptr) {
    Exposure.Handle.Record = Request.LifetimeGeneration;
    Exposure.Handle.Generation = *Request.LifetimeGeneration;
  }

  const UserdataExposure Result = ExposeUserdataValue(
      Owner.Implementation->VirtualMachine.Handle, Context, *Registered,
      Owner.Implementation->NativeIdentities(), Exposure);
  Observed.Status = std::string(UserdataExposureStatusText(Result.Status));
  Observed.Created = Result.Status == UserdataExposureStatus::Created;
  Observed.Reused = Result.Status == UserdataExposureStatus::Reused;
  Observed.Nonce = Result.Identity.Nonce;
  return Observed;
}

ClassValueWriteObservation
StateTestHooks::ExposeClassValue(State &Owner,
                                 const ClassValueExposureRequest &Request) {
  ClassValueWriteObservation Observed;
  Observed.Failure =
      std::string(StructuredFailureText(StructuredFailure::InternalFailure));
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return Observed;

  const RegisteredClass *Registered =
      Owner.Implementation->Classes.Find(Request.QualifiedName);
  if (!Registered)
    return Observed;

  const std::shared_ptr<const GenerationSet> Generations =
      Owner.Implementation->CurrentGenerations();
  const std::shared_ptr<const TypeGeneration> Types =
      Generations ? Generations->Types() : nullptr;
  if (!Types)
    return Observed;

  static_cast<void>(
      Owner.Implementation->VirtualMachine.PublishUserdataContexts(
          Owner.Implementation->UserdataAccess(),
          Owner.Implementation->UserdataExposure()));

  auto Intent = std::make_shared<ClassExposureIntent>();
  Intent->Storage = Request.Storage;
  Intent->Ownership = Request.Ownership;
  Intent->Access = Request.Access;
  Intent->Handle = Request.Handle;
  Intent->SharedOwnership = Request.SharedOwnership;
  Intent->Allocator = Request.Allocator;

  return WriteExposedClassValue(Owner.Implementation->VirtualMachine.Handle,
                                *Types, Registered->Key, Request.Path,
                                std::move(Intent));
}

ClassValueWriteObservation StateTestHooks::ConstructClassValue(
    State &Owner, const ClassValueConstructionRequest &Request) {
  ClassValueWriteObservation Observed;
  Observed.Failure =
      std::string(StructuredFailureText(StructuredFailure::InternalFailure));
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return Observed;

  const RegisteredClass *Registered =
      Owner.Implementation->Classes.Find(Request.QualifiedName);
  if (!Registered)
    return Observed;

  const std::shared_ptr<const GenerationSet> Generations =
      Owner.Implementation->CurrentGenerations();
  const std::shared_ptr<const TypeGeneration> Types =
      Generations ? Generations->Types() : nullptr;
  if (!Types)
    return Observed;

  static_cast<void>(
      Owner.Implementation->VirtualMachine.PublishUserdataContexts(
          Owner.Implementation->UserdataAccess(),
          Owner.Implementation->UserdataExposure()));

  auto Intent = std::make_shared<ClassExposureIntent>();
  Intent->Storage = nullptr;
  Intent->Ownership = Request.Ownership;
  Intent->Access = Request.Access;
  Intent->Handle = Request.Handle;
  Intent->SharedOwnership = Request.SharedOwnership;
  Intent->Allocator = Request.Allocator;
  Intent->Construct = Request.Construct;

  return WriteExposedClassValue(Owner.Implementation->VirtualMachine.Handle,
                                *Types, Registered->Key, Request.Path,
                                std::move(Intent));
}

bool StateTestHooks::ReleaseClassValue(State &Owner, const void *Storage,
                                       ReleaseCause Cause) {
  if (!Owner.Implementation)
    return false;
  return Owner.Implementation->UserdataOwnership().ReleaseByStorage(Storage,
                                                                    Cause);
}

ClassAccessObservation
StateTestHooks::AccessClassUserdata(State &Owner,
                                    const ClassAccessRequest &Request) {
  ClassAccessObservation Observed;
  Observed.Failure =
      std::string(StructuredFailureText(StructuredFailure::InternalFailure));
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return Observed;

  const RegisteredClass *Registered =
      Owner.Implementation->Classes.Find(Request.QualifiedName);
  if (!Registered)
    return Observed;

  const std::shared_ptr<const GenerationSet> Generations =
      Owner.Implementation->CurrentGenerations();
  const std::shared_ptr<const TypeGeneration> Types =
      Generations ? Generations->Types() : nullptr;
  if (!Types)
    return Observed;

  const UserdataHandleObservation Handle =
      ReadExposedUserdataHandle(Owner.Implementation->VirtualMachine.Handle,
                                *Types, Registered->Key, Request.Path);
  Observed.ReachedNativeCode = Handle.ReachedNativeCode;
  Observed.DeliveredExpectedObject = Handle.ReachedNativeCode &&
                                     Request.ExpectedStorage != nullptr &&
                                     Handle.Storage == Request.ExpectedStorage;
  Observed.PermitsMutation = Handle.PermitsMutation;
  Observed.Failure = Handle.Failure;
  Observed.Diagnostic = Handle.Diagnostic;
  return Observed;
}

bool StateTestHooks::RetireClassUserdata(State &Owner, const void *Storage) {
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return false;
  const UserdataCacheEntry *Cached =
      Owner.Implementation->NativeIdentityCache().FindActive(Storage);
  if (Cached == nullptr)
    return false;
  return RetireExposedUserdata(Owner.Implementation->VirtualMachine.Handle,
                               Owner.Implementation->UserdataAccess(),
                               Cached->Identity);
}

std::optional<UserdataHeader>
StateTestHooks::ObserveClassUserdata(const State &Owner,
                                     const std::string &Path) {
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return std::nullopt;

  UserdataHeader Observed;
  if (!ObserveExposedUserdataHeader(Owner.Implementation->VirtualMachine.Handle,
                                    Path, Observed))
    return std::nullopt;
  return Observed;
}

bool StateTestHooks::ClassMemberIsRegistered(const State &Owner,
                                             std::string_view QualifiedName,
                                             std::string_view Member) {
  if (!Owner.Implementation)
    return false;

  const RegisteredClass *Registered =
      Owner.Implementation->RegisteredClasses().Find(QualifiedName);
  if (!Registered)
    return false;
  const RegisteredMember *Declared = Registered->FindMember(Member);
  return Declared != nullptr && Declared->IsComplete();
}

std::size_t StateTestHooks::ClassMemberCount(const State &Owner,
                                             std::string_view QualifiedName) {
  if (!Owner.Implementation)
    return 0;

  const RegisteredClass *Registered =
      Owner.Implementation->RegisteredClasses().Find(QualifiedName);
  return Registered ? Registered->Members.size() : 0;
}

std::size_t StateTestHooks::ClassOperatorCount(const State &Owner,
                                               std::string_view QualifiedName) {
  if (!Owner.Implementation)
    return 0;

  const RegisteredClass *Registered =
      Owner.Implementation->RegisteredClasses().Find(QualifiedName);
  return Registered ? Registered->Operators.size() : 0;
}

bool StateTestHooks::ClassOperatorIsRegistered(const State &Owner,
                                               std::string_view QualifiedName,
                                               ClassOperator Selected) {
  return ClassOperatorSegment(Owner, QualifiedName, Selected).has_value();
}

std::optional<std::string>
StateTestHooks::ClassOperatorSegment(const State &Owner,
                                     std::string_view QualifiedName,
                                     ClassOperator Selected) {
  if (!Owner.Implementation)
    return std::nullopt;

  const RegisteredClass *Registered =
      Owner.Implementation->RegisteredClasses().Find(QualifiedName);
  if (!Registered)
    return std::nullopt;
  const RegisteredOperator *Declared = Registered->FindOperator(Selected);
  if (!Declared || !Declared->Symbol.IsValid())
    return std::nullopt;
  return Declared->Segment;
}

std::vector<ClassBaseView>
StateTestHooks::ClassBases(const State &Owner, std::string_view QualifiedName) {
  if (!Owner.Implementation)
    return {};

  const ClassRegistry &Classes = Owner.Implementation->RegisteredClasses();
  const RegisteredClass *Registered = Classes.Find(QualifiedName);
  if (!Registered)
    return {};
  return Classes.BasesOf(Registered->Type);
}

std::vector<ClassCastView>
StateTestHooks::ClassCasts(const State &Owner, std::string_view QualifiedName) {
  if (!Owner.Implementation)
    return {};

  const ClassRegistry &Classes = Owner.Implementation->RegisteredClasses();
  const RegisteredClass *Registered = Classes.Find(QualifiedName);
  if (!Registered)
    return {};
  return Classes.CastsOf(Registered->Type);
}

std::vector<ClassInheritedMemberView>
StateTestHooks::ClassInheritedMembers(const State &Owner,
                                      std::string_view QualifiedName) {
  if (!Owner.Implementation)
    return {};

  const ClassRegistry &Classes = Owner.Implementation->RegisteredClasses();
  const RegisteredClass *Registered = Classes.Find(QualifiedName);
  if (!Registered)
    return {};
  return Classes.InheritedMembersOf(Registered->Type);
}

ClassMemberAccessObservation
StateTestHooks::ReadClassMemberValue(State &Owner,
                                     const ClassMemberAccessRequest &Request) {
  ClassMemberAccessObservation Observed;
  Observed.Failure = std::string(
      MemberAccessFailureText(MemberAccessFailure::UnavailableRequest));
  Observed.Receiver =
      std::string(UserdataAccessFailureText(UserdataAccessFailure::None));
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return Observed;

  MemberAccessTarget Target = ResolveMemberAccess(
      Owner.Implementation->RegisteredClasses().Find(Request.QualifiedName),
      Owner.Implementation->UserdataAccess(),
      Owner.Implementation->LazyMemberValues(),
      Owner.Implementation->LifecycleGeneration(), Request.Member);
  if (!Target.Member) {
    if (Target.ClassIsRegistered)
      Observed.Failure = std::string(
          MemberAccessFailureText(MemberAccessFailure::UnknownMember));
    return Observed;
  }

  MemberReadResult Result;
  const ExposedUserdataVisitor Visit = [&](UserdataHeader &Header) {
    Result = ReadClassMember(Target.Context, Header, *Target.Member);
  };
  if (!VisitExposedUserdataHeader(Owner.Implementation->VirtualMachine.Handle,
                                  Request.Path, Visit))
    return Observed;

  Observed.Reached = Result.IsSuccess();
  Observed.Failure = std::string(MemberAccessFailureText(Result.Failure));
  Observed.Receiver = std::string(UserdataAccessFailureText(Result.Receiver));
  Observed.Boundary = std::string(
      MemberSideEffectBoundaryText(MemberSideEffectBoundaryOf(Result.Failure)));
  Observed.Diagnostic = Result.Refusal;
  Observed.ServedFromCache = Result.ServedFromCache;
  Observed.Recorded = Result.Recorded;
  if (Result.IsSuccess())
    Observed.Produced = Result.Produced;
  return Observed;
}

ClassMemberAccessObservation
StateTestHooks::WriteClassMemberValue(State &Owner,
                                      const ClassMemberAccessRequest &Request) {
  ClassMemberAccessObservation Observed;
  Observed.Failure = std::string(
      MemberAccessFailureText(MemberAccessFailure::UnavailableRequest));
  Observed.Receiver =
      std::string(UserdataAccessFailureText(UserdataAccessFailure::None));
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return Observed;

  MemberAccessTarget Target = ResolveMemberAccess(
      Owner.Implementation->RegisteredClasses().Find(Request.QualifiedName),
      Owner.Implementation->UserdataAccess(),
      Owner.Implementation->LazyMemberValues(),
      Owner.Implementation->LifecycleGeneration(), Request.Member);
  if (!Target.Member) {
    if (Target.ClassIsRegistered)
      Observed.Failure = std::string(
          MemberAccessFailureText(MemberAccessFailure::UnknownMember));
    return Observed;
  }

  MemberWriteResult Result;
  const ExposedUserdataVisitor Visit = [&](UserdataHeader &Header) {
    Result = WriteClassMember(Target.Context, Header, *Target.Member,
                              Request.Incoming);
  };
  if (!VisitExposedUserdataHeader(Owner.Implementation->VirtualMachine.Handle,
                                  Request.Path, Visit))
    return Observed;

  Observed.Reached = Result.IsSuccess();
  Observed.Failure = std::string(MemberAccessFailureText(Result.Failure));
  Observed.Receiver = std::string(UserdataAccessFailureText(Result.Receiver));
  Observed.Boundary = std::string(
      MemberSideEffectBoundaryText(MemberSideEffectBoundaryOf(Result.Failure)));
  Observed.Diagnostic = Result.Refusal;
  Observed.Invalidated = Result.Invalidated;
  return Observed;
}

std::optional<MemberDispatchObservation>
StateTestHooks::ObserveLastClassMemberDispatch(const State &Owner) {
  if (!Owner.Implementation)
    return std::nullopt;
  const MemberDispatchObservation *Observed =
      Owner.Implementation->MemberDispatchObservations().Last();
  if (!Observed)
    return std::nullopt;
  return *Observed;
}

void StateTestHooks::ClearClassMemberDispatch(State &Owner) noexcept {
  if (Owner.Implementation)
    Owner.Implementation->MemberDispatchObservations().Clear();
}

std::size_t
StateTestHooks::InvalidateClassMemberCache(State &Owner,
                                           const std::string &Path) {
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return 0;

  LazyPropertyCache &Cache = Owner.Implementation->LazyMemberValues();
  std::size_t Removed = 0;
  const ExposedUserdataVisitor Visit = [&](UserdataHeader &Header) {
    Removed = Detail::InvalidateClassMemberCache(Cache, Header);
  };
  static_cast<void>(VisitExposedUserdataHeader(
      Owner.Implementation->VirtualMachine.Handle, Path, Visit));
  return Removed;
}

std::size_t
StateTestHooks::LazyMemberCacheNodeCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->LazyMemberValues().NodeCount();
}

std::size_t
StateTestHooks::LazyMemberCacheEntryCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->LazyMemberValues().EntryCount();
}

std::size_t
StateTestHooks::LiveLazyMemberCacheEntryCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->LazyMemberValues().LiveEntryCount(
      Owner.Implementation->LifecycleGeneration());
}

std::size_t
StateTestHooks::LazyMemberCacheEntryCountOf(const State &Owner,
                                            const void *Storage) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->LazyMemberValues().EntryCountOf(Storage);
}

std::uint64_t
StateTestHooks::LazyMemberCacheGenerationOf(const State &Owner,
                                            const void *Storage) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->LazyMemberValues().GenerationOf(Storage);
}

LazyCacheCounters
StateTestHooks::LazyMemberCacheCounters(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return LazyCacheCounters{};
  return Owner.Implementation->LazyMemberValues().Counters();
}

void StateTestHooks::ResetLazyMemberCacheCounters(State &Owner) noexcept {
  if (Owner.Implementation)
    Owner.Implementation->LazyMemberValues().ResetCounters();
}

bool StateTestHooks::ClassUserdataNamesLazyEntries(const State &Owner,
                                                   const std::string &Path) {
  const std::optional<UserdataHeader> Observed =
      ObserveClassUserdata(Owner, Path);
  return Observed.has_value() && Observed->LazyCache.IsPopulated();
}

std::uint64_t
StateTestHooks::ClassUserdataLazyGeneration(const State &Owner,
                                            const std::string &Path) {
  const std::optional<UserdataHeader> Observed =
      ObserveClassUserdata(Owner, Path);
  return Observed ? Observed->LazyCache.Generation : 0;
}

OwnershipRegistry *StateTestHooks::UserdataOwnershipOf(State &Owner) noexcept {
  if (!Owner.Implementation)
    return nullptr;
  return &Owner.Implementation->UserdataOwnership();
}

ReleaseCounters
StateTestHooks::UserdataReleaseCounters(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return ReleaseCounters{};
  return Owner.Implementation->UserdataOwnership().Counters();
}

ConstructionCounters
StateTestHooks::UserdataConstructionCounters(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return ConstructionCounters{};
  return Owner.Implementation->UserdataOwnership().ConstructionCounts();
}

std::size_t StateTestHooks::OwnedUserdataCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->UserdataOwnership().RecordCount();
}

std::size_t
StateTestHooks::PublishedUserdataCount(const State &Owner) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->UserdataOwnership().PublishedCount();
}

JoinedSubmissionReport StateTestHooks::SubmitJoinedFunctions(
    State &Owner, std::vector<JoinedFunctionDeclaration> Declarations,
    bool IgnoreNestedFailures) {
  if (!Owner.Implementation) {
    JoinedSubmissionReport Report;
    Report.Submitted = Declarations.size();
    return Report;
  }
  return Owner.Implementation->SubmitJoinedFunctionDeclarations(
      std::move(Declarations), IgnoreNestedFailures, false);
}

JoinedSubmissionReport StateTestHooks::PublishJoinedFunctions(
    State &Owner, std::vector<JoinedFunctionDeclaration> Declarations,
    bool IgnoreNestedFailures) {
  if (!Owner.Implementation) {
    JoinedSubmissionReport Report;
    Report.Submitted = Declarations.size();
    return Report;
  }
  return Owner.Implementation->SubmitJoinedFunctionDeclarations(
      std::move(Declarations), IgnoreNestedFailures, true);
}

CallbackBoundaryObservation StateTestHooks::SubmitThroughCallback(
    State &Owner, std::vector<JoinedFunctionDeclaration> Declarations,
    std::size_t ThrowAfterSubmissions, bool ThrowStandardException,
    bool PublishWhenComplete) {
  if (!Owner.Implementation) {
    CallbackBoundaryObservation Observed;
    Observed.Submitted = Declarations.size();
    return Observed;
  }
  return Owner.Implementation->SubmitJoinedFunctionsThroughCallback(
      std::move(Declarations), ThrowAfterSubmissions, ThrowStandardException,
      PublishWhenComplete);
}

std::optional<std::string>
StateTestHooks::ObserveVmPathValueKind(State &Owner,
                                       const std::string &Path) noexcept {
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return std::nullopt;

  SavedVmValue Saved;
  VirtualMachineOwner &Machine = Owner.Implementation->VirtualMachine;
  if (!Machine.CaptureVmPath(Path, Saved))
    return std::nullopt;
  const std::string Kind(VmValueKindText(Saved.Kind));
  Machine.ReleaseSavedValue(Saved);
  return Kind;
}

PublicationObservation StateTestHooks::ProbeInstallationJournal(
    State &Owner, const std::vector<std::string> &Paths,
    const std::vector<InstallationScope> &Overlays,
    bool RestoreInsteadOfCommit) {
  PublicationObservation Observed;
  if (!Owner.Implementation || !Owner.Implementation->IsReady())
    return Observed;

  State::Impl &Implementation = *Owner.Implementation;
  const int EntryDepth = Implementation.VirtualMachine.StackDepth();

  {
    InstallationJournal Journal(Implementation.VirtualMachine,
                                Implementation.Bindings, Implementation.Faults,
                                EntryDepth);

    for (const std::string &Path : Paths) {
      if (!Journal.JournalVirtualMachinePath(Path))
        break;
      if (Implementation.VirtualMachine.SetIntegerGlobal(Path, 4242))
        Journal.MarkInstalled();
    }
    for (const InstallationScope Scope : Overlays)
      Journal.JournalOverlay(Scope, std::string(InstallationScopeText(Scope)));

    if (RestoreInsteadOfCommit)
      Journal.Undo();
    else
      Journal.Commit();

    ObserveJournal(Journal, Observed);
    Observed.IsPublished = Journal.IsCommitted();
  }

  Observed.StackDepthAfter = Implementation.VirtualMachine.StackDepth();
  return Observed;
}

LifecycleAttemptObservation
StateTestHooks::PrepareLifecycleAttempt(State &Owner,
                                        const LifecycleAttempt &Attempt) {
  if (!Owner.Implementation)
    return LifecycleAttemptObservation();
  return Owner.Implementation->PrepareLifecycleAttempt(Attempt);
}

void StateTestHooks::InjectFault(State &Owner, StateFaultPoint Point,
                                 std::size_t Count) noexcept {
  if (Owner.Implementation)
    Owner.Implementation->Faults.Inject(Point, Count);
}

bool StateTestHooks::ConsumeFault(State &Owner,
                                  StateFaultPoint Point) noexcept {
  return Owner.Implementation && Owner.Implementation->Faults.Consume(Point);
}

std::size_t StateTestHooks::PendingFaults(const State &Owner,
                                          StateFaultPoint Point) noexcept {
  if (!Owner.Implementation)
    return 0;
  return Owner.Implementation->Faults.Pending(Point);
}

} // namespace Luna::Detail
