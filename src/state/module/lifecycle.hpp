#pragma once

// The analysis half of dynamic module lifecycle: what one unload or replacement
// would reach, and why it is refused.
//
// Nothing here mutates anything. Before a lifecycle operation may stage a
// single change, it has to answer two questions completely, and both answers
// are pure functions of one immutable description of the State:
//
//   1. What does the operation affect? Every function, namespace, type, live
//      userdata value, reflection record, cache entry, installed closure,
//      dependent module, rooted reference, and retained dispatch generation the
//      operation would reach is named in one canonically ordered closure.
//   2. May it proceed? Dependency constraints, canonical type and descriptor
//      equality, class and ownership compatibility, callable signature
//      compatibility, reflected declaration compatibility, and the explicit
//      userdata migration or continued-validity policy are each validated, and
//      every failure becomes one canonically ordered blocker.
//
// A single blocker refuses the whole operation. Because the analysis is const
// throughout and reports rather than acts, a refused request cannot leave a
// partial effect behind: there is no effect to leave.
//
// Dynamic lifecycle is not enabled yet. That is itself the first blocker every
// request receives today, which is exactly the deterministic load-only refusal
// Requirement 17.10 asks for, and it is reported alongside the complete closure
// so a caller still learns what the operation would have reached.
//
// Nothing here names a virtual machine, a stack index, a callable target, a
// metatable, or a native object address. Userdata is named by its class and its
// state-local nonce, generations by their number, and modules by their
// canonical `Identity@Version` key.

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

// Which lifecycle operation is being analyzed.
enum class LifecycleOperation : std::uint8_t { Unload, Replacement };

[[nodiscard]] std::string_view
LifecycleOperationText(LifecycleOperation Operation) noexcept;

// What kind of thing one lifecycle operation reaches. The declaration order is
// the canonical kind order of every closure enumeration, and it follows exactly
// the order Requirement 17.2 names.
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

// One thing the operation reaches. `Subject` is a canonical Luna-owned name and
// `Ordinal` is the numeric key of a subject that is counted rather than named,
// such as a dispatch generation, so ordering stays numeric where numbers are
// what the subject is.
struct LifecycleAffectedItem final {
  LifecycleAffectedKind Kind = LifecycleAffectedKind::Function;
  std::string Subject;
  std::string Detail;
  std::uint64_t Ordinal = 0;

  // Canonical one-line text: `kind|subject|detail`.
  [[nodiscard]] std::string Text() const;

  friend bool operator==(const LifecycleAffectedItem &Left,
                         const LifecycleAffectedItem &Right);
};

// Canonical affected order: kind, then numeric ordinal, then subject, then
// detail.
[[nodiscard]] std::strong_ordering
CompareAffected(const LifecycleAffectedItem &Left,
                const LifecycleAffectedItem &Right);

// Why one lifecycle operation is refused. The declaration order is the
// canonical blocker order: the dynamic-mode gate first, then the request
// itself, then dependency resolution, then declaration compatibility, then live
// state.
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

// One reason the operation may not proceed. A dependency blocker also carries
// the canonical dependency path that produced it, from the dependent module to
// the module the request names.
struct LifecycleBlocker final {
  LifecycleBlockerKind Kind = LifecycleBlockerKind::UnsupportedDynamicMode;
  std::string Subject;
  std::string Detail;
  std::vector<std::string> DependencyPath;

  // Canonical dependency path text, `->`-separated, or an empty string.
  [[nodiscard]] std::string PathText() const;

  // Canonical one-line text: `kind|subject|detail[|path]`.
  [[nodiscard]] std::string Text() const;

  // Human-readable sentence used by lifecycle diagnostics.
  [[nodiscard]] std::string Message() const;

  friend bool operator==(const LifecycleBlocker &Left,
                         const LifecycleBlocker &Right);
};

// Canonical blocker order: kind, then subject, then detail, then dependency
// path.
[[nodiscard]] std::strong_ordering
CompareBlocker(const LifecycleBlocker &Left, const LifecycleBlocker &Right);

// One canonical symbol of the described State, with everything compatibility
// validation compares. It is derived from the immutable reflection generation,
// so it carries no target and no address.
struct LifecycleSymbol final {
  SymbolKind Kind = SymbolKind::Namespace;
  std::string QualifiedName;
  std::string Signature;
  TypeId Type;
  TypeDescriptor Descriptor;

  // The canonical ownership text of a construction candidate or a member, as
  // reflection published it. Two declarations that differ here are not
  // ownership-compatible even when their signatures agree.
  std::string OwnershipText;

  // The module that owns this symbol, or an empty string when it is not
  // module-owned.
  std::string ModuleIdentity;
};

// One installed closure of the described State, named by the canonical path it
// dispatches for and the permanent slot it carries.
struct LifecycleDispatchSlot final {
  std::uint64_t Slot = 0;
  std::string QualifiedName;
  bool IsAvailable = false;
};

