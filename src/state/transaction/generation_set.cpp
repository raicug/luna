// clang-format off
#include "state/transaction/generation_set.hpp"

#include <luna/reflection/ids.hpp>

#include "state/identity/symbol_descriptor.hpp"
#include "state/reflection/storage.hpp"
#include "state/registration/plan.hpp"
#include "state/type/type_generation.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace Luna::Detail {
namespace {

[[nodiscard]] bool CommittedPrecedes(const CommittedSymbol &Left,
                                     const CommittedSymbol &Right) {
  return PlanEntryPrecedes(Left.Symbol, Left.Identity, Right.Symbol,
                           Right.Identity);
}

} // namespace

bool CommittedSymbol::IsValid() const {
  return Symbol.IsValid() && Identity.IsValid();
}

CommittedSymbol MakeCommittedSymbol(const DescriptorPlanEntry &Entry) {
  CommittedSymbol Symbol;
  Symbol.Category = Entry.Category;
  Symbol.Symbol = Entry.Symbol;
  Symbol.Identity = Entry.Identity;
  Symbol.VmPath = Entry.VmPath;
  return Symbol;
}

std::shared_ptr<const CommittedSymbolTable> CommittedSymbolTable::Empty() {
  static const std::shared_ptr<const CommittedSymbolTable> Shared =
      Build(std::vector<CommittedSymbol>());
  return Shared;
}

std::shared_ptr<const CommittedSymbolTable>
CommittedSymbolTable::Build(std::vector<CommittedSymbol> Symbols) {
  std::sort(Symbols.begin(), Symbols.end(), CommittedPrecedes);
  std::shared_ptr<CommittedSymbolTable> Table(new CommittedSymbolTable());
  Table->Symbols = std::move(Symbols);
  return Table;
}

std::shared_ptr<const CommittedSymbolTable>
CommittedSymbolTable::Extend(const CommittedSymbolTable &Current,
                             std::vector<CommittedSymbol> Added) {
  std::vector<CommittedSymbol> Symbols;
  Symbols.reserve(Current.Symbols.size() + Added.size());
  Symbols.insert(Symbols.end(), Current.Symbols.begin(), Current.Symbols.end());
  for (CommittedSymbol &Symbol : Added)
    Symbols.push_back(std::move(Symbol));
  return Build(std::move(Symbols));
}

const CommittedSymbol *
CommittedSymbolTable::At(std::size_t Index) const noexcept {
  return Index < Symbols.size() ? &Symbols[Index] : nullptr;
}

const CommittedSymbol *
CommittedSymbolTable::Find(std::string_view QualifiedName) const noexcept {
  for (const CommittedSymbol &Symbol : Symbols) {
    if (Symbol.Symbol.QualifiedName == QualifiedName)
      return &Symbol;
  }
  return nullptr;
}

const CommittedSymbol *
CommittedSymbolTable::Find(const SymbolId &Identity) const noexcept {
  for (const CommittedSymbol &Symbol : Symbols) {
    if (Symbol.Identity == Identity)
      return &Symbol;
  }
  return nullptr;
}

bool CommittedSymbolTable::Contains(
    std::string_view QualifiedName) const noexcept {
  return Find(QualifiedName) != nullptr;
}

std::size_t
CommittedSymbolTable::CountOf(PlanEntryKind Category) const noexcept {
  std::size_t Result = 0;
  for (const CommittedSymbol &Symbol : Symbols) {
    if (Symbol.Category == Category)
      ++Result;
  }
  return Result;
}

std::shared_ptr<const GenerationSet> GenerationSet::Initial() {
  static const std::shared_ptr<const GenerationSet> Shared = [] {
    std::shared_ptr<GenerationSet> Initial(new GenerationSet());
    Initial->GenerationValue = 0;
    Initial->SymbolTable = CommittedSymbolTable::Empty();
    Initial->ReflectionGeneration = ReflectionStorage::Empty();

    Initial->TypeGenerationValue = TypeGeneration::Foundation();
    return std::shared_ptr<const GenerationSet>(std::move(Initial));
  }();
  return Shared;
}

std::shared_ptr<const GenerationSet>
GenerationSet::Derive(const GenerationSet &Current,
                      std::shared_ptr<const CommittedSymbolTable> Symbols,
                      std::shared_ptr<const ReflectionStorage> Reflection,
                      std::shared_ptr<const TypeGeneration> Types) {
  std::shared_ptr<GenerationSet> Next(new GenerationSet());
  Next->GenerationValue = Current.GenerationValue + 1;
  Next->SymbolTable =
      Symbols ? std::move(Symbols) : CommittedSymbolTable::Empty();
  Next->ReflectionGeneration =
      Reflection ? std::move(Reflection) : ReflectionStorage::Empty();
  if (Types)
    Next->TypeGenerationValue = std::move(Types);
  else
    Next->TypeGenerationValue = Current.TypeGenerationValue
                                    ? Current.TypeGenerationValue
                                    : TypeGeneration::Foundation();
  return Next;
}

SymbolView::SymbolView(const CommittedSymbolTable &Committed,
                       const DescriptorPlan &Pending) noexcept
    : Committed(&Committed), Pending(&Pending) {}

std::optional<SymbolViewEntry>
SymbolView::Find(std::string_view QualifiedName) const {
  if (const DescriptorPlanEntry *Entry = Pending->Find(QualifiedName))
    return SymbolViewEntry{Entry->Category, &Entry->Symbol, Entry->Identity,
                           Entry->VmPath, true};
  if (const CommittedSymbol *Symbol = Committed->Find(QualifiedName))
    return SymbolViewEntry{Symbol->Category, &Symbol->Symbol, Symbol->Identity,
                           Symbol->VmPath, false};
  return std::nullopt;
}

std::optional<SymbolViewEntry>
SymbolView::Find(const SymbolId &Identity) const {
  if (const DescriptorPlanEntry *Entry = Pending->Find(Identity))
    return SymbolViewEntry{Entry->Category, &Entry->Symbol, Entry->Identity,
                           Entry->VmPath, true};
  if (const CommittedSymbol *Symbol = Committed->Find(Identity))
    return SymbolViewEntry{Symbol->Category, &Symbol->Symbol, Symbol->Identity,
                           Symbol->VmPath, false};
  return std::nullopt;
}

std::vector<SymbolViewEntry>
SymbolView::FindAll(std::string_view QualifiedName) const {
  std::vector<SymbolViewEntry> Found;
  for (const DescriptorPlanEntry &Entry : Pending->PlannedEntries()) {
    if (Entry.Symbol.QualifiedName == QualifiedName)
      Found.push_back(SymbolViewEntry{Entry.Category, &Entry.Symbol,
                                      Entry.Identity, Entry.VmPath, true});
  }
  for (std::size_t Index = 0; Index < Committed->Size(); ++Index) {
    const CommittedSymbol *Symbol = Committed->At(Index);
    if (Symbol && Symbol->Symbol.QualifiedName == QualifiedName)
      Found.push_back(SymbolViewEntry{Symbol->Category, &Symbol->Symbol,
                                      Symbol->Identity, Symbol->VmPath, false});
  }
  return Found;
}

bool SymbolView::Contains(std::string_view QualifiedName) const {
  return Find(QualifiedName).has_value();
}

std::size_t SymbolView::CommittedCount() const noexcept {
  return Committed->Size();
}

std::size_t SymbolView::PendingCount() const noexcept {
  return Pending->Size();
}

std::span<const DescriptorPlanEntry>
SymbolView::PendingEntries() const noexcept {
  return Pending->PlannedEntries();
}

} // namespace Luna::Detail
