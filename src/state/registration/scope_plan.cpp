// clang-format off
#include "state/registration/scope_plan.hpp"

#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>

#include "state/identity/identity_registry.hpp"
#include "state/identity/symbol_descriptor.hpp"
#include "state/reflection/storage.hpp"
#include "state/registration/plan.hpp"

#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace Luna::Detail {

std::string JoinQualifiedName(std::string_view Parent,
                              std::string_view Segment) {
  if (Parent.empty())
    return std::string(Segment);
  std::string Joined(Parent);
  Joined.push_back(QualifiedNameSeparator);
  Joined.append(Segment);
  return Joined;
}

std::string_view ParentQualifiedName(std::string_view QualifiedName) noexcept {
  const std::size_t Separator = QualifiedName.rfind(QualifiedNameSeparator);
  if (Separator == std::string_view::npos)
    return std::string_view();
  return QualifiedName.substr(0, Separator);
}

std::string_view FinalSegment(std::string_view QualifiedName) noexcept {
  const std::size_t Separator = QualifiedName.rfind(QualifiedNameSeparator);
  if (Separator == std::string_view::npos)
    return QualifiedName;
  return QualifiedName.substr(Separator + 1);
}

DescriptorPlanEntry MakeNamespacePlanEntry(std::string QualifiedName,
                                           SymbolId Parent) {
  DescriptorPlanEntry Entry;
  Entry.Category = PlanEntryKind::Scope;

  Entry.VmPath = QualifiedName;
  Entry.Symbol = MakeScopeSymbol(SymbolKind::Namespace, QualifiedName, Parent);
  if (const auto Identity =
          SymbolIdentityRegistry::ComputeIdentity(Entry.Symbol))
    Entry.Identity = *Identity;

  ReflectionRecordFields Record;
  Record.Kind = SymbolKind::Namespace;
  Record.Id = Entry.Identity;
  Record.Name = std::string(FinalSegment(QualifiedName));
  Record.QualifiedName = std::move(QualifiedName);
  Record.Scope = Parent.IsValid() ? ScopeId(Parent) : ScopeId::Root();
  Record.Declaration = Entry.Identity;
  Record.Returns = ReturnShape::Zero;
  Entry.Record = std::move(Record);
  return Entry;
}

void NamespaceOwnershipTable::Record(NamespaceOwnership Ownership) {
  for (NamespaceOwnership &Existing : Records) {
    if (Existing.QualifiedName == Ownership.QualifiedName) {
      Existing = std::move(Ownership);
      return;
    }
  }
  Records.push_back(std::move(Ownership));
}

const NamespaceOwnership *
NamespaceOwnershipTable::Find(std::string_view QualifiedName) const noexcept {
  for (const NamespaceOwnership &Existing : Records) {
    if (Existing.QualifiedName == QualifiedName)
      return &Existing;
  }
  return nullptr;
}

bool NamespaceOwnershipTable::Matches(const NamespaceOwnership &Ownership,
                                      const StateIdentity &Identity,
                                      std::string_view QualifiedName,
                                      const SymbolId &Scope,
                                      const void *Table) noexcept {
  return Ownership.Identity == Identity &&
         Ownership.QualifiedName == QualifiedName && Ownership.Scope == Scope &&
         Ownership.Table != nullptr && Ownership.Table == Table;
}

bool NamespaceOwnershipTable::IsCurrent(
    const NamespaceOwnership &Ownership,
    std::uint64_t LifecycleGeneration) noexcept {
  return Ownership.LifecycleGeneration == LifecycleGeneration;
}

} // namespace Luna::Detail
