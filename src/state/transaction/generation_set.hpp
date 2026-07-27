#pragma once

// clang-format off
#include <luna/reflection/ids.hpp>

#include "state/identity/symbol_descriptor.hpp"
#include "state/reflection/storage.hpp"
#include "state/registration/plan.hpp"
#include "state/type/type_generation.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace Luna::Detail {

struct CommittedSymbol final {
  PlanEntryKind Category = PlanEntryKind::Function;
  SymbolDescriptor Symbol;
  SymbolId Identity;
  std::string VmPath;

  [[nodiscard]] bool IsValid() const;
};

class CommittedSymbolTable final {
public:
  [[nodiscard]] static std::shared_ptr<const CommittedSymbolTable> Empty();

  [[nodiscard]] static std::shared_ptr<const CommittedSymbolTable>
  Build(std::vector<CommittedSymbol> Symbols);

  [[nodiscard]] static std::shared_ptr<const CommittedSymbolTable>
  Extend(const CommittedSymbolTable &Current,
         std::vector<CommittedSymbol> Added);

  [[nodiscard]] std::size_t Size() const noexcept { return Symbols.size(); }
  [[nodiscard]] bool IsEmpty() const noexcept { return Symbols.empty(); }

  [[nodiscard]] const CommittedSymbol *At(std::size_t Index) const noexcept;

  [[nodiscard]] const CommittedSymbol *
  Find(std::string_view QualifiedName) const noexcept;
  [[nodiscard]] const CommittedSymbol *
  Find(const SymbolId &Identity) const noexcept;
  [[nodiscard]] bool Contains(std::string_view QualifiedName) const noexcept;

  [[nodiscard]] std::size_t CountOf(PlanEntryKind Category) const noexcept;

private:
  CommittedSymbolTable() = default;

  std::vector<CommittedSymbol> Symbols;
};

class GenerationSet final {
public:
  [[nodiscard]] static std::shared_ptr<const GenerationSet> Initial();

  [[nodiscard]] static std::shared_ptr<const GenerationSet>
  Derive(const GenerationSet &Current,
         std::shared_ptr<const CommittedSymbolTable> Symbols,
         std::shared_ptr<const ReflectionStorage> Reflection,
         std::shared_ptr<const TypeGeneration> Types = nullptr);

  [[nodiscard]] std::uint64_t Generation() const noexcept {
    return GenerationValue;
  }

  [[nodiscard]] const CommittedSymbolTable &Symbols() const noexcept {
    return *SymbolTable;
  }

  [[nodiscard]] const std::shared_ptr<const CommittedSymbolTable> &
  SharedSymbols() const noexcept {
    return SymbolTable;
  }

  [[nodiscard]] const std::shared_ptr<const ReflectionStorage> &
  Reflection() const noexcept {
    return ReflectionGeneration;
  }

  [[nodiscard]] const std::shared_ptr<const TypeGeneration> &
  Types() const noexcept {
    return TypeGenerationValue;
  }

private:
  GenerationSet() = default;

  std::uint64_t GenerationValue = 0;
  std::shared_ptr<const CommittedSymbolTable> SymbolTable;
  std::shared_ptr<const ReflectionStorage> ReflectionGeneration;
  std::shared_ptr<const TypeGeneration> TypeGenerationValue;
};

struct SymbolViewEntry final {
  PlanEntryKind Category = PlanEntryKind::Function;
  const SymbolDescriptor *Symbol = nullptr;
  SymbolId Identity;
  std::string_view VmPath;
  bool IsPending = false;
};

class SymbolView final {
public:
  SymbolView(const CommittedSymbolTable &Committed,
             const DescriptorPlan &Pending) noexcept;

  [[nodiscard]] std::optional<SymbolViewEntry>
  Find(std::string_view QualifiedName) const;
  [[nodiscard]] std::optional<SymbolViewEntry>
  Find(const SymbolId &Identity) const;

  [[nodiscard]] std::vector<SymbolViewEntry>
  FindAll(std::string_view QualifiedName) const;

  [[nodiscard]] bool Contains(std::string_view QualifiedName) const;

  [[nodiscard]] std::size_t CommittedCount() const noexcept;
  [[nodiscard]] std::size_t PendingCount() const noexcept;

  [[nodiscard]] std::span<const DescriptorPlanEntry>
  PendingEntries() const noexcept;

private:
  const CommittedSymbolTable *Committed;
  const DescriptorPlan *Pending;
};

[[nodiscard]] CommittedSymbol
MakeCommittedSymbol(const DescriptorPlanEntry &Entry);

} // namespace Luna::Detail
