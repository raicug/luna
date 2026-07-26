#pragma once

// The committed half of registration. One generation set is the immutable
// bundle publication swaps atomically: the canonical committed symbol table and
// the immutable reflection generation that describes the same symbols. Because
// a generation set holds no virtual-machine resource, it may be captured by a
// transaction, retained by a reader, and outlive the State that published it.
//
// Committed symbols use the same canonical schema as `DescriptorPlanEntry`, so
// validation reads committed and pending declarations through one view instead
// of maintaining a competing model.

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

// One committed declaration. Every field mirrors the canonical schema of one
// planned declaration, minus the pending payloads publication has consumed.
struct CommittedSymbol final {
  PlanEntryKind Category = PlanEntryKind::Function;
  SymbolDescriptor Symbol;
  SymbolId Identity;
  std::string VmPath;

  [[nodiscard]] bool IsValid() const;
};

// The immutable committed symbol table of one generation. It is built once, in
// canonical order, and never mutated afterwards.
class CommittedSymbolTable final {
public:
  [[nodiscard]] static std::shared_ptr<const CommittedSymbolTable> Empty();

  [[nodiscard]] static std::shared_ptr<const CommittedSymbolTable>
  Build(std::vector<CommittedSymbol> Symbols);

  // The committed symbols of `Current` plus `Added`, in canonical order.
  [[nodiscard]] static std::shared_ptr<const CommittedSymbolTable>
  Extend(const CommittedSymbolTable &Current,
         std::vector<CommittedSymbol> Added);

  [[nodiscard]] std::size_t Size() const noexcept { return Symbols.size(); }
  [[nodiscard]] bool IsEmpty() const noexcept { return Symbols.empty(); }

  // Canonical position access: index 0 is the canonically first symbol.
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

// One immutable generation set. `Generation` advances only through publication.
class GenerationSet final {
public:
  [[nodiscard]] static std::shared_ptr<const GenerationSet> Initial();

  // The successor of `Current`: a later generation number, the given committed
  // symbol table, the reflection generation published with it, and the
  // canonical type generation published with it. A null type generation keeps
  // the one `Current` already observes, so a plan that declares no type cannot
  // advance a generation nothing changed.
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

  // The canonical type generation of this bundle. It is immutable, holds no
  // virtual-machine resource, and is what one invocation captures at entry.
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

// One canonical symbol as seen by validation, whether it is already committed
// or still pending inside the active transaction.
struct SymbolViewEntry final {
  PlanEntryKind Category = PlanEntryKind::Function;
  const SymbolDescriptor *Symbol = nullptr;
  SymbolId Identity;
  std::string_view VmPath;
  bool IsPending = false;
};

// The committed plus pending symbol view of one transaction. Pending entries
// are visible here and nowhere else: an ordinary reflection, dispatch, or VM
// query only ever observes the committed generation set.
class SymbolView final {
public:
  SymbolView(const CommittedSymbolTable &Committed,
             const DescriptorPlan &Pending) noexcept;

  [[nodiscard]] std::optional<SymbolViewEntry>
  Find(std::string_view QualifiedName) const;
  [[nodiscard]] std::optional<SymbolViewEntry>
  Find(const SymbolId &Identity) const;

  // Every symbol of one qualified name, which is more than one exactly when the
  // name owns an overload set.
  [[nodiscard]] std::vector<SymbolViewEntry>
  FindAll(std::string_view QualifiedName) const;

  [[nodiscard]] bool Contains(std::string_view QualifiedName) const;

  [[nodiscard]] std::size_t CommittedCount() const noexcept;
  [[nodiscard]] std::size_t PendingCount() const noexcept;

  // Every declaration the active transaction has planned so far. Validation
  // reads it to compare one new declaration against the pending ones.
  [[nodiscard]] std::span<const DescriptorPlanEntry>
  PendingEntries() const noexcept;

private:
  const CommittedSymbolTable *Committed;
  const DescriptorPlan *Pending;
};

// The committed counterpart of one planned declaration. The pending payloads a
// plan entry carries are consumed by publication, never copied into the
// committed table.
[[nodiscard]] CommittedSymbol
MakeCommittedSymbol(const DescriptorPlanEntry &Entry);

} // namespace Luna::Detail
