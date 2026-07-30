// clang-format off
#include "state/module/lifecycle.hpp"

#include "state/dispatch/generation.hpp"
#include "state/freeze/cache.hpp"
#include "state/module/registry.hpp"
#include "state/reflection/storage.hpp"
#include "state/userdata/class_registry.hpp"
#include "state/userdata/identity_cache.hpp"
#include "state/userdata/lazy_cache.hpp"
#include "state/userdata/ownership.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] bool PathContains(std::string_view Scope,
                                std::string_view Candidate) noexcept {
  if (Scope.empty() || Candidate.size() < Scope.size())
    return false;
  if (Candidate.compare(0, Scope.size(), Scope) != 0)
    return false;
  if (Candidate.size() == Scope.size())
    return true;
  return Candidate[Scope.size()] == '.';
}

[[nodiscard]] bool IsCallableKind(SymbolKind Kind) noexcept {
  switch (Kind) {
  case SymbolKind::OverloadSet:
  case SymbolKind::FunctionCandidate:
  case SymbolKind::Constructor:
  case SymbolKind::Factory:
  case SymbolKind::Method:
  case SymbolKind::StaticMethod:
  case SymbolKind::Operator:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool IsClassKind(SymbolKind Kind) noexcept {
  switch (Kind) {
  case SymbolKind::Class:
  case SymbolKind::Constructor:
  case SymbolKind::Factory:
  case SymbolKind::Method:
  case SymbolKind::StaticMethod:
  case SymbolKind::Property:
  case SymbolKind::Field:
  case SymbolKind::Operator:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] LifecycleAffectedKind AffectedKindOf(SymbolKind Kind) noexcept {
  switch (Kind) {
  case SymbolKind::Namespace:
  case SymbolKind::Module:
    return LifecycleAffectedKind::Namespace;
  case SymbolKind::Class:
  case SymbolKind::Enumeration:
  case SymbolKind::Type:
    return LifecycleAffectedKind::Type;
  default:
    break;
  }
  return IsCallableKind(Kind) ? LifecycleAffectedKind::Function
                              : LifecycleAffectedKind::ReflectionRecord;
}

[[nodiscard]] std::string GenerationSubject(std::uint64_t Number) {
  return std::string("generation ").append(std::to_string(Number));
}

[[nodiscard]] std::string
ConstraintText(const std::vector<VersionConstraint> &Constraints) {
  std::string Text;
  for (const VersionConstraint &Constraint : Constraints) {
    if (!Text.empty())
      Text.append(", ");
    Text.append(Constraint.ToString());
  }
  return Text.empty() ? std::string("no constraint") : Text;
}

[[nodiscard]] std::map<std::string, std::vector<std::string>>
CollectDependents(const LifecycleSubject &Subject, std::string_view Identity) {
  std::map<std::string, std::vector<std::string>> Dependents;
  std::vector<std::string> Frontier{std::string(Identity)};

  while (!Frontier.empty()) {
    const std::string Required = std::move(Frontier.back());
    Frontier.pop_back();

    std::vector<const ModuleManifest *> Ordered;
    Ordered.reserve(Subject.LoadedModules.size());
    for (const ModuleManifest &Loaded : Subject.LoadedModules)
      Ordered.push_back(&Loaded);
    std::sort(Ordered.begin(), Ordered.end(),
              [](const ModuleManifest *Left, const ModuleManifest *Right) {
                return CompareManifest(*Left, *Right) ==
                       std::strong_ordering::less;
              });

    for (const ModuleManifest *Loaded : Ordered) {
      if (Loaded->Identity() == Identity || Loaded->Identity() == Required)
        continue;
      if (Loaded->FindDependency(Required) == nullptr)
        continue;
      if (Dependents.find(Loaded->Identity()) != Dependents.end())
        continue;

      std::vector<std::string> Path{Loaded->Key()};
      if (Required != Identity) {
        const auto Reached = Dependents.find(Required);
        if (Reached != Dependents.end())
          Path.insert(Path.end(), Reached->second.begin(),
                      Reached->second.end());
      } else {
        const ModuleManifest *Target = Subject.FindLoaded(Identity);
        Path.push_back(Target ? Target->Key() : std::string(Identity));
      }
      Dependents.emplace(Loaded->Identity(), std::move(Path));
      Frontier.push_back(Loaded->Identity());
    }
  }
  return Dependents;
}

} // namespace

std::string_view LifecycleOperationText(LifecycleOperation Operation) noexcept {
  switch (Operation) {
  case LifecycleOperation::Unload:
    return "unload";
  case LifecycleOperation::Replacement:
    return "replacement";
  }
  return "invalid";
}

std::string_view
LifecycleAffectedKindText(LifecycleAffectedKind Kind) noexcept {
  switch (Kind) {
  case LifecycleAffectedKind::Function:
    return "function";
  case LifecycleAffectedKind::Namespace:
    return "namespace";
  case LifecycleAffectedKind::Type:
    return "type";
  case LifecycleAffectedKind::Userdata:
    return "userdata";
  case LifecycleAffectedKind::ReflectionRecord:
    return "reflection_record";
  case LifecycleAffectedKind::Cache:
    return "cache";
  case LifecycleAffectedKind::Closure:
    return "closure";
  case LifecycleAffectedKind::DependentModule:
    return "dependent_module";
  case LifecycleAffectedKind::RootedReference:
    return "rooted_reference";
  case LifecycleAffectedKind::RetainedGeneration:
    return "retained_generation";
  }
  return "invalid";
}

std::string_view LifecycleBlockerKindText(LifecycleBlockerKind Kind) noexcept {
  switch (Kind) {
  case LifecycleBlockerKind::UnsupportedDynamicMode:
    return "unsupported-dynamic-mode";
  case LifecycleBlockerKind::UnknownModule:
    return "unknown-module";
  case LifecycleBlockerKind::InvalidReplacementManifest:
    return "invalid-replacement-manifest";
  case LifecycleBlockerKind::IdentityMismatch:
    return "identity-mismatch";
  case LifecycleBlockerKind::MissingDependency:
    return "missing-dependency";
  case LifecycleBlockerKind::UnsatisfiedDependencyConstraint:
    return "unsatisfied-dependency-constraint";
  case LifecycleBlockerKind::DependentModule:
    return "dependent-module";
  case LifecycleBlockerKind::IncompatibleDeclaration:
    return "incompatible-declaration";
  case LifecycleBlockerKind::IncompatibleType:
    return "incompatible-type";
  case LifecycleBlockerKind::IncompatibleCallableSignature:
    return "incompatible-callable-signature";
  case LifecycleBlockerKind::IncompatibleClassOwnership:
    return "incompatible-class-ownership";
  case LifecycleBlockerKind::LiveUserdata:
    return "live-userdata";
  case LifecycleBlockerKind::UnavailableUserdataMigration:
    return "unavailable-userdata-migration";
  case LifecycleBlockerKind::RootedReference:
    return "rooted-reference";
  }
  return "invalid";
}

std::string_view LifecycleCacheKindText(LifecycleCacheKind Kind) noexcept {
  switch (Kind) {
  case LifecycleCacheKind::FrozenLookup:
    return "frozen_lookup";
  case LifecycleCacheKind::FrozenNamespace:
    return "frozen_namespace";
  case LifecycleCacheKind::FrozenModule:
    return "frozen_module";
  case LifecycleCacheKind::FrozenMetatable:
    return "frozen_metatable";
  case LifecycleCacheKind::LazyMemberValue:
    return "lazy_member_value";
  case LifecycleCacheKind::NativeIdentity:
    return "native_identity";
  }
  return "invalid";
}

std::string LifecycleAffectedItem::Text() const {
  std::string Result(LifecycleAffectedKindText(Kind));
  Result.push_back('|');
  Result.append(Subject);
  Result.push_back('|');
  Result.append(Detail);
  return Result;
}

bool operator==(const LifecycleAffectedItem &Left,
                const LifecycleAffectedItem &Right) {
  return Left.Kind == Right.Kind && Left.Ordinal == Right.Ordinal &&
         Left.Subject == Right.Subject && Left.Detail == Right.Detail;
}

std::strong_ordering CompareAffected(const LifecycleAffectedItem &Left,
                                     const LifecycleAffectedItem &Right) {
  if (Left.Kind != Right.Kind)
    return static_cast<std::uint8_t>(Left.Kind) <=>
           static_cast<std::uint8_t>(Right.Kind);
  if (Left.Ordinal != Right.Ordinal)
    return Left.Ordinal <=> Right.Ordinal;
  if (const std::strong_ordering Subjects =
          Left.Subject.compare(Right.Subject) <=> 0;
      Subjects != std::strong_ordering::equal)
    return Subjects;
  return Left.Detail.compare(Right.Detail) <=> 0;
}

std::string LifecycleBlocker::PathText() const {
  std::string Text;
  for (const std::string &Step : DependencyPath) {
    if (!Text.empty())
      Text.append(" -> ");
    Text.append(Step);
  }
  return Text;
}

std::string LifecycleBlocker::Text() const {
  std::string Result(LifecycleBlockerKindText(Kind));
  Result.push_back('|');
  Result.append(Subject);
  Result.push_back('|');
  Result.append(Detail);
  const std::string Path = PathText();
  if (!Path.empty()) {
    Result.push_back('|');
    Result.append(Path);
  }
  return Result;
}

std::string LifecycleBlocker::Message() const {
  std::string Result(LifecycleBlockerKindText(Kind));
  if (!Subject.empty()) {
    Result.append(" for '");
    Result.append(Subject);
    Result.push_back('\'');
  }
  if (!Detail.empty()) {
    Result.append(": ");
    Result.append(Detail);
  }
  const std::string Path = PathText();
  if (!Path.empty()) {
    Result.append(" (dependency path ");
    Result.append(Path);
    Result.push_back(')');
  }
  return Result;
}

bool operator==(const LifecycleBlocker &Left, const LifecycleBlocker &Right) {
  return Left.Kind == Right.Kind && Left.Subject == Right.Subject &&
         Left.Detail == Right.Detail &&
         Left.DependencyPath == Right.DependencyPath;
}

std::strong_ordering CompareBlocker(const LifecycleBlocker &Left,
                                    const LifecycleBlocker &Right) {
  if (Left.Kind != Right.Kind)
    return static_cast<std::uint8_t>(Left.Kind) <=>
           static_cast<std::uint8_t>(Right.Kind);
  if (const std::strong_ordering Subjects =
          Left.Subject.compare(Right.Subject) <=> 0;
      Subjects != std::strong_ordering::equal)
    return Subjects;
  if (const std::strong_ordering Details =
          Left.Detail.compare(Right.Detail) <=> 0;
      Details != std::strong_ordering::equal)
    return Details;
  return Left.PathText().compare(Right.PathText()) <=> 0;
}

std::string LifecycleUserdataValue::Subject() const {
  std::string Result = ClassQualifiedName.empty()
                           ? std::string("<unregistered class>")
                           : ClassQualifiedName;
  Result.push_back('#');
  Result.append(std::to_string(Nonce));
  return Result;
}

const ModuleManifest *
LifecycleSubject::FindLoaded(std::string_view Identity) const noexcept {
  for (const ModuleManifest &Loaded : LoadedModules) {
    if (Loaded.Identity() == Identity)
      return &Loaded;
  }
  return nullptr;
}

void LifecycleClosure::Add(LifecycleAffectedKind Kind, std::string Subject,
                           std::string Detail, std::uint64_t Ordinal) {
  LifecycleAffectedItem Item;
  Item.Kind = Kind;
  Item.Subject = std::move(Subject);
  Item.Detail = std::move(Detail);
  Item.Ordinal = Ordinal;

  const auto Position = std::lower_bound(
      Items.begin(), Items.end(), Item,
      [](const LifecycleAffectedItem &Left,
         const LifecycleAffectedItem &Right) {
        return CompareAffected(Left, Right) == std::strong_ordering::less;
      });
  if (Position != Items.end() && *Position == Item)
    return;
  Items.insert(Position, std::move(Item));
}

std::vector<LifecycleAffectedItem>
LifecycleClosure::OfKind(LifecycleAffectedKind Kind) const {
  std::vector<LifecycleAffectedItem> Selected;
  for (const LifecycleAffectedItem &Item : Items) {
    if (Item.Kind == Kind)
      Selected.push_back(Item);
  }
  return Selected;
}

std::size_t LifecycleClosure::CountOfKind(LifecycleAffectedKind Kind) const {
  std::size_t Count = 0;
  for (const LifecycleAffectedItem &Item : Items) {
    if (Item.Kind == Kind)
      ++Count;
  }
  return Count;
}

bool LifecycleClosure::Contains(LifecycleAffectedKind Kind,
                                std::string_view Subject) const {
  for (const LifecycleAffectedItem &Item : Items) {
    if (Item.Kind == Kind && Item.Subject == Subject)
      return true;
  }
  return false;
}

std::vector<std::string> LifecycleClosure::Text() const {
  std::vector<std::string> Lines;
  Lines.reserve(Items.size());
  for (const LifecycleAffectedItem &Item : Items)
    Lines.push_back(Item.Text());
  return Lines;
}

bool LifecycleAnalysis::HasBlocker(LifecycleBlockerKind Kind) const noexcept {
  for (const LifecycleBlocker &Blocker : Blockers) {
    if (Blocker.Kind == Kind)
      return true;
  }
  return false;
}

std::vector<std::string> LifecycleAnalysis::BlockerText() const {
  std::vector<std::string> Lines;
  Lines.reserve(Blockers.size());
  for (const LifecycleBlocker &Blocker : Blockers)
    Lines.push_back(Blocker.Text());
  return Lines;
}

std::string LifecycleAnalysis::Message() const {
  std::string Result("module ");
  Result.append(LifecycleOperationText(Operation));
  if (!Identity.empty()) {
    Result.append(" of '");
    Result.append(Identity);
    Result.push_back('\'');
  }
  if (Blockers.empty()) {
    Result.append(" has no blocker");
    return Result;
  }
  Result.append(" is refused by ");
  Result.append(std::to_string(Blockers.size()));
  Result.append(Blockers.size() == 1 ? " blocker" : " blockers");
  for (const LifecycleBlocker &Blocker : Blockers) {
    Result.append("; ");
    Result.append(Blocker.Message());
  }
  return Result;
}

namespace {

[[nodiscard]] bool IsMemberKind(SymbolKind Kind) noexcept {
  switch (Kind) {
  case SymbolKind::Constructor:
  case SymbolKind::Factory:
  case SymbolKind::Method:
  case SymbolKind::StaticMethod:
  case SymbolKind::Property:
  case SymbolKind::Field:
  case SymbolKind::Operator:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] std::string OwningClassName(std::string_view QualifiedName) {
  const std::size_t Separator = QualifiedName.rfind('.');
  if (Separator == std::string_view::npos)
    return std::string();
  return std::string(QualifiedName.substr(0, Separator));
}

[[nodiscard]] bool NamesOverlap(std::string_view Left,
                                std::string_view Right) noexcept {
  return Left == Right || PathContains(Left, Right) ||
         PathContains(Right, Left);
}

[[nodiscard]] std::string
JoinDependencyPath(const std::vector<std::string> &Path) {
  std::string Text;
  for (const std::string &Step : Path) {
    if (!Text.empty())
      Text.append(" -> ");
    Text.append(Step);
  }
  return Text;
}

[[nodiscard]] std::string
RetainedGenerationDetail(const LifecycleRetainedGeneration &Held) {
  std::string Detail(Held.IsCurrent ? "current" : "superseded");
  Detail.append("; invocations=");
  Detail.append(std::to_string(Held.Invocations));
  Detail.append(", userdata_cleanups=");
  Detail.append(std::to_string(Held.UserdataCleanups));
  Detail.append(", lifecycle_journals=");
  Detail.append(std::to_string(Held.LifecycleJournals));
  return Detail;
}

[[nodiscard]] std::string UserdataDetail(const LifecycleUserdataValue &Value) {
  std::string Detail(OwnershipModelText(Value.Ownership));
  Detail.append(Value.IsPublished ? ", published" : ", unpublished");
  if (Value.RemainsValid)
    Detail.append(", remains valid");
  if (Value.MigrationAvailable)
    Detail.append(", migration available");
  return Detail;
}

[[nodiscard]] std::string ReferencedSubject(std::string_view Subject) {
  const std::size_t Marker = Subject.find('#');
  return std::string(
      Marker == std::string_view::npos ? Subject : Subject.substr(0, Marker));
}

[[nodiscard]] bool Contains(const std::set<std::string> &Names,
                            const std::string &Candidate) {
  return Names.find(Candidate) != Names.end();
}

[[nodiscard]] bool OverlapsAny(const std::set<std::string> &Names,
                               std::string_view Candidate) {
  for (const std::string &Name : Names) {
    if (NamesOverlap(Name, Candidate))
      return true;
  }
  return false;
}

[[nodiscard]] std::string
ReflectedOwnershipText(const ReflectionRecordFields &Record) {
  std::string Text = Record.OwnershipResult;
  Text.push_back('|');
  Text.append(Record.AllocatorPolicy);
  Text.push_back('|');
  Text.append(Record.MemberOwnershipText);
  return Text;
}

[[nodiscard]] bool RecordIsLive(const OwnershipRecord &Record) noexcept {
  switch (Record.Lifetime) {
  case LifetimeState::Allocated:
  case LifetimeState::Constructed:
  case LifetimeState::Published:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] std::string ClassNameOf(const ClassRegistry *Classes,
                                      const SymbolId &ClassSymbol,
                                      const TypeId &DynamicType) {
  if (Classes == nullptr)
    return std::string();
  const RegisteredClass *Registered = Classes->FindBySymbol(ClassSymbol);
  if (Registered == nullptr)
    Registered = Classes->Find(DynamicType);
  return Registered != nullptr ? Registered->QualifiedName : std::string();
}

} // namespace

LifecycleAnalysis AnalyzeLifecycleRequest(const LifecycleRequest &Request,
                                          const LifecycleSubject &Subject) {
  LifecycleAnalysis Analysis;
  Analysis.Operation = Request.Operation;
  Analysis.Identity = Request.Identity;

  const auto Refuse = [&Analysis](LifecycleBlockerKind Kind, std::string Named,
                                  std::string Detail,
                                  std::vector<std::string> Path) {
    LifecycleBlocker Blocker;
    Blocker.Kind = Kind;
    Blocker.Subject = std::move(Named);
    Blocker.Detail = std::move(Detail);
    Blocker.DependencyPath = std::move(Path);
    Analysis.Blockers.push_back(std::move(Blocker));
  };

  if (!Subject.DynamicLifecycleEnabled)
    Refuse(LifecycleBlockerKind::UnsupportedDynamicMode, Request.Identity,
           "dynamic module lifecycle is unsupported for this State, which "
           "remains load-only",
           {});

  const ModuleManifest *Target = Subject.FindLoaded(Request.Identity);
  if (Target == nullptr)
    Refuse(LifecycleBlockerKind::UnknownModule, Request.Identity,
           "the module is not loaded", {});

  const bool Replacing = Request.IsReplacement();
  bool ReplacementDescribesModule = Replacing;
  if (Replacing && !Request.Replacement.IsValid()) {
    ReplacementDescribesModule = false;
    Refuse(LifecycleBlockerKind::InvalidReplacementManifest, Request.Identity,
           std::string("the replacement manifest is invalid (")
               .append(ModuleManifestStatusText(Request.Replacement.Status()))
               .append(")"),
           {});
  } else if (Replacing && Request.Replacement.Identity() != Request.Identity) {
    ReplacementDescribesModule = false;
    Refuse(
        LifecycleBlockerKind::IdentityMismatch, Request.Replacement.Identity(),
        std::string("the replacement manifest declares a module other than '")
            .append(Request.Identity)
            .append("'"),
        {});
  }

  if (ReplacementDescribesModule) {
    std::vector<const ModuleDependency *> Required;
    Required.reserve(Request.Replacement.Dependencies().size());
    for (const ModuleDependency &Dependency :
         Request.Replacement.Dependencies())
      Required.push_back(&Dependency);
    std::sort(Required.begin(), Required.end(),
              [](const ModuleDependency *Left, const ModuleDependency *Right) {
                return Left->Identity < Right->Identity;
              });
    for (const ModuleDependency *Dependency : Required) {
      const ModuleManifest *Provider = Subject.FindLoaded(Dependency->Identity);
      if (Provider == nullptr) {
        Refuse(LifecycleBlockerKind::MissingDependency, Dependency->Identity,
               "the replacement requires a module that is not loaded",
               {Request.Replacement.Key(), Dependency->Identity});
        continue;
      }
      for (const VersionConstraint &Constraint : Dependency->Constraints) {
        if (Constraint.IsSatisfiedBy(Provider->Version()))
          continue;
        Refuse(LifecycleBlockerKind::UnsatisfiedDependencyConstraint,
               Dependency->Identity,
               std::string("the replacement requires ")
                   .append(ConstraintText(Dependency->Constraints))
                   .append(" but version ")
                   .append(Provider->Version().ToString())
                   .append(" is loaded"),
               {Request.Replacement.Key(), Provider->Key()});
        break;
      }
    }
  }

  const std::map<std::string, std::vector<std::string>> Dependents =
      Target != nullptr ? CollectDependents(Subject, Request.Identity)
                        : std::map<std::string, std::vector<std::string>>();
  for (const auto &[DependentIdentity, Path] : Dependents) {
    Analysis.Affected.Add(LifecycleAffectedKind::DependentModule,
                          DependentIdentity, JoinDependencyPath(Path));
    if (!Replacing) {
      Refuse(LifecycleBlockerKind::DependentModule, DependentIdentity,
             "the dependent module remains loaded", Path);
      continue;
    }
    if (!ReplacementDescribesModule)
      continue;
    const ModuleManifest *Dependent = Subject.FindLoaded(DependentIdentity);
    const ModuleDependency *Requirement =
        Dependent != nullptr ? Dependent->FindDependency(Request.Identity)
                             : nullptr;
    if (Requirement == nullptr)
      continue;
    for (const VersionConstraint &Constraint : Requirement->Constraints) {
      if (Constraint.IsSatisfiedBy(Request.Replacement.Version()))
        continue;
      Refuse(LifecycleBlockerKind::UnsatisfiedDependencyConstraint,
             DependentIdentity,
             std::string("the dependent module requires ")
                 .append(ConstraintText(Requirement->Constraints))
                 .append(" of '")
                 .append(Request.Identity)
                 .append("' but the replacement declares version ")
                 .append(Request.Replacement.Version().ToString()),
             Path);
      break;
    }
  }

  std::map<std::string, const LifecycleSymbol *, std::less<>> Declared;
  if (Replacing) {
    for (const LifecycleSymbol &Symbol : Request.ReplacementSymbols)
      Declared.emplace(Symbol.QualifiedName, &Symbol);
  }

  std::vector<const LifecycleSymbol *> Owned;
  if (!Request.Identity.empty()) {
    for (const LifecycleSymbol &Symbol : Subject.Symbols) {
      if (Symbol.ModuleIdentity == Request.Identity)
        Owned.push_back(&Symbol);
    }
  }
  std::sort(Owned.begin(), Owned.end(),
            [](const LifecycleSymbol *Left, const LifecycleSymbol *Right) {
              if (Left->QualifiedName != Right->QualifiedName)
                return Left->QualifiedName < Right->QualifiedName;
              return static_cast<int>(Left->Kind) <
                     static_cast<int>(Right->Kind);
            });

  std::set<std::string> OwnedNames;
  std::set<std::string> RemovedNames;
  std::set<std::string> RetainedNames;
  std::set<std::string> IncompatibleNames;
  std::set<std::string> RemovedClasses;
  std::set<std::string> IncompatibleClasses;
  std::set<std::string> RemovedTypes;

  for (const LifecycleSymbol *Symbol : Owned) {
    OwnedNames.insert(Symbol->QualifiedName);

    const LifecycleSymbol *Next = nullptr;
    if (Replacing) {
      const auto Found = Declared.find(Symbol->QualifiedName);
      if (Found != Declared.end())
        Next = Found->second;
    }

    const bool Removed = Next == nullptr;
    bool Incompatible = false;
    if (Next != nullptr) {
      if (Next->Kind != Symbol->Kind) {
        Incompatible = true;
        Refuse(LifecycleBlockerKind::IncompatibleDeclaration,
               Symbol->QualifiedName,
               std::string("the replacement declares '")
                   .append(SymbolKindText(Next->Kind))
                   .append("' instead of '")
                   .append(SymbolKindText(Symbol->Kind))
                   .append("'"),
               {});
      }
      if (!(Next->Type == Symbol->Type)) {
        Incompatible = true;
        Refuse(LifecycleBlockerKind::IncompatibleType, Symbol->QualifiedName,
               "the canonical type identity changed", {});
      } else if (!(Next->Descriptor == Symbol->Descriptor)) {
        Incompatible = true;
        Refuse(LifecycleBlockerKind::IncompatibleType, Symbol->QualifiedName,
               "the canonical type descriptor changed", {});
      }
      if (IsCallableKind(Symbol->Kind) &&
          Next->Signature != Symbol->Signature) {
        Incompatible = true;
        Refuse(LifecycleBlockerKind::IncompatibleCallableSignature,
               Symbol->QualifiedName,
               std::string("the replacement declares signature '")
                   .append(Next->Signature)
                   .append("' instead of '")
                   .append(Symbol->Signature)
                   .append("'"),
               {});
      }
      if (IsClassKind(Symbol->Kind) &&
          Next->OwnershipText != Symbol->OwnershipText) {
        Incompatible = true;
        Refuse(LifecycleBlockerKind::IncompatibleClassOwnership,
               Symbol->QualifiedName,
               std::string("the replacement declares ownership '")
                   .append(Next->OwnershipText)
                   .append("' instead of '")
                   .append(Symbol->OwnershipText)
                   .append("'"),
               {});
      }
    }

    const std::string_view Standing =
        Removed ? "removed"
                : (Incompatible ? "incompatible" : "retained compatibly");
    Analysis.Affected.Add(AffectedKindOf(Symbol->Kind), Symbol->QualifiedName,
                          std::string(Standing));
    Analysis.Affected.Add(
        LifecycleAffectedKind::ReflectionRecord, Symbol->QualifiedName,
        std::string(SymbolKindText(Symbol->Kind)).append(" ").append(Standing));

    if (Removed) {
      RemovedNames.insert(Symbol->QualifiedName);
      if (AffectedKindOf(Symbol->Kind) == LifecycleAffectedKind::Type)
        RemovedTypes.insert(Symbol->QualifiedName);
    } else {
      RetainedNames.insert(Symbol->QualifiedName);
      if (Incompatible)
        IncompatibleNames.insert(Symbol->QualifiedName);
    }

    const std::string ClassName =
        Symbol->Kind == SymbolKind::Class
            ? Symbol->QualifiedName
            : (IsMemberKind(Symbol->Kind)
                   ? OwningClassName(Symbol->QualifiedName)
                   : std::string());
    if (ClassName.empty())
      continue;
    if (Removed && (Symbol->Kind == SymbolKind::Class || !Replacing))
      RemovedClasses.insert(ClassName);
    else if (Removed || Incompatible)
      IncompatibleClasses.insert(ClassName);
  }

  for (const auto &[Name, Symbol] : Declared) {
    if (Contains(OwnedNames, Name))
      continue;
    Analysis.Affected.Add(AffectedKindOf(Symbol->Kind), Name, "added");
    Analysis.Affected.Add(
        LifecycleAffectedKind::ReflectionRecord, Name,
        std::string(SymbolKindText(Symbol->Kind)).append(" added"));
  }

  for (const LifecycleDispatchSlot &Slot : Subject.DispatchSlots) {
    bool Matched = false;
    bool Unavailable = false;
    for (const std::string &Name : OwnedNames) {
      if (Name != Slot.QualifiedName && !PathContains(Name, Slot.QualifiedName))
        continue;
      Matched = true;
      if (Contains(RemovedNames, Name))
        Unavailable = true;
    }
    if (!Matched)
      continue;
    Analysis.Affected.Add(LifecycleAffectedKind::Closure, Slot.QualifiedName,
                          Unavailable ? "becomes an unavailable entry"
                                      : "resolves the published generation",
                          Slot.Slot);
  }

  bool AffectsUserdata = false;
  for (const LifecycleUserdataValue &Value : Subject.LiveUserdata) {
    if (Value.ClassQualifiedName.empty())
      continue;
    if (!OverlapsAny(OwnedNames, Value.ClassQualifiedName))
      continue;
    AffectsUserdata = true;
    Analysis.Affected.Add(LifecycleAffectedKind::Userdata, Value.Subject(),
                          UserdataDetail(Value), Value.Nonce);

    if (Contains(RemovedClasses, Value.ClassQualifiedName) ||
        Contains(RemovedNames, Value.ClassQualifiedName)) {
      Refuse(LifecycleBlockerKind::LiveUserdata, Value.Subject(),
             std::string("live userdata prevents removing class '")
                 .append(Value.ClassQualifiedName)
                 .append("'"),
             {});
      continue;
    }
    if (Contains(IncompatibleClasses, Value.ClassQualifiedName) ||
        Contains(IncompatibleNames, Value.ClassQualifiedName)) {
      if (!Value.MigrationAvailable)
        Refuse(LifecycleBlockerKind::UnavailableUserdataMigration,
               Value.Subject(),
               std::string("no migration is available for incompatible class '")
                   .append(Value.ClassQualifiedName)
                   .append("'"),
               {});
      continue;
    }
    if (!Value.RemainsValid && !Value.MigrationAvailable)
      Refuse(LifecycleBlockerKind::UnavailableUserdataMigration,
             Value.Subject(),
             std::string("no continued-validity or migration policy is "
                         "declared for class '")
                 .append(Value.ClassQualifiedName)
                 .append("'"),
             {});
  }

  for (const LifecycleRootedReference &Reference : Subject.RootedReferences) {
    const std::string Referenced = ReferencedSubject(Reference.Subject);
    if (!OverlapsAny(OwnedNames, Referenced))
      continue;
    Analysis.Affected.Add(LifecycleAffectedKind::RootedReference,
                          Reference.Subject, Reference.Detail);
    if (!Contains(RemovedNames, Referenced) &&
        !Contains(RemovedClasses, Referenced) &&
        !Contains(IncompatibleNames, Referenced) &&
        !Contains(IncompatibleClasses, Referenced))
      continue;
    Refuse(LifecycleBlockerKind::RootedReference, Reference.Subject,
           Reference.Detail.empty()
               ? std::string("a rooted reference to '")
                     .append(Referenced)
                     .append("' cannot be invalidated safely")
               : Reference.Detail,
           {});
  }

  for (const LifecycleCacheEntry &Entry : Subject.Caches) {
    bool Matched = Entry.Subject == Request.Identity ||
                   OverlapsAny(OwnedNames, Entry.Subject);
    if (!Matched && AffectsUserdata &&
        (Entry.Kind == LifecycleCacheKind::LazyMemberValue ||
         Entry.Kind == LifecycleCacheKind::NativeIdentity))
      Matched = true;
    if (!Matched)
      continue;
    Analysis.Affected.Add(LifecycleAffectedKind::Cache, Entry.Subject,
                          std::string(LifecycleCacheKindText(Entry.Kind)));
    Analysis.InvalidatedCaches.push_back(Entry);
  }
  std::sort(
      Analysis.InvalidatedCaches.begin(), Analysis.InvalidatedCaches.end(),
      [](const LifecycleCacheEntry &Left, const LifecycleCacheEntry &Right) {
        if (Left.Kind != Right.Kind)
          return static_cast<std::uint8_t>(Left.Kind) <
                 static_cast<std::uint8_t>(Right.Kind);
        return Left.Subject < Right.Subject;
      });

  if (Target != nullptr) {
    for (const LifecycleRetainedGeneration &Held : Subject.RetainedGenerations)
      Analysis.Affected.Add(LifecycleAffectedKind::RetainedGeneration,
                            GenerationSubject(Held.Number),
                            RetainedGenerationDetail(Held), Held.Number);
  }

  Analysis.RemovedSubjects.assign(RemovedNames.begin(), RemovedNames.end());
  Analysis.RetainedSubjects.assign(RetainedNames.begin(), RetainedNames.end());
  Analysis.RemovedTypes.assign(RemovedTypes.begin(), RemovedTypes.end());

  std::sort(Analysis.Blockers.begin(), Analysis.Blockers.end(),
            [](const LifecycleBlocker &Left, const LifecycleBlocker &Right) {
              return CompareBlocker(Left, Right) == std::strong_ordering::less;
            });
  Analysis.Blockers.erase(
      std::unique(Analysis.Blockers.begin(), Analysis.Blockers.end()),
      Analysis.Blockers.end());
  return Analysis;
}
LifecycleSubject
DescribeLifecycleSubject(const LifecycleSubjectSources &Sources) {
  LifecycleSubject Subject;
  Subject.DynamicLifecycleEnabled = Sources.DynamicLifecycleEnabled;
  Subject.Frozen = Sources.Frozen;

  if (Sources.Modules != nullptr) {
    for (const ModuleManifest *Loaded : Sources.Modules->LoadedModules()) {
      if (Loaded != nullptr)
        Subject.LoadedModules.push_back(*Loaded);
    }
    std::sort(Subject.LoadedModules.begin(), Subject.LoadedModules.end(),
              [](const ModuleManifest &Left, const ModuleManifest &Right) {
                return CompareManifest(Left, Right) ==
                       std::strong_ordering::less;
              });
  }

  if (Sources.Reflection != nullptr) {
    const ReflectionStorage &Storage = *Sources.Reflection;
    for (const std::size_t Index : Storage.AllOrder()) {
      const ReflectionRecordFields *Record = Storage.RecordAt(Index);
      if (Record == nullptr)
        continue;
      LifecycleSymbol Symbol;
      Symbol.Kind = Record->Kind;
      Symbol.QualifiedName = Record->QualifiedName;
      Symbol.Signature = Record->Signature;
      Symbol.Type = Record->Type;
      Symbol.Descriptor = Record->Descriptor;
      Symbol.OwnershipText = ReflectedOwnershipText(*Record);
      if (Record->Module) {
        const ReflectionModuleFields *Owner = Storage.ModuleAt(*Record->Module);
        if (Owner != nullptr)
          Symbol.ModuleIdentity = Owner->Identity;
      }
      Subject.Symbols.push_back(std::move(Symbol));
    }
  }

  if (Sources.Dispatch != nullptr) {
    const std::shared_ptr<const DispatchGeneration> Current =
        Sources.Dispatch->Capture();
    if (Current) {
      for (const DispatchEntry &Entry : Current->All()) {
        LifecycleDispatchSlot Slot;
        Slot.Slot = Entry.Slot.Value;
        Slot.QualifiedName = Entry.QualifiedName;
        Slot.IsAvailable = Entry.IsAvailable();
        Subject.DispatchSlots.push_back(std::move(Slot));
      }
      std::sort(Subject.DispatchSlots.begin(), Subject.DispatchSlots.end(),
                [](const LifecycleDispatchSlot &Left,
                   const LifecycleDispatchSlot &Right) {
                  return Left.Slot < Right.Slot;
                });

      LifecycleRetainedGeneration Live;
      Live.Number = Current->Generation();
      Live.IsCurrent = true;
      Live.Invocations =
          Sources.Dispatch->RetainerCount(DispatchRetainer::Invocation);
      Live.UserdataCleanups =
          Sources.Dispatch->RetainerCount(DispatchRetainer::UserdataCleanup);
      Live.LifecycleJournals =
          Sources.Dispatch->RetainerCount(DispatchRetainer::LifecycleJournal);
      Subject.RetainedGenerations.push_back(Live);
    }

    for (const std::uint64_t Number :
         Sources.Dispatch->RetainedGenerationNumbers()) {
      LifecycleRetainedGeneration Held;
      Held.Number = Number;
      Subject.RetainedGenerations.push_back(Held);
    }
    std::sort(Subject.RetainedGenerations.begin(),
              Subject.RetainedGenerations.end(),
              [](const LifecycleRetainedGeneration &Left,
                 const LifecycleRetainedGeneration &Right) {
                return Left.Number < Right.Number;
              });
  }

  if (Sources.Userdata != nullptr) {
    for (const OwnershipRecord *Record : Sources.Userdata->OwnedValues()) {
      if (Record == nullptr || !RecordIsLive(*Record))
        continue;
      LifecycleUserdataValue Value;
      Value.ClassQualifiedName = ClassNameOf(
          Sources.Classes, Record->ClassSymbol, Record->DynamicType);
      Value.Type = Record->DynamicType;
      Value.Nonce = Record->Identity.Nonce;
      Value.Ownership = Record->Ownership;
      Value.IsPublished = Record->Lifetime == LifetimeState::Published;
      if (Record->HasLiveHandle()) {
        LifecycleRootedReference Rooted;
        Rooted.Subject = Value.Subject();
        Rooted.Detail = "a native lifetime handle roots this userdata";
        Subject.RootedReferences.push_back(std::move(Rooted));
      }
      Subject.LiveUserdata.push_back(std::move(Value));
    }
    std::sort(Subject.LiveUserdata.begin(), Subject.LiveUserdata.end(),
              [](const LifecycleUserdataValue &Left,
                 const LifecycleUserdataValue &Right) {
                if (Left.ClassQualifiedName != Right.ClassQualifiedName)
                  return Left.ClassQualifiedName < Right.ClassQualifiedName;
                return Left.Nonce < Right.Nonce;
              });
    std::sort(Subject.RootedReferences.begin(), Subject.RootedReferences.end(),
              [](const LifecycleRootedReference &Left,
                 const LifecycleRootedReference &Right) {
                return Left.Subject < Right.Subject;
              });
  }

  if (Sources.FrozenCaches != nullptr) {
    const FreezeCacheStorage &Caches = *Sources.FrozenCaches;
    for (const FrozenLookupEntry &Entry : Caches.SortedLookups())
      Subject.Caches.push_back(
          {LifecycleCacheKind::FrozenLookup, Entry.QualifiedName});
    for (const FrozenNamespaceEntry &Entry : Caches.NamespaceCache())
      Subject.Caches.push_back(
          {LifecycleCacheKind::FrozenNamespace, Entry.QualifiedName});
    for (const FrozenModuleEntry &Entry : Caches.ModuleCache())
      Subject.Caches.push_back(
          {LifecycleCacheKind::FrozenModule, Entry.Identity});
    for (const FrozenMetatableEntry &Entry : Caches.MetatableMap())
      Subject.Caches.push_back(
          {LifecycleCacheKind::FrozenMetatable, Entry.QualifiedName});
  }

  if (Sources.LazyValues != nullptr && Sources.LazyValues->EntryCount() > 0)
    Subject.Caches.push_back(
        {LifecycleCacheKind::LazyMemberValue, "<lazy member values>"});

  if (Sources.Identities != nullptr) {
    for (const UserdataCacheEntry &Entry : Sources.Identities->Entries()) {
      if (!Entry.IsActive)
        continue;
      std::string Named =
          ClassNameOf(Sources.Classes, Entry.ClassSymbol, Entry.DynamicType);
      if (Named.empty())
        Named = "<unregistered class>";
      Named.push_back('#');
      Named.append(std::to_string(Entry.Identity.Nonce));
      Subject.Caches.push_back(
          {LifecycleCacheKind::NativeIdentity, std::move(Named)});
    }
  }

  std::sort(
      Subject.Caches.begin(), Subject.Caches.end(),
      [](const LifecycleCacheEntry &Left, const LifecycleCacheEntry &Right) {
        if (Left.Kind != Right.Kind)
          return static_cast<std::uint8_t>(Left.Kind) <
                 static_cast<std::uint8_t>(Right.Kind);
        return Left.Subject < Right.Subject;
      });
  Subject.Caches.erase(std::unique(Subject.Caches.begin(), Subject.Caches.end(),
                                   [](const LifecycleCacheEntry &Left,
                                      const LifecycleCacheEntry &Right) {
                                     return Left.Kind == Right.Kind &&
                                            Left.Subject == Right.Subject;
                                   }),
                       Subject.Caches.end());
  return Subject;
}
} // namespace Luna::Detail