// One live exposed value of the described State. It is named by its class and
// its state-local nonce, never by an address.
struct LifecycleUserdataValue final {
  std::string ClassQualifiedName;
  TypeId Type;
  std::uint64_t Nonce = 0;
  OwnershipModel Ownership = OwnershipModel::Borrowed;
  bool IsPublished = false;

  // The explicit policy of this value across a replacement: whether native code
  // declared a migration for it, and whether it is declared to remain valid
  // when its class is retained unchanged. Neither is assumed.
  bool MigrationAvailable = false;
  bool RemainsValid = false;

  [[nodiscard]] std::string Subject() const;
};

// Which cache one entry belongs to.
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

// One reference to a symbol that is retained outside dispatch indirection, so
// it cannot simply resolve a new generation on its next use.
struct LifecycleRootedReference final {
  std::string Subject;
  std::string Detail;
};

// One dispatch generation the described State still has to keep readable.
struct LifecycleRetainedGeneration final {
  std::uint64_t Number = 0;
  bool IsCurrent = false;
  std::size_t Invocations = 0;
  std::size_t UserdataCleanups = 0;
  std::size_t LifecycleJournals = 0;
};

// The immutable description one lifecycle analysis reads. Building it observes
// the State; analyzing it never touches the State at all.
struct LifecycleSubject final {
  std::vector<ModuleManifest> LoadedModules;
  std::vector<LifecycleSymbol> Symbols;
  std::vector<LifecycleDispatchSlot> DispatchSlots;
  std::vector<LifecycleUserdataValue> LiveUserdata;
  std::vector<LifecycleCacheEntry> Caches;
  std::vector<LifecycleRootedReference> RootedReferences;
  std::vector<LifecycleRetainedGeneration> RetainedGenerations;

  // Whether this State supports dynamic lifecycle at all, and whether it is
  // frozen. Both are refusals rather than opinions: a State without dynamic
  // lifecycle stays load-only, and a frozen State accepts no mutation.
  bool DynamicLifecycleEnabled = false;
  bool Frozen = false;

  [[nodiscard]] const ModuleManifest *
  FindLoaded(std::string_view Identity) const noexcept;
};

// One lifecycle request. A replacement additionally carries the manifest it
// would install and the canonical declarations it would publish, because
// declaration, type, signature, and ownership compatibility can only be decided
// against the actual replacement symbols.
struct LifecycleRequest final {
  LifecycleOperation Operation = LifecycleOperation::Unload;
  std::string Identity;
  ModuleManifest Replacement;
  std::vector<LifecycleSymbol> ReplacementSymbols;

  [[nodiscard]] bool IsReplacement() const noexcept {
    return Operation == LifecycleOperation::Replacement;
  }
};

// The canonically ordered, deduplicated closure of one lifecycle operation.
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

  // Canonical one-line text per item, in canonical order.
  [[nodiscard]] std::vector<std::string> Text() const;

private:
  // Kept sorted by CompareAffected after every insertion, so enumeration never
  // depends on the order the analysis discovered things in.
  std::vector<LifecycleAffectedItem> Items;
};

// What one lifecycle request would reach and why it is refused. Producing it
// mutates nothing.
struct LifecycleAnalysis final {
  LifecycleOperation Operation = LifecycleOperation::Unload;
  std::string Identity;
  LifecycleClosure Affected;

  // Canonically ordered and deduplicated.
  std::vector<LifecycleBlocker> Blockers;

  [[nodiscard]] bool IsPermitted() const noexcept { return Blockers.empty(); }

  [[nodiscard]] bool HasBlocker(LifecycleBlockerKind Kind) const noexcept;

  [[nodiscard]] std::vector<std::string> BlockerText() const;

  // The deterministic lifecycle diagnostic: the operation, the module, and
  // every blocker in canonical order.
  [[nodiscard]] std::string Message() const;
};

// The whole analysis: closure first, then every compatibility and liveness
// validation, then canonical ordering. It reads `Subject` and never writes it.
[[nodiscard]] LifecycleAnalysis
AnalyzeLifecycleRequest(const LifecycleRequest &Request,
                        const LifecycleSubject &Subject);

// Everything one State description is read from. Every member is observed
// read-only; an absent member simply contributes nothing.
struct LifecycleSubjectSources final {
  const ReflectionStorage *Reflection = nullptr;
  const ModuleRegistry *Modules = nullptr;
  const DispatchTable *Dispatch = nullptr;
  const OwnershipRegistry *Userdata = nullptr;
  const ClassRegistry *Classes = nullptr;
  const LazyPropertyCache *LazyValues = nullptr;
  const FreezeCacheStorage *FrozenCaches = nullptr;

  bool DynamicLifecycleEnabled = false;
  bool Frozen = false;
};

// Describes one State for analysis without changing anything about it.
[[nodiscard]] LifecycleSubject
DescribeLifecycleSubject(const LifecycleSubjectSources &Sources);

} // namespace Luna::Detail
