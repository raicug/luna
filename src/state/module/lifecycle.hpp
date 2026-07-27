#pragma once

// clang-format off
#include <luna/module/module_manifest.hpp>
#include <luna/reflection/ids.hpp>
#include <luna/type/type_descriptor.hpp>

#include "state/userdata/identity.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

class ClassRegistry;
class DispatchTable;
class FreezeCacheStorage;
class LazyPropertyCache;
class ModuleRegistry;
class OwnershipRegistry;
class ReflectionStorage;
class UserdataIdentityCache;

enum class LifecycleOperation : std::uint8_t { Unload, Replacement };

[[nodiscard]] std::string_view
LifecycleOperationText(LifecycleOperation Operation) noexcept;

enum class LifecycleAffectedKind : std::uint8_t {
  Function,
  Namespace,
  Type,
  Userdata,
  ReflectionRecord,
  Cache,
  Closure,
  DependentModule,
  RootedReference,
  RetainedGeneration
};

[[nodiscard]] std::string_view
LifecycleAffectedKindText(LifecycleAffectedKind Kind) noexcept;

struct LifecycleAffectedItem final {
  LifecycleAffectedKind Kind = LifecycleAffectedKind::Function;
  std::string Subject;
  std::string Detail;
  std::uint64_t Ordinal = 0;

  [[nodiscard]] std::string Text() const;

  friend bool operator==(const LifecycleAffectedItem &Left,
                         const LifecycleAffectedItem &Right);
};

[[nodiscard]] std::strong_ordering
CompareAffected(const LifecycleAffectedItem &Left,
                const LifecycleAffectedItem &Right);

enum class LifecycleBlockerKind : std::uint8_t {
  UnsupportedDynamicMode,
  UnknownModule,
  InvalidReplacementManifest,
  IdentityMismatch,
  MissingDependency,
  UnsatisfiedDependencyConstraint,
  DependentModule,
  IncompatibleDeclaration,
  IncompatibleType,
  IncompatibleCallableSignature,
  IncompatibleClassOwnership,
  LiveUserdata,
  UnavailableUserdataMigration,
  RootedReference
};

[[nodiscard]] std::string_view
LifecycleBlockerKindText(LifecycleBlockerKind Kind) noexcept;

struct LifecycleBlocker final {
  LifecycleBlockerKind Kind = LifecycleBlockerKind::UnsupportedDynamicMode;
  std::string Subject;
  std::string Detail;
  std::vector<std::string> DependencyPath;

  [[nodiscard]] std::string PathText() const;

  [[nodiscard]] std::string Text() const;

  [[nodiscard]] std::string Message() const;

  friend bool operator==(const LifecycleBlocker &Left,
                         const LifecycleBlocker &Right);
};

[[nodiscard]] std::strong_ordering
CompareBlocker(const LifecycleBlocker &Left, const LifecycleBlocker &Right);

struct LifecycleSymbol final {
  SymbolKind Kind = SymbolKind::Namespace;
  std::string QualifiedName;
  std::string Signature;
  TypeId Type;
  TypeDescriptor Descriptor;

  std::string OwnershipText;

  std::string ModuleIdentity;
};

struct LifecycleDispatchSlot final {
  std::uint64_t Slot = 0;
  std::string QualifiedName;
  bool IsAvailable = false;
};

struct LifecycleUserdataValue final {
  std::string ClassQualifiedName;
  TypeId Type;
  std::uint64_t Nonce = 0;
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  bool IsPublished = false;

  bool MigrationAvailable = false;
  bool RemainsValid = false;

  [[nodiscard]] std::string Subject() const;
};

enum class LifecycleCacheKind : std::uint8_t {
  FrozenLookup,
  FrozenNamespace,
  FrozenModule,
  FrozenMetatable,
  LazyMemberValue,
  NativeIdentity
};

[[nodiscard]] std::string_view
LifecycleCacheKindText(LifecycleCacheKind Kind) noexcept;

struct LifecycleCacheEntry final {
  LifecycleCacheKind Kind = LifecycleCacheKind::FrozenLookup;
  std::string Subject;
};

