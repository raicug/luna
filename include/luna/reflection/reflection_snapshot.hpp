#pragma once

// One owning immutable reflection snapshot. Acquiring a snapshot captures
// exactly one committed generation before evaluation, and every lookup,
// enumeration, record, string, and range obtained from it reads only that
// generation. Because a snapshot shares immutable storage instead of pointing
// at mutable State storage, it stays valid and unchanged across later
// registrations, freeze, module replacement, a State move, destruction of the
// originating State, and reads from another thread.

// clang-format off
#include <luna/reflection/ids.hpp>
#include <luna/reflection/reflection_record.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
// clang-format on

namespace Luna {

namespace Detail {
class ReflectionStorage;
class ReflectionDatabase;
} // namespace Detail

// Canonically ordered symbol range over one captured generation. The range
// retains its generation, so indexing it never reads freed storage.
class ReflectionRecordRange final {
public:
  ReflectionRecordRange() noexcept = default;

  [[nodiscard]] std::size_t Size() const noexcept { return CountValue; }
  [[nodiscard]] bool IsEmpty() const noexcept { return CountValue == 0; }
  [[nodiscard]] ReflectionRecord At(std::size_t Index) const;

private:
  friend class Detail::ReflectionStorage;

  ReflectionRecordRange(
      std::shared_ptr<const Detail::ReflectionStorage> Storage,
      const std::size_t *Indices, std::size_t Count) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
  const std::size_t *IndicesValue = nullptr;
  std::size_t CountValue = 0;
};

// Canonically ordered canonical-type range over one captured generation.
class TypeRecordRange final {
public:
  TypeRecordRange() noexcept = default;

  [[nodiscard]] std::size_t Size() const noexcept { return CountValue; }
  [[nodiscard]] bool IsEmpty() const noexcept { return CountValue == 0; }
  [[nodiscard]] TypeRecord At(std::size_t Index) const;

private:
  friend class Detail::ReflectionStorage;

  TypeRecordRange(std::shared_ptr<const Detail::ReflectionStorage> Storage,
                  const std::size_t *Indices, std::size_t Count) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
  const std::size_t *IndicesValue = nullptr;
  std::size_t CountValue = 0;
};

// Canonically ordered module range over one captured generation.
class ModuleRecordRange final {
public:
  ModuleRecordRange() noexcept = default;

  [[nodiscard]] std::size_t Size() const noexcept { return CountValue; }
  [[nodiscard]] bool IsEmpty() const noexcept { return CountValue == 0; }
  [[nodiscard]] ModuleRecord At(std::size_t Index) const;

private:
  friend class Detail::ReflectionStorage;

  ModuleRecordRange(std::shared_ptr<const Detail::ReflectionStorage> Storage,
                    const std::size_t *Indices, std::size_t Count) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
  const std::size_t *IndicesValue = nullptr;
  std::size_t CountValue = 0;
};

class ReflectionSnapshot final {
public:
  // A default snapshot observes one empty generation rather than nothing, so
  // every query on it is still well defined.
  ReflectionSnapshot() noexcept = default;

  // Generation number of the captured reflection model.
  [[nodiscard]] std::uint64_t Generation() const noexcept;
  [[nodiscard]] std::size_t Size() const noexcept;
  [[nodiscard]] bool IsEmpty() const noexcept;

  [[nodiscard]] ReflectionRecord Find(SymbolId Id) const;
  [[nodiscard]] ReflectionRecord Find(std::string_view QualifiedName) const;

  [[nodiscard]] ReflectionRecordRange Symbols() const;
  [[nodiscard]] ReflectionRecordRange Symbols(ScopeId Scope) const;
  [[nodiscard]] ReflectionRecordRange Symbols(SymbolKind Kind) const;
  [[nodiscard]] TypeRecordRange Types() const;
  [[nodiscard]] ModuleRecordRange Modules() const;
  [[nodiscard]] TypeRecord FindType(TypeId Id) const;

private:
  friend class Detail::ReflectionStorage;
  friend class Detail::ReflectionDatabase;

  explicit ReflectionSnapshot(
      std::shared_ptr<const Detail::ReflectionStorage> Storage) noexcept;

  std::shared_ptr<const Detail::ReflectionStorage> StorageValue;
};

} // namespace Luna
