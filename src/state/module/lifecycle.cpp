// clang-format off
#include "state/module/lifecycle.hpp"

#include "state/dispatch/generation.hpp"
#include "state/freeze/cache.hpp"
#include "state/module/registry.hpp"
#include "state/reflection/storage.hpp"
#include "state/userdata/class_registry.hpp"
#include "state/userdata/lazy_cache.hpp"
#include "state/userdata/ownership.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
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

} // namespace Luna::Detail