struct LifecycleRootedReference final {
  std::string Subject;
  std::string Detail;
};

struct LifecycleRetainedGeneration final {
  std::uint64_t Number = 0;
  bool IsCurrent = false;
  std::size_t Invocations = 0;
  std::size_t UserdataCleanups = 0;
  std::size_t LifecycleJournals = 0;
};

struct LifecycleSubject final {
  std::vector<ModuleManifest> LoadedModules;
  std::vector<LifecycleSymbol> Symbols;
  std::vector<LifecycleDispatchSlot> DispatchSlots;
  std::vector<LifecycleUserdataValue> LiveUserdata;
  std::vector<LifecycleCacheEntry> Caches;
  std::vector<LifecycleRootedReference> RootedReferences;
  std::vector<LifecycleRetainedGeneration> RetainedGenerations;

  bool DynamicLifecycleEnabled = false;
  bool Frozen = false;

  [[nodiscard]] const ModuleManifest *
  FindLoaded(std::string_view Identity) const noexcept;
};

struct LifecycleRequest final {
  LifecycleOperation Operation = LifecycleOperation::Unload;
  std::string Identity;
  ModuleManifest Replacement;
  std::vector<LifecycleSymbol> ReplacementSymbols;

  [[nodiscard]] bool IsReplacement() const noexcept {
    return Operation == LifecycleOperation::Replacement;
  }
};

class LifecycleClosure final {
public:
  void Add(LifecycleAffectedKind Kind, std::string Subject, std::string Detail,
           std::uint64_t Ordinal = 0);

  [[nodiscard]] const std::vector<LifecycleAffectedItem> &All() const noexcept {
    return Items;
  }

  [[nodiscard]] std::vector<LifecycleAffectedItem>
  OfKind(LifecycleAffectedKind Kind) const;

  [[nodiscard]] std::size_t CountOfKind(LifecycleAffectedKind Kind) const;

  [[nodiscard]] bool Contains(LifecycleAffectedKind Kind,
                              std::string_view Subject) const;

  [[nodiscard]] std::size_t Size() const noexcept { return Items.size(); }
  [[nodiscard]] bool IsEmpty() const noexcept { return Items.empty(); }

  [[nodiscard]] std::vector<std::string> Text() const;

private:
  std::vector<LifecycleAffectedItem> Items;
};

struct LifecycleAnalysis final {
  LifecycleOperation Operation = LifecycleOperation::Unload;
  std::string Identity;
  LifecycleClosure Affected;

  std::vector<LifecycleBlocker> Blockers;

  std::vector<std::string> RemovedSubjects;

  std::vector<std::string> RetainedSubjects;

  std::vector<std::string> RemovedTypes;

  std::vector<LifecycleCacheEntry> InvalidatedCaches;

  [[nodiscard]] bool IsPermitted() const noexcept { return Blockers.empty(); }

  [[nodiscard]] bool HasBlocker(LifecycleBlockerKind Kind) const noexcept;

  [[nodiscard]] std::vector<std::string> BlockerText() const;

  [[nodiscard]] std::string Message() const;
};

[[nodiscard]] LifecycleAnalysis
AnalyzeLifecycleRequest(const LifecycleRequest &Request,
                        const LifecycleSubject &Subject);

struct LifecycleSubjectSources final {
  const ReflectionStorage *Reflection = nullptr;
  const ModuleRegistry *Modules = nullptr;
  const DispatchTable *Dispatch = nullptr;
  const OwnershipRegistry *Userdata = nullptr;
  const ClassRegistry *Classes = nullptr;
  const LazyPropertyCache *LazyValues = nullptr;
  const FreezeCacheStorage *FrozenCaches = nullptr;
  const UserdataIdentityCache *Identities = nullptr;

  bool DynamicLifecycleEnabled = false;
  bool Frozen = false;
};

[[nodiscard]] LifecycleSubject
DescribeLifecycleSubject(const LifecycleSubjectSources &Sources);

} // namespace Luna::Detail
